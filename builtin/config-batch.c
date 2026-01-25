#define USE_THE_REPOSITORY_VARIABLE
#include "builtin.h"
#include "config.h"
#include "environment.h"
#include "parse-options.h"
#include "strbuf.h"
#include "string-list.h"

static const char *const builtin_config_batch_usage[] = {
	N_("git config-batch <options>"),
	NULL
};

static int zformat = 0;

#define UNKNOWN_COMMAND "unknown_command"
#define HELP_COMMAND "help"
#define GET_COMMAND "get"
#define SET_COMMAND "set"
#define COMMAND_PARSE_ERROR "command_parse_error"

static void print_word(const char *word, int start)
{
	if (zformat) {
		printf("%"PRIu32":%s", (uint32_t)strlen(word), word);
		fputc(0, stdout);
	} else if (start)
		printf("%s", word);
	else
		printf(" %s", word);
}

static int emit_response(const char *response, ...)
{
	va_list params;
	const char *token;

	print_word(response, 1);

	va_start(params, response);
	while ((token = va_arg(params, const char *)))
		print_word(token, 0);
	va_end(params);

	if (zformat)
		fputc(0, stdout);
	else
		printf("\n");
	fflush(stdout);
	return 0;
}

static int command_parse_error(const char *command)
{
	return emit_response(COMMAND_PARSE_ERROR, command, NULL);
}

/**
 * A function pointer type for defining a command. The function is
 * responsible for handling different versions of the command name.
 *
 * Provides the remaining 'data' for the command, to be parsed by
 * the function as needed according to its parsing rules.
 *
 * These functions should only return a negative value if they result
 * in such a catastrophic failure that the process should end.
 *
 * Return 0 on success.
 */
typedef int (*command_fn)(struct repository *repo,
			  const char *prefix,
			  char *data, size_t data_len);

static int unknown_command(struct repository *repo UNUSED,
			   const char *prefix UNUSED,
			   char *data UNUSED, size_t data_len UNUSED)
{
	return emit_response(UNKNOWN_COMMAND, NULL);
}

/*
 * Parse the next token using the NUL-byte format.
 */
static size_t parse_ztoken(char **data, size_t *data_len,
			   char **token, int *err)
{
	size_t i = 0, token_len;

	while (i < *data_len && (*data)[i] != ':') {
		if ((*data)[i] < '0' || (*data)[i] > '9') {
			goto parse_error;
		}
		i++;
	}

	if (i >= *data_len || (*data)[i] != ':' || i > 5)
		goto parse_error;

	(*data)[i] = 0;
	token_len = atoi(*data);

	if (token_len + i + 1 >= *data_len)
		goto parse_error;

	*token = *data + i + 1;
	*data_len = *data_len - (i + 1);

	/* check for early NULs. */
	for (i = 0; i < token_len; i++) {
		if (!(*token)[i])
			goto parse_error;
	}
	/* check for matching NUL. */
	if ((*token)[token_len])
		goto parse_error;

	*data = *token + token_len + 1;
	*data_len = *data_len - (token_len + 1);
	return token_len;

parse_error:
	*err = 1;
	*token = NULL;
	return 0;
}

static size_t parse_whitespace_token(char **data, size_t *data_len,
				     char **token, int *err UNUSED)
{
	size_t i = 0;

	*token = *data;

	while (i < *data_len && (*data)[i] && (*data)[i] != ' ')
		i++;

	if (i >= *data_len) {
		*data_len = 0;
		*data = NULL;
		return i;
	}

	(*data)[i] = 0;
	*data_len = (*data_len) - (i + 1);
	*data = *data + (i + 1);
	return i;
}

/**
 * Given the remaining data line and its size, attempt to extract
 * a token. When the token delimiter is determined, the data
 * string is mutated to insert a NUL byte at the end of the token.
 * The data pointer is mutated to point at the next character (or
 * set to NULL if that exceeds the string length). The data_len
 * value is mutated to subtract the length of the discovered
 * token.
 *
 * The returned value is the length of the token that was
 * discovered.
 *
 * The 'token' pointer is used to set the start of the token.
 * In the whitespace format, this is always the input value of
 * 'data' but in the NUL-terminated format this follows an "<N>:"
 * prefix.
 *
 * In the case of the NUL-terminated format, a bad parse of the
 * decimal length or a mismatch of the decimal length and the
 * length of the following NUL-terminated string will result in
 * the value pointed at by 'err' to be set to 1.
 */
