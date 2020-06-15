/*
 * This file is in the public domain.
 * You may freely use, modify, distribute, and relicense it.
 */

#include "cache.h"
#include "config.h"
#include "refs.h"
#include "svndump.h"

int cmd_main(int argc, const char **argv)
{
	int nongit_ok = 0;
	char *branch_name;

	setup_git_directory_gently(&nongit_ok);
	git_config(git_default_config, NULL);
	branch_name = git_main_branch_name(MAIN_BRANCH_FULL_NAME |
					   MAIN_BRANCH_FOR_INIT);

	if (svndump_init(NULL))
		return 1;

	svndump_read((argc > 1) ? argv[1] : NULL, branch_name,
			"refs/notes/svn/revs");
	free(branch_name);
	svndump_deinit();
	svndump_reset();
	return 0;
}
