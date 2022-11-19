#include "cache.h"
#include "metacommit.h"
#include "commit.h"
#include "change-table.h"
#include "refs.h"

void clear_metacommit_data(struct metacommit_data *state)
{
	oidcpy(&state->content, null_oid());
	oid_array_clear(&state->replace);
	oid_array_clear(&state->origin);
	state->abandoned = 0;
}

static void compute_default_change_name(struct commit *initial_commit,
					struct strbuf *result)
{
	struct strbuf default_name = STRBUF_INIT;
	const char *buffer;
	const char *subject;
	const char *eol;
	size_t len;
	buffer = get_commit_buffer(initial_commit, NULL);
	find_commit_subject(buffer, &subject);
	eol = strchrnul(subject, '\n');
	for (len = 0; subject < eol && len < 10; subject++, len++) {
		char next = *subject;
		if (isspace(next))
			continue;

		strbuf_addch(&default_name, next);
	}
	sanitize_refname_component(default_name.buf, result);
	unuse_commit_buffer(initial_commit, buffer);
}

/*
 * Computes a change name for a change rooted at the given initial commit. Good
 * change names should be memorable, unique, and easy to type. They are not
 * required to match the commit comment.
 */
static void compute_change_name(struct commit *initial_commit,
				struct strbuf *result)
{
	struct strbuf default_name = STRBUF_INIT;
	struct object_id unused;

	if (initial_commit)
		compute_default_change_name(initial_commit, &default_name);
	else
		BUG("initial commit is NULL");
	strbuf_addstr(result, "refs/metas/");
	strbuf_addbuf(result, &default_name);

	/* If there is already a change of this name, append a suffix */
	if (!read_ref(result->buf, &unused)) {
		int suffix = 2;
		size_t original_length = result->len;

		while (1) {
			strbuf_addf(result, "%d", suffix);
			if (read_ref(result->buf, &unused))
				break;
			strbuf_remove(result, original_length,
				      result->len - original_length);
			++suffix;
		}
	}

	strbuf_release(&default_name);
}

struct resolve_metacommit_context {
	struct change_table *active_changes;
	struct string_list *changes;
	struct oid_array *heads;
};

static int resolve_metacommit_callback(const char *refname, void *cb_data)
{
	struct resolve_metacommit_context *ctx = cb_data;
	struct change_head *chhead;

	chhead = get_change_head(ctx->active_changes, refname);

	if (ctx->changes)
		string_list_append(ctx->changes, refname)->util = &chhead->head;
	if (ctx->heads)
		oid_array_append(ctx->heads, &(chhead->head));

	return 0;
}

static void resolve_oids(struct change_table *active_changes,
			 struct string_list *to_advance,
			 const struct oid_array *input_array,
			 struct oid_array *output_array)
{
	struct resolve_metacommit_context ctx = { .active_changes =
							  active_changes,
						  .changes = to_advance,
						  .heads = output_array };
	size_t i;
	for (i = 0; i < input_array->nr; i++) {
		size_t old_number = output_array->nr;
		for_each_change_referencing(active_changes,
					    &(input_array->oid[i]),
					    resolve_metacommit_callback, &ctx);
		/* If no changes were found, use the unresolved value. */
		if (old_number == output_array->nr)
			oid_array_append(output_array, &(input_array->oid[i]));
	}
}

/*
 * Produces the final form of a metacommit based on the current change refs.
 */
static void resolve_metacommit(struct repository *repo,
			       struct change_table *active_changes,
			       const struct metacommit_data *to_resolve,
			       struct metacommit_data *resolved_output,
			       struct string_list *to_advance, int allow_append)
{
	size_t old_change_list_length = to_advance->nr;
	struct commit *content;

	oidcpy(&resolved_output->content, &to_resolve->content);
	resolved_output->abandoned = to_resolve->abandoned;

	/*
	 * First look for changes that point to any of the replacement edges in
	 * the metacommit. These will be the changes that get advanced by this
	 * metacommit.
	 */
	if (allow_append) {
		resolve_oids(active_changes, to_advance, &to_resolve->replace,
			     &resolved_output->replace);
	}

	resolve_oids(active_changes, NULL, &to_resolve->origin,
		     &resolved_output->origin);

	/*
	 * If no changes were advanced by this metacommit, we'll need to create
	 * a new one.
	 */
	if (to_advance->nr == old_change_list_length) {
		struct strbuf change_name;

		strbuf_init(&change_name, 80);

		content = lookup_commit_reference_gently(
			repo, &(to_resolve->content), 1);

		compute_change_name(content, &change_name);
		string_list_append(to_advance, change_name.buf);
		strbuf_release(&change_name);
	}
}

static void lookup_commits(struct repository *repo, struct oid_array *to_lookup,
			   struct commit_list **result)
{
	int i = to_lookup->nr;

	while (--i >= 0) {
		struct object_id *next = &(to_lookup->oid[i]);
		struct commit *commit =
			lookup_commit_reference_gently(repo, next, 1);
		commit_list_insert(commit, result);
	}
}

#define PARENT_TYPE_PREFIX "parent-type "

int write_metacommit(struct repository *repo, struct metacommit_data *state,
		     struct object_id *result)
{
	struct commit_list *parents = NULL;
	struct strbuf comment;
	size_t i;
	struct commit *content;

	strbuf_init(&comment,
		    strlen(PARENT_TYPE_PREFIX) + 1 +
			    2 * (state->origin.nr + state->replace.nr));
	lookup_commits(repo, &state->origin, &parents);
	lookup_commits(repo, &state->replace, &parents);
	content = lookup_commit_reference_gently(repo, &state->content, 1);
	if (!content) {
		strbuf_release(&comment);
		free_commit_list(parents);
		return -1;
	}
	commit_list_insert(content, &parents);

