#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "git-compat-util.h"
#include "hex.h"
#include "connected.h"
#include "oid-array.h"
#include "setup.h"

struct cb_data {
	struct oid_array *oids;
	size_t idx;
};

static const struct object_id *iterate_oids(void *data)
{
	struct cb_data *cb = data;
	if (cb->idx >= cb->oids->nr)
		return NULL;
	return &cb->oids->oid[cb->idx++];
}

int cmd__check_connected(int argc, const char **argv)
{
	struct oid_array oids = OID_ARRAY_INIT;
	struct check_connected_options opt = CHECK_CONNECTED_INIT;
	struct cb_data cb;
	int i, ret;

	setup_git_directory(the_repository);

	for (i = 1; i < argc; i++) {
		struct object_id oid;
		if (!strcmp(argv[i], "--shallow-file")) {
			if (++i >= argc)
				die("--shallow-file requires an argument");
			opt.shallow_file = argv[i];
			continue;
		}
		if (!strcmp(argv[i], "--err-file")) {
			if (++i >= argc)
				die("--err-file requires a path argument");
			opt.err_fd = open(argv[i],
					  O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (opt.err_fd < 0)
				die_errno("could not open '%s'", argv[i]);
			continue;
		}
		if (!strcmp(argv[i], "--quiet")) {
			opt.quiet = 1;
			continue;
		}
		if (get_oid_hex(argv[i], &oid))
			die("not a valid object: %s", argv[i]);
		oid_array_append(&oids, &oid);
	}

	if (!oids.nr)
		die("usage: test-tool check-connected [--shallow-file <path>] [--err-file <path>] [--quiet] <oid>...");

	cb.oids = &oids;
	cb.idx = 0;

	ret = check_connected(iterate_oids, &cb, &opt);
	oid_array_clear(&oids);
	return !!ret;
}
