#define USE_THE_REPOSITORY_VARIABLE

#include "builtin.h"
#include "config.h"
#include "gettext.h"
#include "gpg-interface.h"
#include "ident.h"
#include "parse-options.h"
#include "strbuf.h"

static const char * const whoami_usage[] = {
	N_("git whoami [options]"),
	NULL
};

int cmd_whoami(int argc,
	       const char **argv,
	       const char *prefix,
	       struct repository *repo UNUSED)
{
	int show_author = 0;
	int show_committer = 0;
	int show_name = 0;
	int show_email = 0;
	int show_signing_key = 0;
	int verbose = 0;
	int ret = 0;

	struct option whoami_options[] = {
		OPT_BOOL('a', "author", &show_author, N_("show author identity")),
		OPT_BOOL('c', "committer", &show_committer, N_("show committer identity")),
		OPT_BOOL('n', "name", &show_name, N_("show name only")),
		OPT_BOOL('e', "email", &show_email, N_("show email only")),
		OPT_BOOL('s', "signing-key", &show_signing_key, N_("show commit signing key")),
		OPT__VERBOSE(&verbose, N_("show detailed identity and signing status")),
		OPT_END()
	};

	struct strbuf author_info = STRBUF_INIT;
	struct strbuf committer_info = STRBUF_INIT;
	struct ident_split author_split, committer_split;
	struct strbuf author_name = STRBUF_INIT;
	struct strbuf author_email = STRBUF_INIT;
	struct strbuf committer_name = STRBUF_INIT;
	struct strbuf committer_email = STRBUF_INIT;

	char *signing_key = NULL;
	char *gpg_format = NULL;
	char *ssh_default_key_cmd = NULL;
	char *resolved_key = NULL;
	int gpgsign = 0;
	int is_ssh = 0;

	argc = parse_options(argc, argv, prefix, whoami_options,
			     whoami_usage, 0);

	if (argc > 0)
		usage_with_options(whoami_usage, whoami_options);

	die_for_incompatible_opt2(show_author, "--author", show_committer, "--committer");
	die_for_incompatible_opt2(show_name, "--name", show_email, "--email");
	die_for_incompatible_opt2(show_signing_key, "--signing-key", show_name, "--name");
	die_for_incompatible_opt2(show_signing_key, "--signing-key", show_email, "--email");
	die_for_incompatible_opt2(show_signing_key, "--signing-key", show_author, "--author");
	die_for_incompatible_opt2(show_signing_key, "--signing-key", show_committer, "--committer");
	die_for_incompatible_opt2(show_signing_key, "--signing-key", verbose, "--verbose");
	die_for_incompatible_opt2(verbose, "--verbose", show_name, "--name");
	die_for_incompatible_opt2(verbose, "--verbose", show_email, "--email");
	die_for_incompatible_opt2(verbose, "--verbose", show_author, "--author");
	die_for_incompatible_opt2(verbose, "--verbose", show_committer, "--committer");

	repo_config(the_repository, git_default_config, NULL);

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

	repo_config_get_bool(the_repository, "commit.gpgsign", &gpgsign);
	repo_config_get_string(the_repository, "user.signingkey", &signing_key);
	repo_config_get_string(the_repository, "gpg.format", &gpg_format);
	repo_config_get_string(the_repository, "gpg.ssh.defaultkeycommand", &ssh_default_key_cmd);

	is_ssh = gpg_format && !strcmp(gpg_format, "ssh");

	if (signing_key && *signing_key) {
		resolved_key = xstrdup(signing_key);
	} else if (is_ssh) {
		if (ssh_default_key_cmd && *ssh_default_key_cmd)
			resolved_key = get_signing_key_id();
	} else if (gpgsign) {
		resolved_key = get_signing_key_id();
	}

	if (show_signing_key) {
		if (resolved_key && *resolved_key) {
			puts(resolved_key);
			ret = 0;
		} else {
			ret = 1;
		}
		goto cleanup;
	}

	if (show_name) {
		if (show_author)
			puts(author_name.buf);
		else
			puts(committer_name.buf);
		goto cleanup;
	}

	if (show_email) {
		if (show_author)
			puts(author_email.buf);
		else
			puts(committer_email.buf);
		goto cleanup;
	}

	if (show_author) {
		puts(author_info.buf);
		goto cleanup;
	}

	if (show_committer) {
		puts(committer_info.buf);
		goto cleanup;
	}

	if (verbose) {
		printf(_("Author Name:      %s\n"), author_name.buf);
		printf(_("Author Email:     %s\n"), author_email.buf);
		printf(_("Committer Name:   %s\n"), committer_name.buf);
		printf(_("Committer Email:  %s\n"), committer_email.buf);
		if (signing_key && *signing_key)
			printf(_("Signing Key:      %s\n"), signing_key);
		else if (resolved_key && *resolved_key)
			printf(_("Signing Key:      %s (default fallback)\n"), resolved_key);
		else
			printf(_("Signing Key:      %s\n"), _("none"));
		printf(_("Signing Format:   %s\n"),
		       gpg_format ? gpg_format : "openpgp");
		printf(_("GPG Signing:      %s\n"),
		       gpgsign ? _("enabled") : _("disabled"));
	} else {
		printf(_("Author:    %s\n"), author_info.buf);
		printf(_("Committer: %s\n"), committer_info.buf);
		if (gpgsign) {
			if (signing_key && *signing_key) {
				printf(_("Signing:   %s (format: %s, commit.gpgsign: true)\n"),
				       signing_key,
				       gpg_format ? gpg_format : "openpgp");
			} else if (resolved_key && *resolved_key) {
				printf(_("Signing:   default key (%s) (format: %s, commit.gpgsign: true)\n"),
				       resolved_key,
				       gpg_format ? gpg_format : "openpgp");
			} else {
				printf(_("Signing:   enabled (no signing key configured)\n"));
			}
		} else {
			if (signing_key && *signing_key) {
				printf(_("Signing:   disabled (key: %s, format: %s, commit.gpgsign: false)\n"),
				       signing_key,
				       gpg_format ? gpg_format : "openpgp");
			} else {
				printf(_("Signing:   disabled (commit.gpgsign: false)\n"));
			}
		}
	}

cleanup:
	free(signing_key);
	free(gpg_format);
	free(ssh_default_key_cmd);
	free(resolved_key);
	strbuf_release(&author_info);
	strbuf_release(&committer_info);
	strbuf_release(&author_name);
	strbuf_release(&author_email);
	strbuf_release(&committer_name);
	strbuf_release(&committer_email);

	return ret;
}
