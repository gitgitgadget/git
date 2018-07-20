#include "cache.h"
#include "commit.h"
#include "config.h"
#include "revision.h"
#include "argv-array.h"
#include "list-objects.h"
#include "list-objects-filter.h"
#include "list-objects-filter-options.h"
#include "remote-odb.h"
#include "odb-helper.h"

/*
 * Parse value of the argument to the "filter" keyword.
 * On the command line this looks like:
 *       --filter=<arg>
 * and in the pack protocol as:
 *       "filter" SP <arg>
 *
 * The filter keyword will be used by many commands.
 * See Documentation/rev-list-options.txt for allowed values for <arg>.
 *
 * Capture the given arg as the "filter_spec".  This can be forwarded to
 * subordinate commands when necessary.  We also "intern" the arg for
 * the convenience of the current command.
 */
static int gently_parse_list_objects_filter(
	struct list_objects_filter_options *filter_options,
	const char *arg,
	struct strbuf *errbuf)
{
	const char *v0;

	if (!arg)
		return 0;

	if (filter_options->choice) {
		if (errbuf) {
			strbuf_init(errbuf, 0);
			strbuf_addstr(
				errbuf,
				_("multiple filter-specs cannot be combined"));
		}
		return 1;
	}

	filter_options->filter_spec = strdup(arg);

	if (!strcmp(arg, "blob:none")) {
		filter_options->choice = LOFC_BLOB_NONE;
		return 0;

	} else if (skip_prefix(arg, "blob:limit=", &v0)) {
		if (git_parse_ulong(v0, &filter_options->blob_limit_value)) {
			filter_options->choice = LOFC_BLOB_LIMIT;
			return 0;
		}

	} else if (skip_prefix(arg, "sparse:oid=", &v0)) {
		struct object_context oc;
		struct object_id sparse_oid;

		/*
		 * Try to parse <oid-expression> into an OID for the current
		 * command, but DO NOT complain if we don't have the blob or
		 * ref locally.
		 */
		if (!get_oid_with_context(v0, GET_OID_BLOB,
					  &sparse_oid, &oc))
			filter_options->sparse_oid_value = oiddup(&sparse_oid);
		filter_options->choice = LOFC_SPARSE_OID;
		return 0;

	} else if (skip_prefix(arg, "sparse:path=", &v0)) {
		filter_options->choice = LOFC_SPARSE_PATH;
		filter_options->sparse_path_value = strdup(v0);
		return 0;
	}

	if (errbuf) {
		strbuf_init(errbuf, 0);
		strbuf_addf(errbuf, "invalid filter-spec '%s'", arg);
	}
	memset(filter_options, 0, sizeof(*filter_options));
	return 1;
}

int parse_list_objects_filter(struct list_objects_filter_options *filter_options,
			      const char *arg)
{
	struct strbuf buf = STRBUF_INIT;
	if (gently_parse_list_objects_filter(filter_options, arg, &buf))
		die("%s", buf.buf);
	return 0;
}

int opt_parse_list_objects_filter(const struct option *opt,
				  const char *arg, int unset)
{
	struct list_objects_filter_options *filter_options = opt->value;

	if (unset || !arg) {
		list_objects_filter_set_no_filter(filter_options);
		return 0;
	}

	return parse_list_objects_filter(filter_options, arg);
}

void list_objects_filter_release(
	struct list_objects_filter_options *filter_options)
{
	free(filter_options->filter_spec);
	free(filter_options->sparse_oid_value);
	free(filter_options->sparse_path_value);
	memset(filter_options, 0, sizeof(*filter_options));
}

void partial_clone_register(
	const char *remote,
	const struct list_objects_filter_options *filter_options)
{
	char *cfg_name;
	char *filter_name;

	/* Check if it is already registered */
	if (find_odb_helper(remote))
		return;

	git_config_set("core.repositoryformatversion", "1");

	/* Add odb config for the remote */
	cfg_name = xstrfmt("odb.%s.promisorRemote", remote);
	git_config_set(cfg_name, remote);
	free(cfg_name);

	/*
	 * Record the initial filter-spec in the config as
	 * the default for subsequent fetches from this remote.
	 */
	filter_name = xstrfmt("odb.%s.partialclonefilter", remote);
	git_config_set(filter_name, filter_options->filter_spec);
	free(filter_name);

	/* Make sure the config info are reset */
	remote_odb_reinit();
}

void partial_clone_get_default_filter_spec(
	struct list_objects_filter_options *filter_options,
	const char *remote)
{
	struct odb_helper *helper = find_odb_helper(remote);

	/*
	 * Parse default value, but silently ignore it if it is invalid.
	 */
	if (helper)
		gently_parse_list_objects_filter(filter_options,
						 helper->partial_clone_filter,
						 NULL);
}
