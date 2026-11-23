#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "hex.h"
#include "repository.h"
#include "odb.h"
#include "setup.h"

/*
 * Prints the size of the object corresponding to the given hash in a specific
 * gitdir. This is similar to "git -C gitdir cat-file -s", except that this
 * exercises the code that accesses the object of an arbitrary repository that
 * is not the_repository. ("git -C gitdir" makes it so that the_repository is
 * the one in gitdir.)
 */
static void object_info(const char *gitdir, const char *oid_hex)
{
	struct repository r;
	struct object_id oid;
	size_t size;
	struct object_info oi = {.sizep = &size};
	const char *p;

	if (repo_init(&r, gitdir, NULL))
		die("could not init repo");
	if (parse_oid_hex_algop(oid_hex, &oid, &p, r.hash_algo))
		die("could not parse oid");
	if (odb_read_object_info_extended(r.objects, &oid, &oi, 0))
		die("could not obtain object info");
	printf("%d\n", (int) size);

	repo_clear(&r);
}

static void retry_object_info(const char *oid_hex)
{
	struct object_id oid;
	enum object_type type;
	struct object_info oi = { .typep = &type };
	struct object_database *odb = the_repository->objects;

	if (get_oid_hex(oid_hex, &oid))
		die("could not parse oid");
	if (!odb_read_object_info_extended(odb, &oid, &oi,
					   OBJECT_INFO_LOOKUP_REPLACE |
					   OBJECT_INFO_SKIP_FETCH_OBJECT))
		die("unexpected success without fetching");
	if (odb_read_object_info_extended(odb, &oid, &oi,
					  OBJECT_INFO_LOOKUP_REPLACE))
		die("could not retry object with fetching enabled");
	puts(type_name(type));
}

int cmd__partial_clone(int argc, const char **argv)
{
	setup_git_directory(the_repository);

	if (argc < 2)
		die("too few arguments");
	if (argc == 4 && !strcmp(argv[1], "object-info"))
		object_info(argv[2], argv[3]);
	else if (argc == 3 && !strcmp(argv[1], "retry-object-info"))
		retry_object_info(argv[2]);
	else
		die("invalid argument '%s'", argv[1]);

	return 0;
}
