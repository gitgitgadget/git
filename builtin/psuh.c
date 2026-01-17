#include "builtin.h"
#include "gettext.h"

int cmd_psuh(int argc, const char **argv,
	const char *prefix, struct repository *repo) {
	int i;
	const char *config_name;
	printf(_("Pushing to %s\n"), repo->name);
	printf(_("Pushing to %s\n"), repo->url);
	printf(Q_("Your args (there is %d):\n",
		  "Your args (there are %d):\n",
		  argc),
		argc);
	for (i = 0; i < argc; i++)
		printf("%d: %s\n", i, argv[i]);

	printf(_("Your current working directory:\n<top-level>%s%s\n"),
	       prefix ? "/" : "", prefix ? prefix : "");

	repo_config(repo, git_default_config, NULL);

	if (repo_config_get_string_tmp(repo, "user.name", &config_name))
		printf(_("No name is found in config\n"));
	else
		printf(_("Your name: %s\n"), config_name);	

	return 0;
}
