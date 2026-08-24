#define USE_THE_REPOSITORY_VARIABLE

#include "builtin.h"
#include "config.h"
#include "gettext.h"
#include "gpg-interface.h"
#include "ident.h"
#include "parse-options.h"
#include "strbuf.h"

static const char * const ident_usage[] = {
	N_("git ident [<options>]"),
	NULL
};

enum ident_who {
	IDENT_AUTHOR = 1 << 0,
	IDENT_COMMITTER = 1 << 1,
};

enum ident_part {
	IDENT_NAME = 1 << 0,
	IDENT_EMAIL = 1 << 1,
};

struct ident_config {
	int gpgsign;
	char *signing_key;
	char *gpg_format;
	char *ssh_default_key_cmd;
};

static int ident_config_cb(const char *var, const char *value,
			   const struct config_context *ctx, void *data)
{
	struct ident_config *cfg = data;

	if (!strcmp(var, "commit.gpgsign")) {
		cfg->gpgsign = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "user.signingkey"))
		return git_config_string(&cfg->signing_key, var, value);
	if (!strcmp(var, "gpg.format"))
		return git_config_string(&cfg->gpg_format, var, value);
	if (!strcmp(var, "gpg.ssh.defaultkeycommand"))
		return git_config_string(&cfg->ssh_default_key_cmd, var, value);

	return git_default_config(var, value, ctx, data);
}

static void print_ident_entry(const char *label,
			      const struct strbuf *name,
			      const struct strbuf *email,
			      int parts,
			      int verbose,
			      char eol)
{
	struct strbuf out = STRBUF_INIT;

	if (verbose && label)
		strbuf_addf(&out, "%s: ", label);

	if ((parts & IDENT_NAME) && (parts & IDENT_EMAIL))
		strbuf_addf(&out, "%s <%s>", name->buf, email->buf);
	else if (parts & IDENT_NAME)
		strbuf_addbuf(&out, name);
	else if (parts & IDENT_EMAIL)
		strbuf_addf(&out, "<%s>", email->buf);

	printf("%s%c", out.buf, eol);
	strbuf_release(&out);
}

int cmd_ident(int argc,
	      const char **argv,
	      const char *prefix,
	      struct repository *repo)
{
	int show_author = 0;
	int show_committer = 0;
	int show_name = 0;
	int show_email = 0;
	int show_signing_key = 0;
	int porcelain = 0;
	int nul_term = 0;
	int verbose = 0;
	int ret = 0;
	char eol;
	int selected_who = 0;
	int selected_parts = 0;

	struct option ident_options[] = {
		OPT_BOOL('a', "author", &show_author, N_("show author identity")),
		OPT_BOOL('c', "committer", &show_committer, N_("show committer identity")),
		OPT_BOOL('n', "name", &show_name, N_("show name only")),
		OPT_BOOL('e', "email", &show_email, N_("show email only")),
		OPT_BOOL('s', "signing-key", &show_signing_key, N_("show commit signing key")),
		OPT_BOOL(0, "porcelain", &porcelain, N_("machine-readable output")),
		OPT_BOOL('z', "null", &nul_term, N_("terminate entries with NUL")),
		OPT__VERBOSE(&verbose, N_("show detailed identity")),
		OPT_END()
	};

	struct ident_config cfg = { 0 };
	struct strbuf author_info = STRBUF_INIT;
	struct strbuf committer_info = STRBUF_INIT;
	struct ident_split author_split, committer_split;
	struct strbuf author_name = STRBUF_INIT;
	struct strbuf author_email = STRBUF_INIT;
	struct strbuf committer_name = STRBUF_INIT;
	struct strbuf committer_email = STRBUF_INIT;
	char *resolved_key = NULL;
	int is_ssh = 0;

	argc = parse_options(argc, argv, prefix, ident_options,
			     ident_usage, 0);

	if (argc > 0)
		usage_with_options(ident_usage, ident_options);

	die_for_incompatible_opt2(porcelain, "--porcelain", verbose, "--verbose");
	die_for_incompatible_opt2(porcelain, "--porcelain", show_name, "--name");
	die_for_incompatible_opt2(porcelain, "--porcelain", show_email, "--email");

	eol = nul_term ? '\0' : '\n';

	repo_config(repo, ident_config_cb, &cfg);

