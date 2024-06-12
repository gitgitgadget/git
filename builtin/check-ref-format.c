/*
 * GIT - The information manager from hell
 */

#include "builtin.h"
#include "refs.h"
#include "setup.h"
#include "strbuf.h"
#include "parse-options.h"

static const char * const builtin_check_ref_format_usage[] = {
	N_("git check-ref-format [--normalize] [<options>] <refname>\n"),
	N_("   or: git check-ref-format --branch <branchname-shorthand>"),
	NULL,
};

/*
 * Return a copy of refname but with leading slashes removed and runs
 * of adjacent slashes replaced with single slashes.
 *
 * This function is similar to normalize_path_copy(), but stripped down
 * to meet check_ref_format's simpler needs.
 */
static char *collapse_slashes(const char *refname)
{
	char *ret = xmallocz(strlen(refname));
	char ch;
	char prev = '/';
	char *cp = ret;

	while ((ch = *refname++) != '\0') {
		if (prev == '/' && ch == prev)
			continue;

		*cp++ = ch;
		prev = ch;
	}
	*cp = '\0';
	return ret;
}

static int check_ref_format_branch(const char *arg)
{
	struct strbuf sb = STRBUF_INIT;
	const char *name;
	int nongit;

	setup_git_directory_gently(&nongit);
	if (strbuf_check_branch_ref(&sb, arg) ||
	    !skip_prefix(sb.buf, "refs/heads/", &name))
		die("'%s' is not a valid branch name", arg);
	printf("%s\n", name);
	strbuf_release(&sb);
	return 0;
}

int cmd_check_ref_format(int argc, const char **argv, const char *prefix)
{
	enum {
		CHECK_REFNAME_FORMAT = 0,
		CHECK_REF_FORMAT_BRANCH,
	} mode = CHECK_REFNAME_FORMAT;

	int normalize = 0;
	int flags = 0;
	const char *refname;
	char *to_free = NULL;
	int ret = 1;

	BUG_ON_NON_EMPTY_PREFIX(prefix);

	struct option options[] = {
		OPT_GROUP(""),
		OPT_CMDMODE(0 , "branch", &mode, N_("check for valid branch name"), CHECK_REF_FORMAT_BRANCH),
		OPT_BOOL(0 , "normalize", &normalize, N_("normalize tracked files")),
		OPT_BIT(0 , "allow-onelevel", &flags, N_("allow one level"), REFNAME_ALLOW_ONELEVEL),
		OPT_BIT(0 , "refspec-pattern", &flags, N_("refspec pattern"), REFNAME_REFSPEC_PATTERN),
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, options, builtin_check_ref_format_usage, 0);

	refname = argv[0];
	if (mode)
		return check_ref_format_branch(argv[2]);
	if (normalize)
		refname = to_free = collapse_slashes(refname);
	if (check_refname_format(refname, flags))
		goto cleanup;
	if (normalize)
		printf("%s\n", refname);

	ret = 0;
cleanup:
	free(to_free);
	return ret;
}