static size_t parse_token(char **data, size_t *data_len,
			  char **token, int *err)
{
	if (!*data_len)
		return 0;
	if (zformat)
		return parse_ztoken(data, data_len, token, err);
	return parse_whitespace_token(data, data_len, token, err);
}

static int help_command_1(struct repository *repo,
			  const char *prefix UNUSED,
			  char *data, size_t data_len);

enum value_match_mode {
	MATCH_ALL,
	MATCH_EXACT,
	MATCH_REGEX,
};

struct get_command_1_data {
	/* parameters */
	char *key;
	enum config_scope scope;
	enum value_match_mode mode;

	/* optional parameters */
	char *value;
	regex_t *value_pattern;

	/* data along the way, for single values. */
	char *found;
	enum config_scope found_scope;
};

static int get_command_1_cb(const char *key, const char *value,
			    const struct config_context *context,
			    void *data)
{
	struct get_command_1_data *d = data;

	if (strcasecmp(key, d->key))
		return 0;

	if (d->scope != CONFIG_SCOPE_UNKNOWN &&
	    d->scope != context->kvi->scope)
		return 0;

	switch (d->mode) {
	case MATCH_EXACT:
		if (strcasecmp(value, d->value))
			return 0;
		break;

	case MATCH_REGEX:
		if (regexec(d->value_pattern, value, 0, NULL, 0))
			return 0;
		break;

	default:
		break;
	}

	free(d->found);
	d->found = xstrdup(value);
	d->found_scope = context->kvi->scope;
	return 0;
}

static const char *scope_str(enum config_scope scope)
{
	switch (scope) {
	case CONFIG_SCOPE_UNKNOWN:
		return "unknown";

	case CONFIG_SCOPE_SYSTEM:
		return "system";

	case CONFIG_SCOPE_GLOBAL:
		return "global";

	case CONFIG_SCOPE_LOCAL:
		return "local";

	case CONFIG_SCOPE_WORKTREE:
		return "worktree";

	case CONFIG_SCOPE_SUBMODULE:
		return "submodule";

	case CONFIG_SCOPE_COMMAND:
		return "command";

	default:
		BUG("invalid config scope");
	}
}

static int parse_scope(const char *str, enum config_scope *scope)
{
	if (!strcmp(str, "inherited")) {
		*scope = CONFIG_SCOPE_UNKNOWN;
		return 0;
	}

	for (enum config_scope s = 0; s < CONFIG_SCOPE__NR; s++) {
		if (!strcmp(str, scope_str(s))) {
			*scope = s;
			return 0;
		}
	}

	return -1;
}

/**
 * 'get' command, version 1.
 *
 * Positional arguments should be of the form:
 *
 * [0] scope ("system", "global", "local", "worktree", "command", "submodule", or "inherited")
 * [1] config key
 * [2*] multi-mode ("regex", "fixed-value")
 * [3*] value regex OR value string
 *
 * [N*] indicates optional parameters that are not needed.
 */
static int get_command_1(struct repository *repo,
			 const char *prefix UNUSED,
			 char *data,
			 size_t data_len)
{
	struct get_command_1_data gc_data = {
		.found = NULL,
		.mode = MATCH_ALL,
	};
	int res = 0, err = 0;
	char *token;
	size_t token_len;

	if (!parse_token(&data, &data_len, &token, &err) || err)
		goto parse_error;

	if (parse_scope(token, &gc_data.scope))
		goto parse_error;

	if (!parse_token(&data, &data_len, &gc_data.key, &err) || err)
		goto parse_error;

	token_len = parse_token(&data, &data_len, &token, &err);
	if (err)
		goto parse_error;

	if (token_len && !strncmp(token, "arg:", 4)) {
		if (!strcmp(token + 4, "regex"))
			gc_data.mode = MATCH_REGEX;
		else if (!strcmp(token + 4, "fixed-value"))
			gc_data.mode = MATCH_EXACT;
		else
			goto parse_error; /* unknown arg. */

		/* Use the remaining data as the value string. */
		if (!zformat)
			gc_data.value = data;
		else {
			parse_token(&data, &data_len, &gc_data.value, &err);
			if (err)
				goto parse_error;
		}

		if (gc_data.mode == MATCH_REGEX) {
			CALLOC_ARRAY(gc_data.value_pattern, 1);
			if (regcomp(gc_data.value_pattern, gc_data.value,
				    REG_EXTENDED)) {
				FREE_AND_NULL(gc_data.value_pattern);
				goto parse_error;
			}
		}
	} else if (token_len) {
		/*
		 * If we have remaining tokens not starting in "arg:",
		 * then we don't understand them.
		 */
		goto parse_error;
	}

	repo_config(repo, get_command_1_cb, &gc_data);

	if (gc_data.found)
		res = emit_response(GET_COMMAND, "1", "found", gc_data.key,
				    scope_str(gc_data.found_scope),
				    gc_data.found,
				    NULL);
	else
		res = emit_response(GET_COMMAND, "1", "missing", gc_data.key,
				    gc_data.value, NULL);

	goto cleanup;


parse_error:
	res = command_parse_error(GET_COMMAND);

cleanup:
	if (gc_data.value_pattern) {
		regfree(gc_data.value_pattern);
		free(gc_data.value_pattern);
	}
	free(gc_data.found);
	return res;
}