	strbuf_addstr(&author_info, git_author_info(IDENT_NO_DATE));
	strbuf_addstr(&committer_info, git_committer_info(IDENT_NO_DATE));

	if (split_ident_line(&author_split, author_info.buf, author_info.len) == 0) {
		if (author_split.name_begin && author_split.name_end)
			strbuf_add(&author_name, author_split.name_begin,
				   author_split.name_end - author_split.name_begin);
		if (author_split.mail_begin && author_split.mail_end)
			strbuf_add(&author_email, author_split.mail_begin,
				   author_split.mail_end - author_split.mail_begin);
	}

	if (split_ident_line(&committer_split, committer_info.buf, committer_info.len) == 0) {
		if (committer_split.name_begin && committer_split.name_end)
			strbuf_add(&committer_name, committer_split.name_begin,
				   committer_split.name_end - committer_split.name_begin);
		if (committer_split.mail_begin && committer_split.mail_end)
			strbuf_add(&committer_email, committer_split.mail_begin,
				   committer_split.mail_end - committer_split.mail_begin);
	}

	is_ssh = cfg.gpg_format && !strcmp(cfg.gpg_format, "ssh");

	if (cfg.signing_key && *cfg.signing_key) {
		resolved_key = xstrdup(cfg.signing_key);
	} else if (is_ssh) {
		if (cfg.ssh_default_key_cmd && *cfg.ssh_default_key_cmd)
			resolved_key = get_signing_key_id();
	} else if (cfg.gpgsign) {
		resolved_key = get_signing_key_id();
	}

	if (show_signing_key && !show_author && !show_committer && !show_name && !show_email && !porcelain) {
		if (resolved_key && *resolved_key) {
			if (verbose)
				printf(_("Signing Key: %s\n"), resolved_key);
			else
				printf("%s%c", resolved_key, eol);
			ret = 0;
		} else {
			ret = 1;
		}
		goto cleanup;
	}

	if (show_author)
		selected_who |= IDENT_AUTHOR;
	if (show_committer)
		selected_who |= IDENT_COMMITTER;
	if (!selected_who)
		selected_who = IDENT_AUTHOR | IDENT_COMMITTER;

	if (show_name)
		selected_parts |= IDENT_NAME;
	if (show_email)
		selected_parts |= IDENT_EMAIL;
	if (!selected_parts)
		selected_parts = IDENT_NAME | IDENT_EMAIL;

	if (porcelain) {
		if (selected_who & IDENT_AUTHOR) {
			printf("user.author.name=%s%c", author_name.buf, eol);
			printf("user.author.email=%s%c", author_email.buf, eol);
		}
		if (selected_who & IDENT_COMMITTER) {
			printf("user.committer.name=%s%c", committer_name.buf, eol);
			printf("user.committer.email=%s%c", committer_email.buf, eol);
		}
		if ((selected_who & (IDENT_AUTHOR | IDENT_COMMITTER)) == (IDENT_AUTHOR | IDENT_COMMITTER)) {
			printf("user.signingkey=%s%c",
			       (cfg.signing_key && *cfg.signing_key) ? cfg.signing_key :
			       (resolved_key && *resolved_key) ? resolved_key : "none",
			       eol);
			printf("gpg.format=%s%c",
			       cfg.gpg_format ? cfg.gpg_format : "openpgp", eol);
			printf("commit.gpgsign=%s%c",
			       cfg.gpgsign ? "true" : "false", eol);
		}
		goto cleanup;
	}

	if (selected_who & IDENT_AUTHOR)
		print_ident_entry("Author", &author_name, &author_email,
				  selected_parts, verbose, eol);

	if (selected_who & IDENT_COMMITTER)
		print_ident_entry("Committer", &committer_name, &committer_email,
				  selected_parts, verbose, eol);

	if (show_signing_key) {
		if (resolved_key && *resolved_key) {
			if (verbose)
				printf(_("Signing Key: %s\n"), resolved_key);
			else
				printf("%s%c", resolved_key, eol);
		} else {
			ret = 1;
		}
	}

cleanup:
	free(cfg.signing_key);
	free(cfg.gpg_format);
	free(cfg.ssh_default_key_cmd);
	free(resolved_key);
	strbuf_release(&author_info);
	strbuf_release(&committer_info);
	strbuf_release(&author_name);
	strbuf_release(&author_email);
	strbuf_release(&committer_name);
	strbuf_release(&committer_email);

	return ret;
}
