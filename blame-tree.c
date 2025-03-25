#include "git-compat-util.h"
#include "blame-tree.h"
#include "strvec.h"
#include "hex.h"
#include "commit.h"
#include "diffcore.h"
#include "diff.h"
#include "object.h"
#include "revision.h"
#include "repository.h"
#include "log-tree.h"

void blame_tree_opts_release(struct blame_tree_options *bto)
{
	strvec_clear(&bto->args);
}

struct blame_tree_entry {
	struct object_id oid;
	struct commit *commit;
};

static void add_from_diff(struct diff_queue_struct *q,
			  struct diff_options *opt UNUSED, void *data)
{
	struct blame_tree *bt = data;

	for (int i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];
		struct blame_tree_entry *ent = xcalloc(1, sizeof(*ent));
		struct string_list_item *it;

		oidcpy(&ent->oid, &p->two->oid);
		it = string_list_append(&bt->paths, p->two->path);
		it->util = ent;
	}
}

static int add_from_revs(struct blame_tree *bt)
{
	size_t count = 0;
	struct diff_options diffopt;

	memcpy(&diffopt, &bt->rev.diffopt, sizeof(diffopt));
	diffopt.output_format = DIFF_FORMAT_CALLBACK;
	diffopt.format_callback = add_from_diff;
	diffopt.format_callback_data = bt;
	diffopt.no_free = 1;

	for (size_t i = 0; i < bt->rev.pending.nr; i++) {
		struct object_array_entry *obj = bt->rev.pending.objects + i;

		if (obj->item->flags & UNINTERESTING)
			continue;

		if (count++)
			return error(_("can only blame one tree at a time"));
		diff_tree_oid(bt->rev.repo->hash_algo->empty_tree,
			      &obj->item->oid, "", &diffopt);
		diff_flush(&diffopt);
	}

	string_list_sort(&bt->paths);
	return 0;
}

void blame_tree_init(struct repository *r, struct blame_tree *bt,
		     const struct blame_tree_options *opts)
{
	repo_init_revisions(r, &bt->rev, opts->prefix);
	bt->rev.def = oid_to_hex(&opts->oid);
	bt->rev.combine_merges = 1;
	bt->rev.show_root_diff = 1;
	bt->rev.boundary = 1;
	bt->rev.no_commit_id = 1;
	bt->rev.diff = 1;
	bt->rev.diffopt.flags.recursive = opts->recursive;
	setup_revisions(opts->args.nr, opts->args.v, &bt->rev, NULL);

	if (add_from_revs(bt) < 0)
		die(_("unable to setup blame-tree"));
}

void blame_tree_release(struct blame_tree *bt)
{
	string_list_clear(&bt->paths, 1);
	release_revisions(&bt->rev);
}

struct blame_tree_callback_data {
	struct commit *commit;
	struct string_list *paths;
	size_t num_interesting;

	blame_tree_fn callback;
	void *callback_data;
};

static void mark_path(const char *path, const struct object_id *oid,
		      struct blame_tree_callback_data *data)
{
	struct string_list_item *item = string_list_lookup(data->paths, path);
	struct blame_tree_entry *ent;

	/* Is it even a path that exists in our tree? */
	if (!item)
		return;

	/* Have we already blamed a commit? */
	ent = item->util;
	if (ent->commit)
		return;

	/*
	 * Is it arriving at a version of interest, or is it from a side branch
	 * which did not contribute to the final state?
	 */
	if (!oideq(oid, &ent->oid))
		return;

	ent->commit = data->commit;
	data->num_interesting--;
	if (data->callback)
		data->callback(path, data->commit, data->callback_data);
}

static void blame_diff(struct diff_queue_struct *q,
		       struct diff_options *opt UNUSED, void *cbdata)
{
	struct blame_tree_callback_data *data = cbdata;

	for (int i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];
		switch (p->status) {
		case DIFF_STATUS_DELETED:
			/*
			 * There's no point in feeding a deletion, as it could
			 * not have resulted in our current state, which
			 * actually has the file.
			 */
			break;

		default:
			/*
			 * Otherwise, we care only that we somehow arrived at
			 * a final path/sha1 state. Note that this covers some
			 * potentially controversial areas, including:
			 *
			 *  1. A rename or copy will be blamed, as it is the
			 *     first time the content has arrived at the given
			 *     path.
			 *
			 *  2. Even a non-content modification like a mode or
			 *     type change will trigger it.
			 *
			 * We take the inclusive approach for now, and blame
			 * anything which impacts the path. Options to tweak
			 * the behavior (e.g., to "--follow" the content across
			 * renames) can come later.
			 */
			mark_path(p->two->path, &p->two->oid, data);
			break;
		}
	}
}

int blame_tree_run(struct blame_tree *bt, blame_tree_fn cb, void *cbdata)
{
	struct blame_tree_callback_data data;

	data.paths = &bt->paths;
	data.num_interesting = bt->paths.nr;
	data.callback = cb;
	data.callback_data = cbdata;

	bt->rev.diffopt.output_format = DIFF_FORMAT_CALLBACK;
	bt->rev.diffopt.format_callback = blame_diff;
	bt->rev.diffopt.format_callback_data = &data;

	prepare_revision_walk(&bt->rev);

	while (data.num_interesting) {
		data.commit = get_revision(&bt->rev);
		if (!data.commit)
			break;

		if (data.commit->object.flags & BOUNDARY) {
			diff_tree_oid(bt->rev.repo->hash_algo->empty_tree,
				       &data.commit->object.oid,
				       "", &bt->rev.diffopt);
			diff_flush(&bt->rev.diffopt);
		} else {
			log_tree_commit(&bt->rev, data.commit);
		}
	}

	return 0;
}
