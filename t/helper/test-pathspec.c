#include "test-tool.h"
#include "pathspec.h"

static const char usage_msg[] =
"test-tool pathspec trie [pathspecs...] -- [paths....]";

/*
 * XXX Yuck. This is a lot of complicated code specific to our test. Even if it
 * runs correctly, we have no real guarantee that the actual trie users are
 * doing it right. And reusing their code is tough, because it happens as part
 * of their own traversals (e.g., we walk the pathspec trie while walking the
 * tree objects themselves).
 *
 * This whole test program should probably go away in favor of directly testing
 * the tree-diff code.
 */
static int trie_match(const struct pathspec_trie *pst,
		      const char *path)
{
	int pathlen = strlen(path);
	int is_dir = 0;

	if (pathlen > 0 && path[pathlen-1] == '/') {
		is_dir = 1;
		pathlen--;
	}

	while (pathlen) {
		const char *slash = memchr(path, '/', pathlen);
		int component_len;
		int pos;

		if (slash)
			component_len = slash - path;
		else
			component_len = pathlen;

		pos = pathspec_trie_lookup(pst, path, component_len);
		if (pos < 0)
			return 0;

		pst = pst->entries[pos];
		path += component_len;
		pathlen -= component_len;

		while (pathlen && *path == '/') {
			path++;
			pathlen--;
		}

		if (pst->terminal) {
			if (!pst->must_be_dir)
				return 1;
			if (pathlen)
				return 1;
			return is_dir;
		}
	}
	return 0;
}

static int cmd_trie(const char **argv)
{
	const char **specs, **paths;
	struct pathspec pathspec;

	paths = specs = argv;
	while (*paths && strcmp(*paths, "--"))
		paths++;
	if (*paths)
		*paths++ = NULL;

	parse_pathspec(&pathspec, 0, 0, "", specs);
	if (!pathspec.trie)
		die("unable to make trie from pathspec");

	for (; *paths; paths++) {
		if (trie_match(pathspec.trie, *paths))
			printf("yes\n");
		else
			printf("no\n");
	}
	return 0;
}

int cmd__pathspec(int argc UNUSED, const char **argv)
{
	const char *cmd = argv[1];

	if (!cmd)
		usage(usage_msg);
	else if (!strcmp(cmd, "trie"))
		return cmd_trie(argv + 2);
	else
		die("unknown cmd: %s", cmd);
}
