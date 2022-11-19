#include "cache.h"
#include "change-table.h"
#include "commit.h"
#include "ref-filter.h"
#include "metacommit-parser.h"

void change_table_init(struct change_table *table)
{
	memset(table, 0, sizeof(*table));
	mem_pool_init(&table->memory_pool, 0);
	oidmap_init(&table->oid_to_metadata_index, 0);
	strmap_init(&table->refname_to_change_head);
}

static void change_list_clear(struct change_list *change_list)
{
	strset_clear(&change_list->refnames);
}

static void
commit_change_list_entry_clear(struct commit_change_list_entry *entry)
{
	change_list_clear(&entry->changes);
}

void change_table_clear(struct change_table *table)
{
	struct oidmap_iter iter;
	struct commit_change_list_entry *next;
	for (next = oidmap_iter_first(&table->oid_to_metadata_index, &iter);
	     next; next = oidmap_iter_next(&iter)) {
		commit_change_list_entry_clear(next);
	}

	oidmap_free(&table->oid_to_metadata_index, 0);
	strmap_clear(&table->refname_to_change_head, 0);
	mem_pool_discard(&table->memory_pool, 0);
}

static void add_head_to_commit(struct change_table *table,
			       const struct object_id *to_add,
			       const char *refname)
{
	struct commit_change_list_entry *entry;

	entry = oidmap_get(&table->oid_to_metadata_index, to_add);
	if (!entry) {
		entry = mem_pool_calloc(&table->memory_pool, 1, sizeof(*entry));
		oidcpy(&entry->entry.oid, to_add);
		strset_init(&entry->changes.refnames);
		oidmap_put(&table->oid_to_metadata_index, entry);
	}
	strset_add(&entry->changes.refnames, refname);
}

void change_table_add(struct change_table *table, const char *refname,
		      struct commit *to_add)
{
	struct change_head *new_head;
	int metacommit_type;

	new_head = mem_pool_calloc(&table->memory_pool, 1, sizeof(*new_head));

	oidcpy(&new_head->head, &to_add->object.oid);

	metacommit_type = get_metacommit_content(to_add, &new_head->content);
	/*
	 * If to_add is not a metacommit then the content is to_add itself,
	 * otherwise it will have been set by the call to
	 * get_metacommit_content.
	 */
	if (metacommit_type == METACOMMIT_TYPE_NONE)
		oidcpy(&new_head->content, &to_add->object.oid);
	new_head->abandoned = (metacommit_type == METACOMMIT_TYPE_ABANDONED);
	new_head->remote = starts_with(refname, "refs/remote/");
	new_head->hidden = starts_with(refname, "refs/hiddenmetas/");

	strmap_put(&table->refname_to_change_head, refname, new_head);

	if (!oideq(&new_head->content, &new_head->head)) {
		/* Also remember to link between refname and the content oid */
		add_head_to_commit(table, &new_head->content, refname);
	}
	add_head_to_commit(table, &new_head->head, refname);
}

static void change_table_add_matching_filter(struct change_table *table,
					     struct repository *repo,
					     struct ref_filter *filter)
{
	int i;
	struct ref_array matching_refs = { 0 };

	filter_refs(&matching_refs, filter, filter->kind);

	/*
	 * Determine the object id for the latest content commit for each
	 * change. Fetch the commit at the head of each change ref. If it's a
	 * normal commit, that's the commit we want. If it's a metacommit,
	 * locate its content parent and use that.
	 */

	for (i = 0; i < matching_refs.nr; i++) {
		struct ref_array_item *item = matching_refs.items[i];
		struct commit *commit;

		commit = lookup_commit_reference(repo, &item->objectname);
		if (!commit) {
			BUG("Invalid commit for refs/meta: %s", item->refname);
		}
		change_table_add(table, item->refname, commit);
	}

	ref_array_clear(&matching_refs);
}

void change_table_add_all_visible(struct change_table *table,
				  struct repository *repo)
{
	struct ref_filter filter = { 0 };
	const char *name_patterns[] = { NULL };
	filter.kind = FILTER_REFS_CHANGES;
	filter.name_patterns = name_patterns;

	change_table_add_matching_filter(table, repo, &filter);
}

static int return_true_callback(const char *refname, void *cb_data)
{
	return 1;
}

int change_table_has_change_referencing(
	struct change_table *table,
	const struct object_id *referenced_commit_id)
{
	return for_each_change_referencing(table, referenced_commit_id,
					   return_true_callback, NULL);
}

int for_each_change_referencing(struct change_table *table,
				const struct object_id *referenced_commit_id,
				each_change_fn fn, void *cb_data)
{
	int ret = 0;
	struct commit_change_list_entry *ccl_entry;
	struct hashmap_iter iter;
	struct strmap_entry *entry;

	ccl_entry =
		oidmap_get(&table->oid_to_metadata_index, referenced_commit_id);
	/*
	 * If this commit isn't referenced by any changes, it won't be in the
	 * map
	 */
	if (!ccl_entry)
		return 0;

	strset_for_each_entry(&ccl_entry->changes.refnames, &iter, entry)
	{
		ret = fn(entry->key, cb_data);
		if (ret != 0)
			break;
	}
	return ret;
}

struct change_head *get_change_head(struct change_table *table,
				    const char *refname)
{
	return strmap_get(&table->refname_to_change_head, refname);
}
