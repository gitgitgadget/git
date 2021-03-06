#include "git-compat-util.h"
#include "approximate-picks.h"
#include "commit.h"
#include "environment.h"
#include "hex.h"
#include "repository.h"

void get_message_pick(struct commit *commit, int *is_revert,
		      struct commit **pick_of, struct commit **base)
{
	const char *message;
	const char *revert_msg = "This reverts commit ";
	const char *cherry_msg = "cherry picked from commit ";
	const char *revert_msg_base = "reversing\nchanges made to ";
	char *loc = NULL;
	struct object_id picked_oid;
	int find_base = 1;

	message = repo_logmsg_reencode(the_repository, commit, NULL,
				       get_log_output_encoding());
	if ((loc = strstr(message, cherry_msg))) {
		/* cherry-pick */
		*is_revert = 0;
		if (get_oid_hex(loc + strlen(cherry_msg), &picked_oid))
			/* Couldn't parse next text as object id */
			goto not_a_pick;
	} else if ((loc = strstr(message, revert_msg))) {
		/* revert */
		*is_revert = 1;
		if (get_oid_hex(loc + strlen(revert_msg), &picked_oid))
			/* Couldn't parse next text as object id */
			goto not_a_pick;
		if ((loc = strstr(message, revert_msg_base))) {
			/* revert relative to specified base */
			struct object_id tmpid;
			if (get_oid_hex(loc + strlen(revert_msg_base), &tmpid))
				/* Couldn't parse next text as object id */
				goto not_a_pick;
			*base = lookup_commit(the_repository, &tmpid);
			find_base = 0;
		}
	} else
		/* neither */
		goto not_a_pick;

	*pick_of = lookup_commit(the_repository, &picked_oid);
	if (repo_parse_commit_gently(the_repository, *pick_of, 1))
		/*
		 * Getting here means commit referenced a nonexistent commit
		 * for what it is a cherry-pick or revert of.  We treat this
		 * as not a cherry-pick or revert.
		 */
		goto not_a_pick;
	if (find_base) {
		struct commit_list *parents = (*pick_of)->parents;
		if (!parents) {
			*base = NULL;
			return;
		}
		if (parents->next)
			/*
			 * Getting here means that either a commit said it was
			 * a revert of a merge without telling us which parent,
			 * or it claimed it was a cherry-pick of a merge which
			 * makes no sense.
			 */
			goto not_a_pick;
		*base = parents->item;
	}
	parse_commit_or_die(*base);
	return;

 not_a_pick:
	*is_revert = -1;
	*pick_of = NULL;
	*base = NULL;
}