/**
 * 'set' command, version 1.
 *
 * Positional arguments should be of the form:
 *
 * [0] scope ("system", "global", "local", or "worktree")
 * [1] config key
 * [2] config value
 */
static int set_command_1(struct repository *repo,
			 const char *prefix,
			 char *data,
			 size_t data_len)
{
	int res = 0, err = 0;
	enum config_scope scope = CONFIG_SCOPE_UNKNOWN;
	char *token = NULL, *key = NULL, *value = NULL;
	struct config_location_options locopts = CONFIG_LOCATION_OPTIONS_INIT;

	if (!parse_token(&data, &data_len, &token, &err) || err)
		goto parse_error;

	if (parse_scope(token, &scope) ||
	    scope == CONFIG_SCOPE_UNKNOWN ||
	    scope == CONFIG_SCOPE_SUBMODULE ||
	    scope == CONFIG_SCOPE_COMMAND)
		goto parse_error;

	if (!parse_token(&data, &data_len, &key, &err) || err)
		goto parse_error;

	/* Use the remaining data as the value string. */
	if (!zformat)
		value = data;
	else {
		parse_token(&data, &data_len, &value, &err);
		if (err)
			goto parse_error;
	}

	if (location_options_set_scope(&locopts, scope))
		goto parse_error;
	location_options_init(repo, &locopts, prefix);

	res = repo_config_set_in_file_gently(repo, locopts.source.file,
					     key, NULL, value);

	if (res)
		res = emit_response(SET_COMMAND, "1", "failure",
				    scope_str(scope), key, value, NULL);
	else
		res = emit_response(SET_COMMAND, "1", "success",
				    scope_str(scope), key, value, NULL);

	goto cleanup;

parse_error:
	res = command_parse_error(SET_COMMAND);

cleanup:
	location_options_release(&locopts);
	return res;
}

struct command {
	const char *name;
	command_fn fn;
	int version;
};

static struct command commands[] = {
	{
		.name = HELP_COMMAND,
		.fn = help_command_1,
		.version = 1,
	},
	{
		.name = GET_COMMAND,
		.fn = get_command_1,
		.version = 1,
	},
	{
		.name = SET_COMMAND,
		.fn = set_command_1,
		.version = 1,
	},
	/* unknown_command must be last. */
	{
		.name = "",
		.fn   = unknown_command,
	},
};

#define COMMAND_COUNT ((size_t)(sizeof(commands) / sizeof(*commands)))

static int help_command_1(struct repository *repo UNUSED,
			  const char *prefix UNUSED,
			  char *data UNUSED, size_t data_len UNUSED)
{
	struct strbuf fmt_str = STRBUF_INIT;

	strbuf_addf(&fmt_str, "%"PRIu32, (uint32_t)(COMMAND_COUNT - 1));
	emit_response(HELP_COMMAND, "1", "count", fmt_str.buf, NULL);
	strbuf_reset(&fmt_str);

	for (size_t i = 0; i < COMMAND_COUNT; i++) {
		/* Halt at unknown command. */
		if (!commands[i].name[0])
			break;

		strbuf_addf(&fmt_str, "%d", commands[i].version);
		emit_response(HELP_COMMAND, "1", commands[i].name, fmt_str.buf, NULL);
		strbuf_reset(&fmt_str);
	}

	strbuf_release(&fmt_str);
	return 0;
}

