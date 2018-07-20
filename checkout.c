#include "cache.h"
#include "remote.h"
#include "refspec.h"
#include "checkout.h"
#include "config.h"
#include "unpack-trees.h"
#include "lockfile.h"
#include "refs.h"
#include "tree.h"
#include "cache-tree.h"

struct tracking_name_data {
	/* const */ char *src_ref;
	char *dst_ref;
	struct object_id *dst_oid;
	int num_matches;
	const char *default_remote;
	char *default_dst_ref;
	struct object_id *default_dst_oid;
};

#define TRACKING_NAME_DATA_INIT { NULL, NULL, NULL, 0, NULL, NULL, NULL }

static int check_tracking_name(struct remote *remote, void *cb_data)
{
	struct tracking_name_data *cb = cb_data;
	struct refspec_item query;
	memset(&query, 0, sizeof(struct refspec_item));
	query.src = cb->src_ref;
	if (remote_find_tracking(remote, &query) ||
	    get_oid(query.dst, cb->dst_oid)) {
		free(query.dst);
		return 0;
	}
	cb->num_matches++;
	if (cb->default_remote && !strcmp(remote->name, cb->default_remote)) {
		struct object_id *dst = xmalloc(sizeof(*cb->default_dst_oid));
		cb->default_dst_ref = xstrdup(query.dst);
		oidcpy(dst, cb->dst_oid);
		cb->default_dst_oid = dst;
	}
	if (cb->dst_ref) {
		free(query.dst);
		return 0;
	}
	cb->dst_ref = query.dst;
	return 0;
}

const char *unique_tracking_name(const char *name, struct object_id *oid,
				 int *dwim_remotes_matched)
{
	struct tracking_name_data cb_data = TRACKING_NAME_DATA_INIT;
	const char *default_remote = NULL;
	if (!git_config_get_string_const("checkout.defaultremote", &default_remote))
		cb_data.default_remote = default_remote;
	cb_data.src_ref = xstrfmt("refs/heads/%s", name);
	cb_data.dst_oid = oid;
	for_each_remote(check_tracking_name, &cb_data);
	if (dwim_remotes_matched)
		*dwim_remotes_matched = cb_data.num_matches;
	free(cb_data.src_ref);
	free((char *)default_remote);
	if (cb_data.num_matches == 1) {
		free(cb_data.default_dst_ref);
		free(cb_data.default_dst_oid);
		return cb_data.dst_ref;
	}
	free(cb_data.dst_ref);
	if (cb_data.default_dst_ref) {
		oidcpy(oid, cb_data.default_dst_oid);
		free(cb_data.default_dst_oid);
		return cb_data.default_dst_ref;
	}
	return NULL;
}

int detach_head_to(struct object_id *oid, const char *action,
		   const char *reflog_message)
{
	struct strbuf ref_name = STRBUF_INIT;
	struct tree_desc desc;
	struct lock_file lock = LOCK_INIT;
	struct unpack_trees_options unpack_tree_opts;
	struct tree *tree;
	int ret = 0;

	if (hold_locked_index(&lock, LOCK_REPORT_ON_ERROR) < 0)
		return -1;

	memset(&unpack_tree_opts, 0, sizeof(unpack_tree_opts));
	setup_unpack_trees_porcelain(&unpack_tree_opts, action);
	unpack_tree_opts.head_idx = 1;
	unpack_tree_opts.src_index = &the_index;
	unpack_tree_opts.dst_index = &the_index;
	unpack_tree_opts.fn = oneway_merge;
	unpack_tree_opts.merge = 1;
	unpack_tree_opts.update = 1;

	if (read_cache_unmerged()) {
		rollback_lock_file(&lock);
		strbuf_release(&ref_name);
		return error_resolve_conflict(_(action));
	}

	if (!fill_tree_descriptor(&desc, oid)) {
		error(_("failed to find tree of %s"), oid_to_hex(oid));
		rollback_lock_file(&lock);
		free((void *)desc.buffer);
		strbuf_release(&ref_name);
		return -1;
	}

	if (unpack_trees(1, &desc, &unpack_tree_opts)) {
		rollback_lock_file(&lock);
		free((void *)desc.buffer);
		strbuf_release(&ref_name);
		return -1;
	}

	tree = parse_tree_indirect(oid);
	prime_cache_tree(&the_index, tree);

	if (write_locked_index(&the_index, &lock, COMMIT_LOCK) < 0)
		ret = error(_("could not write index"));
	free((void *)desc.buffer);

	if (!ret)
		ret = update_ref(reflog_message, "HEAD", oid,
				 NULL, 0, UPDATE_REFS_MSG_ON_ERR);

	strbuf_release(&ref_name);
	return ret;

}
