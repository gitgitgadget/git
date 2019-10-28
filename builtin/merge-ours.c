/*
 * Implementation of git-merge-ours.sh as builtin
 *
 * Copyright (c) 2007 Thomas Harning Jr
 * Original:
 * Original Copyright (c) 2005 Junio C Hamano
 *
 * Pretend we resolved the heads, but declare our tree trumps everybody else.
 */
#define USE_THE_INDEX_COMPATIBILITY_MACROS
#include "git-compat-util.h"
#include "builtin.h"
#include "diff.h"
#include "parse-options.h"

static const char * const merge_ours_usage[] = {
	N_("git merge-ours [<base>...] -- <head> <merge-head>..."),
	NULL,
};

int cmd_merge_ours(int argc, const char **argv, const char *prefix)
{
	struct option options[] = {
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options, merge_ours_usage, 0);

	/*
	 * The contents of the current index becomes the tree we
	 * commit.  The index must match HEAD, or this merge cannot go
	 * through.
	 */
	if (read_cache() < 0)
		die_errno("read_cache failed");
	if (index_differs_from(the_repository, "HEAD", NULL, 0))
		exit(2);
	exit(0);
}
