#include "cache.h"
#include "object.h"
#include "argv-array.h"
#include "odb-helper.h"
#include "run-command.h"
#include "sha1-lookup.h"
#include "fetch-object.h"

struct odb_helper *odb_helper_new(const char *name, int namelen)
{
	struct odb_helper *o;

	o = xcalloc(1, sizeof(*o));
	o->name = xmemdupz(name, namelen);

	return o;
}

int odb_helper_get_direct(struct odb_helper *o,
			  const unsigned char *sha1)
{
	int res;
	uint64_t start = getnanotime();

	res = fetch_object(o->remote, sha1);

	trace_performance_since(start, "odb_helper_get_direct");

	return res;
}

int odb_helper_get_many_direct(struct odb_helper *o,
			       const struct oid_array *to_get)
{
	int res;
	uint64_t start;

	start = getnanotime();

	res = fetch_objects(o->remote, to_get);

	trace_performance_since(start, "odb_helper_get_many_direct");

	return res;
}
