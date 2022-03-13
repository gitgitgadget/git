#include "builtin.h"
#include "gettext.h"
#include <stdio.h>
#include "config.h"
#include "repository.h"
#include "wt-status.h"
#include "commit.h"
#include "pretty.h"

int cmd_psuh(int argc, const char **argv, const char *prefix)
{
    int i;
    const char *cfg_name;
    struct wt_status status;
    struct commit *c = NULL;
    struct strbuf commitline = STRBUF_INIT;

    printf(_("Pony saying hello goes here.\n"));

    printf(Q_("Your args (there is %d):\n",
              "Your args (there are %d):\n",
              argc),
           argc);
    for (i = 0; i < argc; i++)
        printf("%d: %s\n", i, argv[i]);

    printf(_("Your current working directory:\n<top-leve>%s%s\n"),
            prefix ? "/" : "", prefix ? prefix : "");
    wt_status_prepare(the_repository, &status);
    wt_status_prepare(the_repository, &status);
    git_config(git_default_config, &status);
    if (git_config_get_string_tmp("user.name", &cfg_name) > 0)
        printf(_("No name is found in config\n"));
    else
        printf(_("Your name: %s\n"), cfg_name);

    printf(_("Your current branch: %s\n"), status.branch);

    c = lookup_commit_reference_by_name("origin/master");

    if (c != NULL) {
        pp_commit_easy(CMIT_FMT_ONELINE, c, &commitline);
        printf(_("Current commit: %s\n"), commitline.buf);
    }

    return 0;
}
