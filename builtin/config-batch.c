#define USE_THE_REPOSITORY_VARIABLE
#include "builtin.h"
#include "config.h"
#include "environment.h"
#include "parse-options.h"

static const char *const builtin_config_batch_usage[] = {
	N_("git config-batch <options>"),
	NULL
};

int cmd_config_batch(int argc,
		     const char **argv,
		     const char *prefix,
		     struct repository *repo)
{
	struct option options[] = {
		OPT_END(),
	};

	show_usage_with_options_if_asked(argc, argv,
					 builtin_config_batch_usage, options);

	argc = parse_options(argc, argv, prefix, options, builtin_config_batch_usage,
			     0);

	repo_config(repo, git_default_config, NULL);

	return 0;
}
