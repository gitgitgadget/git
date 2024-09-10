#include "builtin.h"
#include "commit.h"
#include "config.h"
#include "gettext.h"
#include "parse-options.h"
#include "pretty.h"
#include "strbuf.h"
#include "wt-status.h"

static const char * const psuh_usage[] = {
    N_("git psuh [<arg>...]"),
    NULL,
};

int cmd_psuh(int argc, const char **argv, const char *prefix)
{
    const char *cfg_name;
    struct wt_status status;
    struct commit *c = NULL;
    struct strbuf commitline = STRBUF_INIT;
    struct option options[] = { OPT_END() };

    argc = parse_options(argc, argv, prefix, options, psuh_usage, 0);

    printf(Q_("Your args (there is %d):\n",
	      "Your args (there are %d):\n",
	      argc),
	   argc);
    for (int i = 0; i < argc; i++)
	printf("%d: %s\n", i, argv[i]);

    printf(_("Your current working directory:\n<top-level>%s%s\n"),
	   prefix ? "/" : "", prefix ? prefix : "");

    wt_status_prepare(the_repository, &status);
    git_config(git_default_config, &status);
    if (git_config_get_string_tmp("user.name", &cfg_name) > 0)
	printf(_("No name found in config\n"));
    else
	printf(_("Your name: %s\n"), cfg_name);

    printf(_("Your current branch: %s\n"), status.branch);

    c = lookup_commit_reference_by_name("origin/master");
    if (c != NULL) {
	pp_commit_easy(CMIT_FMT_ONELINE, c, &commitline);
	printf(_("Current commit: %s\n"), commitline.buf);
    }

    printf(_("Pony saying hello goes here.\n"));
    return 0;
}