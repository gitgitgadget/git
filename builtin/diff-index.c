#define USE_THE_INDEX_COMPATIBILITY_MACROS
#include "cache.h"
#include "config.h"
#include "diff.h"
#include "diff-merges.h"
#include "commit.h"
#include "revision.h"
#include "builtin.h"
#include "submodule.h"

static const char diff_cache_usage[] =
"git diff-index [-m] [--cached] [--merge-base] "
"[<common-diff-options>] <tree-ish> [<path>...]"
"\n"
COMMON_DIFF_OPTIONS_HELP;

int cmd_diff_index(int argc, const char **argv, const char *prefix)
{
	struct rev_info rev;
	unsigned int option = 0;
	int i;
	int result;
	enum sparse_scope scope;

	struct option sparse_scope_options[] = {
		OPT_CALLBACK_F(0, "scope", &scope, N_("[sparse|all]"),
				N_("restrict path scope in sparse specification"),
				PARSE_OPT_NONEG, diff_opt_sparse_scope),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage(diff_cache_usage);

	git_config(git_diff_basic_config, NULL); /* no "diff" UI options */
	repo_init_revisions(the_repository, &rev, prefix);
	rev.abbrev = 0;
	prefix = precompose_argv_prefix(argc, argv, prefix);

	/*
	 * We need (some of) diff for merges options (e.g., --cc), and we need
	 * to avoid conflict with our own meaning of "-m".
	 */
	diff_merges_suppress_m_parsing();

	argc = setup_revisions(argc, argv, &rev, NULL);

	argc = parse_options(argc, argv, prefix, sparse_scope_options, NULL,
			     PARSE_OPT_KEEP_DASHDASH |
			     PARSE_OPT_KEEP_UNKNOWN_OPT |
			     PARSE_OPT_KEEP_ARGV0 |
			     PARSE_OPT_NO_INTERNAL_HELP);

	rev.diffopt.scope = scope;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "--cached"))
			option |= DIFF_INDEX_CACHED;
		else if (!strcmp(arg, "--merge-base"))
			option |= DIFF_INDEX_MERGE_BASE;
		else if (!strcmp(arg, "-m"))
			rev.match_missing = 1;
		else
			usage(diff_cache_usage);
	}
	if (!rev.diffopt.output_format)
		rev.diffopt.output_format = DIFF_FORMAT_RAW;

	rev.diffopt.rotate_to_strict = 1;

	/*
	 * Make sure there is one revision (i.e. pending object),
	 * and there is no revision filtering parameters.
	 */
	if (rev.pending.nr != 1 ||
	    rev.max_count != -1 || rev.min_age != -1 || rev.max_age != -1)
		usage(diff_cache_usage);
	if (!(option & DIFF_INDEX_CACHED)) {
		setup_work_tree();
		if (read_cache_preload(&rev.diffopt.pathspec) < 0) {
			perror("read_cache_preload");
			return -1;
		}
	} else {
		if (read_cache() < 0) {
			perror("read_cache");
			return -1;
		}
		if (rev.diffopt.scope == SPARSE_SCOPE_SPARSE &&
		    strcmp(rev.pending.objects[0].name, "HEAD"))
			diff_collect_changes_index(&rev.diffopt.pathspec,
						   &rev.diffopt.change_index_files);
	}
	result = run_diff_index(&rev, option);
	result = diff_result_code(&rev.diffopt, result);
	release_revisions(&rev);
	return result;
}
