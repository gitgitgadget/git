#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "blame-tree.h"
#include "strvec.h"
#include "hex.h"
#include "quote.h"
#include "config.h"
#include "environment.h"
#include "object-name.h"
#include "parse-options.h"
#include "builtin.h"
#include "setup.h"

static void show_entry(const char *path, const struct commit *commit, void *d)
{
	struct blame_tree *bt = d;

	if (commit->object.flags & BOUNDARY)
		putchar('^');
	printf("%s\t", oid_to_hex(&commit->object.oid));

	if (bt->rev.diffopt.line_termination)
		write_name_quoted(path, stdout, '\n');
	else
		printf("%s%c", path, '\0');

	fflush(stdout);
}

int cmd_blame_tree(int argc, const char **argv, const char *prefix, struct repository *repo)
{
	struct blame_tree bt = BLAME_TREE_INIT;
	struct blame_tree_options opts = BLAME_TREE_OPTIONS_INIT(
		.prefix = prefix,
	);

	struct option options[] = {
		OPT_BOOL(0, "recursive", &opts.recursive,
			 "recurse into to subtrees"),
		OPT_END()
	};

	const char * const blame_tree_usage[] = {
		N_("git blame-tree [--no-recursive] [<rev-opts>]"),
		NULL,
	};

	git_config(git_default_config, NULL);

	if (repo_get_oid(the_repository, "HEAD", &opts.oid))
		 die("unable to get HEAD");

	argc = parse_options(argc, argv, prefix, options, blame_tree_usage,
			     PARSE_OPT_KEEP_ARGV0 | PARSE_OPT_KEEP_UNKNOWN_OPT);
	if (argc)
		strvec_pushv(&opts.args, argv);

	blame_tree_init(repo, &bt, &opts);

	if (blame_tree_run(&bt, show_entry, &bt) < 0)
		die(_("error running blame-tree traversal"));
	blame_tree_release(&bt);
	blame_tree_opts_release(&opts);

	return 0;
}
