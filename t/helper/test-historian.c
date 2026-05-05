/*
 * Build a small history out of a tiny declarative input. Used by tests
 * that need specific merge topologies without long sequences of
 * plumbing commands or fragile shell helpers.
 *
 * The historian reads stdin line by line and emits an equivalent
 * stream to a `git fast-import` child process. It also allocates marks
 * for named objects so tests can refer to commits and blobs by name.
 *
 * Input directives (one per line, shell-style quoting):
 *
 *	blob NAME LINE1 LINE2 ...
 *	    Each LINE becomes a content line in the blob; lines are
 *	    joined with '\n' and the blob ends with a final '\n'. With
 *	    no LINEs, the blob is empty.
 *
 *	commit NAME BRANCH SUBJECT [from=PARENT] [merge=PARENT]... [PATH=BLOB]...
 *	    Creates a commit on refs/heads/BRANCH using the listed
 *	    file=blob mappings as the entire tree (no inheritance from
 *	    parents). Up to one `from=` and any number of `merge=`
 *	    parents may be given. `from=` defaults to the current branch
 *	    tip; if BRANCH has no tip yet, the commit becomes a root.
 *
 * Each `commit NAME` directive also creates a lightweight tag
 * `refs/tags/NAME` so tests can `git rev-parse NAME`.
 *
 * This helper trusts its caller; malformed input results in fast-import
 * errors. That is fine because test scripts feed it tightly controlled
 * input.
 */

#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "git-compat-util.h"
#include "alias.h"
#include "run-command.h"
#include "setup.h"
#include "strbuf.h"
#include "strmap.h"
#include "strvec.h"

static int next_mark = 1;

static int resolve_mark(struct strintmap *names, const char *name)
{
	int n = strintmap_get(names, name);
	if (!n) {
		n = next_mark++;
		strintmap_set(names, name, n);
	}
	return n;
}

static void emit_data(FILE *out, const char *data, size_t len)
{
	fprintf(out, "data %"PRIuMAX"\n", (uintmax_t)len);
	fwrite(data, 1, len, out);
	fputc('\n', out);
}

static void emit_blob(FILE *out, struct strintmap *names,
		      int argc, const char **argv)
{
	struct strbuf content = STRBUF_INIT;
	int n = resolve_mark(names, argv[1]);
	int i;

	for (i = 2; i < argc; i++) {
		strbuf_addstr(&content, argv[i]);
		strbuf_addch(&content, '\n');
	}

	fprintf(out, "blob\nmark :%d\n", n);
	emit_data(out, content.buf, content.len);
	strbuf_release(&content);
}

static void emit_tag(FILE *out, const char *name, int mark)
{
	fprintf(out, "reset refs/tags/%s\nfrom :%d\n\n", name, mark);
}

static void emit_commit(FILE *out, struct strintmap *names,
			int argc, const char **argv, int seq)
{
	int n = resolve_mark(names, argv[1]);
	const char *branch = argv[2];
	const char *subject = argv[3];
	const char *rest;
	int i;

	fprintf(out, "commit refs/heads/%s\nmark :%d\n", branch, n);
	fprintf(out, "author A <a@e> %d +0000\n", 1700000000 + seq);
	fprintf(out, "committer A <a@e> %d +0000\n", 1700000000 + seq);
	emit_data(out, subject, strlen(subject));

	/*
	 * fast-import requires `from` and `merge` to precede all file
	 * operations; emit them first regardless of argv ordering.
	 */
	for (i = 4; i < argc; i++) {
		if (skip_prefix(argv[i], "from=", &rest))
			fprintf(out, "from :%d\n", resolve_mark(names, rest));
		else if (skip_prefix(argv[i], "merge=", &rest))
			fprintf(out, "merge :%d\n", resolve_mark(names, rest));
	}

	/*
	 * The PATH=BLOB list is the entire tree; wipe whatever the
	 * implicit parent contributed before re-applying it.
	 */
	fprintf(out, "deleteall\n");
	for (i = 4; i < argc; i++) {
		const char *eq;
		size_t key_len;
		char *path;

		if (skip_prefix(argv[i], "from=", &rest) ||
		    skip_prefix(argv[i], "merge=", &rest))
			continue;
		eq = strchr(argv[i], '=');
		if (!eq)
			die("bad commit spec '%s'", argv[i]);
		key_len = eq - argv[i];
		path = xmemdupz(argv[i], key_len);
		fprintf(out, "M 100644 :%d %s\n",
			resolve_mark(names, eq + 1), path);
		free(path);
	}

	fputc('\n', out);
	emit_tag(out, argv[1], n);
}

int cmd__historian(int argc, const char **argv UNUSED)
{
	struct child_process fi = CHILD_PROCESS_INIT;
	struct strintmap names = STRINTMAP_INIT;
	struct strbuf line = STRBUF_INIT;
	int seq = 0;
	int ret = 0;
	FILE *fi_in;

	if (argc != 1)
		die("usage: test-tool historian <input");

	setup_git_directory();

	strvec_pushl(&fi.args, "fast-import", "--quiet", "--force", NULL);
	fi.git_cmd = 1;
	fi.in = -1;
	fi.no_stdout = 1;
	if (start_command(&fi))
		die("failed to start git fast-import");
	fi_in = xfdopen(fi.in, "w");

	while (strbuf_getline_lf(&line, stdin) != EOF) {
		const char **a = NULL;
		int n;

		strbuf_trim(&line);
		if (!line.len || line.buf[0] == '#')
			continue;

		n = split_cmdline(line.buf, &a);
		if (n < 0)
			die("split_cmdline failed: %s",
			    split_cmdline_strerror(n));

		if (n >= 2 && !strcmp(a[0], "blob"))
			emit_blob(fi_in, &names, n, a);
		else if (n >= 4 && !strcmp(a[0], "commit"))
			emit_commit(fi_in, &names, n, a, seq++);
		else
			die("unknown directive: %s", a[0]);

		free(a);
	}

	if (fclose(fi_in))
		die_errno("close fast-import stdin");
	if (finish_command(&fi))
		ret = 1;

	strbuf_release(&line);
	strintmap_clear(&names);
	return ret;
}
