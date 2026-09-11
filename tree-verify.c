#include "git-compat-util.h"
#include "commit.h"
#include "gettext.h"
#include "hex.h"
#include "odb.h"
#include "oid-array.h"
#include "oidset.h"
#include "tree.h"
#include "tree-walk.h"
#include "tree-verify.h"
#include "packfile.h"
#include "trace2.h"

struct work_item {
	struct name_entry entry;
	struct oid_array parent_trees;
};

struct verify_state {
	struct oidset trusted_trees;
	struct oidset trusted_blobs;
	struct oidset expanded_trees;
	int trees_loaded;
	int blobs_checked;
	int exclude_promisor_objects;
};

/*
 * Merge-walk the work list against one base tree entry, recording
 * same-path parent subtrees as recursive comparison bases.
 * Returns the updated work-list cursor.
 */
static size_t collect_subtree_bases(struct work_item *work, size_t nr_work,
				    size_t wi, const struct name_entry *entry)
{
	while (wi < nr_work) {
		int cmp = base_name_compare(
			work[wi].entry.path, work[wi].entry.pathlen,
			work[wi].entry.mode,
			entry->path, entry->pathlen,
			entry->mode);
		if (cmp > 0)
			break;
		if (cmp < 0) {
			wi++;
			continue;
		}
		if (S_ISDIR(work[wi].entry.mode) &&
		    S_ISDIR(entry->mode))
			oid_array_append(&work[wi].parent_trees,
					 &entry->oid);
		return wi + 1;
	}
	return wi;
}

static void verify_blob(struct repository *repo,
			const struct object_id *oid,
			struct verify_state *vs)
{
	if (oidset_contains(&vs->trusted_trees, oid))
		die(_("object %s is a tree, not a blob"),
		    oid_to_hex(oid));
	vs->blobs_checked++;
	if (odb_has_object(repo->objects, oid, 0) ||
	    (vs->exclude_promisor_objects &&
	     is_promisor_object(repo, oid))) {
		oidset_insert(&vs->trusted_blobs, oid);
		return;
	}
	die(_("missing blob object '%s'"), oid_to_hex(oid));
}

static void verify_tree(struct repository *repo,
			const struct object_id *new_tree_oid,
			const struct oid_array *base_trees,
			struct verify_state *vs, int depth)
{
	struct tree *tree;
	struct tree_desc desc;
	struct name_entry entry;
	struct work_item *work = NULL;
	size_t nr_work = 0, alloc_work = 0;
	int need_subtree_bases = 0;
	size_t i;

	if (depth > repo->settings.max_allowed_tree_depth)
		die(_("exceeded maximum allowed tree depth"));

	if (oidset_contains(&vs->trusted_trees, new_tree_oid))
		return;

	tree = lookup_tree(repo, new_tree_oid);
	if (!tree || repo_parse_tree_gently(repo, tree, 1)) {
		if (odb_has_object(repo->objects, new_tree_oid, 0))
			die(_("malformed tree object %s"),
			    oid_to_hex(new_tree_oid));
		if (vs->exclude_promisor_objects &&
		    is_promisor_object(repo, new_tree_oid)) {
			oidset_insert(&vs->trusted_trees, new_tree_oid);
			return;
		}
		die(_("bad tree object %s"),
		    oid_to_hex(new_tree_oid));
	}

	vs->trees_loaded++;
	init_tree_desc(&desc, &tree->object.oid,
		       tree->buffer, tree->size);

	while (tree_entry(&desc, &entry)) {
		if (S_ISGITLINK(entry.mode))
			continue;
		if (S_ISDIR(entry.mode)) {
			if (oidset_contains(&vs->trusted_trees, &entry.oid))
				continue;
			need_subtree_bases = 1;
		} else {
			if (oidset_contains(&vs->trusted_blobs, &entry.oid))
				continue;
		}
		ALLOC_GROW(work, nr_work + 1, alloc_work);
		work[nr_work] = (struct work_item){ .entry = entry };
		nr_work++;
	}

