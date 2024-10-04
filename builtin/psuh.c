#define USE_THE_REPOSITORY_VARIABLE

#include <stdio.h>
#include "builtin.h"
#include "config.h"
#include "gettext.h"
#include "repository.h"
#include "wt-status.h"


int cmd_psuh(int argc, const char **argv, const char *prefix, struct repository *repo) {
        const char *cfg_name;
        struct wt_status status;
        int i;

        printf("%d\n", repo->different_commondir);
        printf(_("%s\n"), prefix);

        printf(Q_("Your args (there is %d):\n",
                "Your args (there are %d):\n",
                argc),
                argc);
        for (i = 0; i < argc; i++)
                printf("%d: %s\n", i, argv[i]);

        printf(_("Your current working directory:\n<top-level>%s%s\n"),
                prefix ? "/" : "", prefix ? prefix : "");

        // git config implementation
        git_config(git_default_config, NULL);
        if(git_config_get_string_tmp("user.name", &cfg_name) > 0)
                printf(_("No name is found in config\n"));
        else
                printf(_("Your name: %s\n"), cfg_name);

        // getting current branch from status struct
        wt_status_prepare(the_repository, &status);
        git_config(git_default_config, &status);
        printf(_("Your current branch: %s\n"), status.branch);
        return 0;

}
