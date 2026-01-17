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

#define UNKNOWN_COMMAND "unknown_command"

static int emit_response(const char *response, ...)
{
	va_list params;
	const char *token;

	printf("%s", response);

	va_start(params, response);
	while ((token = va_arg(params, const char *)))
		printf(" %s", token);
	va_end(params);

	printf("\n");
	fflush(stdout);
	return 0;
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
			  char *data, size_t data_len);

static int unknown_command(struct repository *repo UNUSED,
			  char *data UNUSED, size_t data_len UNUSED)
{
	return emit_response(UNKNOWN_COMMAND, NULL);
}

struct command {
	const char *name;
	command_fn fn;
	int version;
};

static struct command commands[] = {
	/* unknown_command must be last. */
	{
		.name = "",
		.fn   = unknown_command,
	},
};

#define COMMAND_COUNT ((size_t)(sizeof(commands) / sizeof(*commands)))

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
static int process_command(struct repository *repo)
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
			res = commands[i].fn(repo, data, data_len);
			goto cleanup;
		}
	}

	BUG(_("scanned to end of command list, including 'unknown_command'"));

cleanup:
	strbuf_reset(&line);
	string_list_clear(&tokens, 0);
	return res;
}

int cmd_config_batch(int argc,
		     const char **argv,
		     const char *prefix,
		     struct repository *repo)
{
	int res = 0;
	struct option options[] = {
		OPT_END(),
	};

	show_usage_with_options_if_asked(argc, argv,
					 builtin_config_batch_usage, options);

	argc = parse_options(argc, argv, prefix, options, builtin_config_batch_usage,
			     0);

	repo_config(repo, git_default_config, NULL);

	while (!(res = process_command(repo)));

	if (res == 1)
		return 0;
	die(_("an unrecoverable error occurred during command execution"));
}