	if (!nr_work)
		goto done;

	for (i = 0; base_trees && i < base_trees->nr; i++) {
		const struct object_id *base_oid = &base_trees->oid[i];
		int expanded = oidset_contains(&vs->expanded_trees, base_oid);
		struct tree *base;
		struct tree_desc base_desc;
		struct name_entry scan_entry;
		size_t wi = 0;

		if (expanded && !need_subtree_bases)
			continue;

		base = lookup_tree(repo, base_oid);
		if (!base || repo_parse_tree_gently(repo, base, 1))
			die(_("bad tree object %s"),
			    oid_to_hex(base_oid));

		vs->trees_loaded++;
		init_tree_desc(&base_desc, &base->object.oid,
			       base->buffer, base->size);

		while (tree_entry(&base_desc, &scan_entry)) {
			if (S_ISGITLINK(scan_entry.mode))
				continue;

			if (need_subtree_bases)
				wi = collect_subtree_bases(work, nr_work,
							   wi, &scan_entry);

			if (!expanded) {
				struct oidset *set = S_ISDIR(scan_entry.mode)
					? &vs->trusted_trees
					: &vs->trusted_blobs;
				oidset_insert(set, &scan_entry.oid);
			}
		}

		if (!expanded)
			oidset_insert(&vs->expanded_trees, base_oid);

		free_tree_buffer(base);
	}

	for (i = 0; i < nr_work; i++) {
		if (S_ISDIR(work[i].entry.mode)) {
			if (!oidset_contains(&vs->trusted_trees,
					     &work[i].entry.oid))
				verify_tree(repo, &work[i].entry.oid,
					    &work[i].parent_trees, vs,
					    depth + 1);
		} else {
			if (!oidset_contains(&vs->trusted_blobs,
					     &work[i].entry.oid))
				verify_blob(repo, &work[i].entry.oid, vs);
		}
	}

done:
	oidset_insert(&vs->trusted_trees, new_tree_oid);
	oidset_insert(&vs->expanded_trees, new_tree_oid);
	free_tree_buffer(tree);
	for (i = 0; i < nr_work; i++)
		oid_array_clear(&work[i].parent_trees);
	free(work);
}

static void verify_commit_tree(struct repository *repo,
			       struct commit *commit,
			       struct verify_state *vs)
{
	struct oid_array base_trees = OID_ARRAY_INIT;
	struct commit_list *p;

	for (p = commit->parents; p; p = p->next) {
		const struct object_id *tree_oid;
		parse_commit_or_die(p->item);
		tree_oid = get_commit_tree_oid(p->item);
		oidset_insert(&vs->trusted_trees, tree_oid);
		oid_array_append(&base_trees, tree_oid);
	}

	verify_tree(repo, get_commit_tree_oid(commit),
		    &base_trees, vs, 0);
	oid_array_clear(&base_trees);
}

void verify_commits_incremental(struct repository *repo,
				struct commit_list **commits,
				int exclude_promisor_objects)
{
	struct verify_state vs = { 0 };
	struct commit_list *iter;
	unsigned nr_before;

	vs.exclude_promisor_objects = exclude_promisor_objects;

	nr_before = commit_list_count(*commits);
	sort_in_topological_order(commits, REV_SORT_IN_GRAPH_ORDER);
	if (commit_list_count(*commits) < nr_before)
		die(_("cycle detected in incoming commit graph"));

	*commits = commit_list_reverse(*commits);

	for (iter = *commits; iter; iter = iter->next)
		verify_commit_tree(repo, iter->item, &vs);

	oidset_clear(&vs.trusted_trees);
	oidset_clear(&vs.trusted_blobs);
	oidset_clear(&vs.expanded_trees);
	trace2_data_intmax("connectivity", repo,
			   "trees_loaded", vs.trees_loaded);
	trace2_data_intmax("connectivity", repo,
			   "blobs_checked", vs.blobs_checked);
}