static int process_command_nul(struct repository *repo,
			       const char *prefix)
{
	static struct strbuf line = STRBUF_INIT;
	char *data, *command, *versionstr;
	size_t data_len, token_len;
	int res = 0, err = 0, version = 0, getc;
	char c;

	/* If we start with EOF it's not an error. */
	getc = fgetc(stdin);
	if (getc == EOF)
		return 1;

	do {
		c = (char)getc;
		strbuf_addch(&line, c);

		if (!c && line.len > 1 && !line.buf[line.len - 2])
			break;

		getc = fgetc(stdin);

		/* It's an error if we reach EOF while parsing a command. */
		if (getc == EOF)
			goto parse_error;
	} while (1);

	data = line.buf;
	data_len = line.len - 1;

	token_len = parse_ztoken(&data, &data_len, &command, &err);
	if (!token_len || err)
		goto parse_error;

	token_len = parse_ztoken(&data, &data_len, &versionstr, &err);
	if (!token_len || err)
		goto parse_error;

	if (!git_parse_int(versionstr, &version)) {
		res = error(_("unable to parse '%s' to integer"),
			    versionstr);
		goto parse_error;
	}

	for (size_t i = 0; i < COMMAND_COUNT; i++) {
		/*
		 * Run the ith command if we have hit the unknown
		 * command or if the name and version match.
		 */
		if (!commands[i].name[0] ||
		    (!strcmp(command, commands[i].name) &&
		     commands[i].version == version)) {
			res = commands[i].fn(repo, prefix, data, data_len);
			goto cleanup;
		}
	}

	BUG(_("scanned to end of command list, including 'unknown_command'"));

parse_error:
	res = unknown_command(repo, prefix, NULL, 0);

cleanup:
	strbuf_release(&line);
	return res;
}

static int process_command_whitespace(struct repository *repo,
				      const char *prefix)
{
	static struct strbuf line = STRBUF_INIT;
	struct string_list tokens = STRING_LIST_INIT_NODUP;
	const char *command;
	int version;
	char *data = NULL;
	size_t data_len = 0;
	int res = 0;

	strbuf_getline(&line, stdin);

	if (!line.len)
		return 1;

	/* Parse out the first two tokens, command and version. */
	string_list_split_in_place(&tokens, line.buf, " ", 2);

	if (tokens.nr < 2) {
		res = error(_("expected at least 2 tokens, got %"PRIu32),
			    (uint32_t)tokens.nr);
		goto cleanup;
	}

	command = tokens.items[0].string;

	if (!git_parse_int(tokens.items[1].string, &version)) {
		res = error(_("unable to parse '%s' to integer"),
			    tokens.items[1].string);
		goto cleanup;
	}

	if (tokens.nr >= 3) {
		data = tokens.items[2].string;
		data_len = strlen(tokens.items[2].string);
	}

	for (size_t i = 0; i < COMMAND_COUNT; i++) {
		/*
		 * Run the ith command if we have hit the unknown
		 * command or if the name and version match.
		 */
		if (!commands[i].name[0] ||
		    (!strcmp(command, commands[i].name) &&
		     commands[i].version == version)) {
			res = commands[i].fn(repo, prefix, data, data_len);
			goto cleanup;
		}
	}

	BUG(_("scanned to end of command list, including 'unknown_command'"));

cleanup:
	strbuf_reset(&line);
	string_list_clear(&tokens, 0);
	return res;
}

/**
 * Process a single line from stdin and process the command.
 *
 * Returns 0 on successful processing of command, including the
 * unknown_command output.
 *
 * Returns 1 on natural exit due to exist signal of empty line.
 *
 * Returns negative value on other catastrophic error.
 */
static int process_command(struct repository *repo,
			   const char *prefix)
{
	if (zformat)
		return process_command_nul(repo, prefix);
	return process_command_whitespace(repo, prefix);
}

int cmd_config_batch(int argc,
		     const char **argv,
		     const char *prefix,
		     struct repository *repo)
{
	int res = 0;
	struct option options[] = {
		OPT_BOOL('z', NULL, &zformat,
			 N_("stdin and stdout is NUL-terminated")),
		OPT_END(),
	};

	show_usage_with_options_if_asked(argc, argv,
					 builtin_config_batch_usage, options);

	argc = parse_options(argc, argv, prefix, options, builtin_config_batch_usage,
			     0);

	repo_config(repo, git_default_config, NULL);

	while (!(res = process_command(repo, prefix)));

	if (res == 1)
		return 0;
	die(_("an unrecoverable error occurred during command execution"));
}