	strbuf_addstr(&comment, PARENT_TYPE_PREFIX);
	strbuf_addstr(&comment, state->abandoned ? "a" : "c");
	for (i = 0; i < state->replace.nr; i++)
		strbuf_addstr(&comment, " r");

	for (i = 0; i < state->origin.nr; i++)
		strbuf_addstr(&comment, " o");

	/* The parents list will be freed by this call. */
	commit_tree(comment.buf, comment.len, repo->hash_algo->empty_tree,
		    parents, result, NULL, NULL);

	strbuf_release(&comment);
	return 0;
}

/*
 * Returns true iff the given metacommit is abandoned, has one or more origin
 * parents, or has one or more replacement parents.
 */
static int is_nontrivial_metacommit(struct metacommit_data *state)
{
	return state->replace.nr || state->origin.nr || state->abandoned;
}

/*
 * Records the relationships described by the given metacommit in the
 * repository.
 *
 * If override_change is NULL (the default), an attempt will be made
 * to append to existing changes wherever possible instead of creating new ones.
 * If override_change is non-null, only the given change ref will be updated.
 *
 * The changes list is filled in with the list of change refs that were updated,
 * with the util pointers pointing to the old object IDS for those changes.
 * The object ID pointers all point to objects owned by the change_table and
 * will go out of scope when the change_table is destroyed.
 *
 * options is a bitwise combination of the UPDATE_OPTION_* flags.
 */
static int record_metacommit_withresult(
	struct repository *repo, struct change_table *chtable,
	const struct metacommit_data *metacommit, const char *override_change,
	int options, struct strbuf *err, struct string_list *changes)
{
	static const char *msg = "updating change";
	struct metacommit_data resolved_metacommit = METACOMMIT_DATA_INIT;
	struct object_id commit_target;
	struct ref_transaction *transaction = NULL;
	struct change_head *overridden_head;
	const struct object_id *old_head;

	size_t i;
	int ret = 0;
	int force = (options & UPDATE_OPTION_FORCE);

	resolve_metacommit(repo, chtable, metacommit, &resolved_metacommit,
			   changes, (options & UPDATE_OPTION_NOAPPEND) == 0);

	if (override_change) {
		string_list_clear(changes, 0);
		overridden_head = get_change_head(chtable, override_change);
		if (overridden_head) {
			/* This is an existing change */
			old_head = &overridden_head->head;
			if (!force) {
				if (!oid_array_readonly_contains(
					    &(resolved_metacommit.replace),
					    &overridden_head->head)) {
					/* Attempted non-fast-forward change */
					strbuf_addf(
						err,
						_("non-fast-forward update to '%s'"),
						override_change);
					ret = -1;
					goto cleanup;
				}
			}
		} else {
			/* ...then this is a newly-created change */
			old_head = null_oid();
		}

		/*
		 * The expected "current" head of the change is stored in the
		 * util pointer. Cast required because old_head is const*
		 */
		string_list_append(changes, override_change)->util =
			(void *)old_head;
	}

	if (is_nontrivial_metacommit(&resolved_metacommit)) {
		/*
		 * If there are any origin or replacement parents, create a new
		 * metacommit object. */
		if (write_metacommit(repo, &resolved_metacommit,
				     &commit_target) < 0) {
			ret = -1;
			goto cleanup;
		}
	} else {
		/*
		 * If the metacommit would only contain a content commit, point
		 * to the commit itself rather than creating a trivial
		 * metacommit.
		 */
		oidcpy(&commit_target, &(resolved_metacommit.content));
	}

	/*
	 * If a change already exists with this target and we're not forcing an
	 * update to some specific override_change && change, there's nothing to
	 * do.
	 */
	if (!override_change &&
	    change_table_has_change_referencing(chtable, &commit_target))
		/* Not an error */
		goto cleanup;

	transaction = ref_transaction_begin(err);

	/* Update the refs for each affected change */
	if (!transaction) {
		ret = -1;
	} else {
		for (i = 0; i < changes->nr; i++) {
			struct string_list_item *it = &changes->items[i];

			/*
			 * The expected current head of the change is stored in
			 * the util pointer. It is null if the change should be
			 * newly-created.
			 */
			if (it->util) {
				if (ref_transaction_update(
					    transaction, it->string,
					    &commit_target,
					    force ? NULL : it->util, 0, msg,
					    err))

					ret = -1;
			} else {
				if (ref_transaction_create(
					    transaction, it->string,
					    &commit_target, 0, msg, err))

					ret = -1;
			}
		}

		if (!ret)
			if (ref_transaction_commit(transaction, err))
				ret = -1;
	}

cleanup:
	ref_transaction_free(transaction);
	clear_metacommit_data(&resolved_metacommit);

	return ret;
}

int record_metacommit(struct repository *repo,
		      const struct metacommit_data *metacommit,
		      const char *override_change, int options,
		      struct strbuf *err, struct string_list *changes)
{
	struct change_table chtable;
	int result;

	change_table_init(&chtable);
	change_table_add_all_visible(&chtable, repo);

	result = record_metacommit_withresult(repo, &chtable, metacommit,
					      override_change, options, err,
					      changes);

	change_table_clear(&chtable);
	return result;
}

void modify_change(struct repository *repo, const struct object_id *old_commit,
		   const struct object_id *new_commit, struct strbuf *err)
{
	struct string_list changes = STRING_LIST_INIT_DUP;
	struct metacommit_data metacommit = METACOMMIT_DATA_INIT;

	oidcpy(&(metacommit.content), new_commit);
	oid_array_append(&(metacommit.replace), old_commit);

	record_metacommit(repo, &metacommit, NULL, 0, err, &changes);

	clear_metacommit_data(&metacommit);
	string_list_clear(&changes, 0);
}
