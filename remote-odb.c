#include "cache.h"
#include "remote-odb.h"
#include "odb-helper.h"
#include "config.h"

static struct odb_helper *helpers;
static struct odb_helper **helpers_tail = &helpers;

static struct odb_helper *find_or_create_helper(const char *name, int len)
{
	struct odb_helper *o;

	for (o = helpers; o; o = o->next)
		if (!strncmp(o->name, name, len) && !o->name[len])
			return o;

	o = odb_helper_new(name, len);
	*helpers_tail = o;
	helpers_tail = &o->next;

	return o;
}

static struct odb_helper *do_find_odb_helper(const char *remote)
{
	struct odb_helper *o;

	for (o = helpers; o; o = o->next)
		if (o->remote && !strcmp(o->remote, remote))
			return o;

	return NULL;
}

static int remote_odb_config(const char *var, const char *value, void *data)
{
	struct odb_helper *o;
	const char *name;
	int namelen;
	const char *subkey;

	if (parse_config_key(var, "odb", &name, &namelen, &subkey) < 0)
		return 0;

	o = find_or_create_helper(name, namelen);

	if (!strcmp(subkey, "promisorremote")) {
		const char *remote;
		int res = git_config_string(&remote, var, value);

		if (res)
			return res;

		if (do_find_odb_helper(remote))
			return error(_("when parsing config key '%s' "
				       "helper for remote '%s' already exists"),
				     var, remote);

		o->remote = remote;

		return 0;
	}
	if (!strcmp(subkey, "partialclonefilter"))
		return git_config_string(&o->partial_clone_filter, var, value);

	return 0;
}

static void remote_odb_do_init(int force)
{
	static int initialized;

	if (!force && initialized)
		return;
	initialized = 1;

	git_config(remote_odb_config, NULL);
}

static inline void remote_odb_init(void)
{
	remote_odb_do_init(0);
}

inline void remote_odb_reinit(void)
{
	remote_odb_do_init(1);
}

struct odb_helper *find_odb_helper(const char *remote)
{
	remote_odb_init();

	if (!remote)
		return helpers;

	return do_find_odb_helper(remote);
}

int has_remote_odb(void)
{
	return !!find_odb_helper(NULL);
}

int remote_odb_get_direct(const unsigned char *sha1)
{
	struct odb_helper *o;

	trace_printf("trace: remote_odb_get_direct: %s", sha1_to_hex(sha1));

	remote_odb_init();

	for (o = helpers; o; o = o->next) {
		if (odb_helper_get_direct(o, sha1) < 0)
			continue;
		return 0;
	}

	return -1;
}

int remote_odb_get_many_direct(const struct oid_array *to_get)
{
	struct odb_helper *o;

	trace_printf("trace: remote_odb_get_many_direct: nr: %d", to_get->nr);

	remote_odb_init();

	for (o = helpers; o; o = o->next) {
		if (odb_helper_get_many_direct(o, to_get) < 0)
			continue;
		return 0;
	}

	return -1;
}
