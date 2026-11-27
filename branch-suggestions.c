#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "branch-suggestions.h"
#include "refs.h"
#include "string-list.h"
#include "levenshtein.h"
#include "gettext.h"
#include "repository.h"
#include "strbuf.h"

#define SIMILARITY_FLOOR 7
#define SIMILAR_ENOUGH(x) ((x) < SIMILARITY_FLOOR)
#define MAX_SUGGESTIONS 5

struct branch_suggestion_cb {
	const char *attempted_name;
	struct string_list *suggestions;
};

static int collect_branch_cb(const char *refname, const char *referent UNUSED,
			     const struct object_id *oid UNUSED,
			     int flags UNUSED, void *cb_data)
{
	struct branch_suggestion_cb *cb = cb_data;
	const char *branch_name;

	/* Since we're using refs_for_each_ref_in with "refs/heads/",
	 * the refname might already be stripped or might still have the prefix */
	if (starts_with(refname, "refs/heads/")) {
		branch_name = refname + strlen("refs/heads/");
	} else {
		branch_name = refname;
	}

	/* Skip the attempted name itself */
	if (!strcmp(branch_name, cb->attempted_name))
		return 0;

	string_list_append(cb->suggestions, branch_name);
	return 0;
}

void suggest_similar_branch_names(const char *attempted_name)
{
	struct string_list branches = STRING_LIST_INIT_DUP;
	struct branch_suggestion_cb cb_data;
	size_t i;
	int best_similarity = INT_MAX;
	int suggestion_count = 0;

	cb_data.attempted_name = attempted_name;
	cb_data.suggestions = &branches;

	/* Collect all local branch names */
	refs_for_each_ref_in(get_main_ref_store(the_repository), "refs/heads/",
			     collect_branch_cb, &cb_data);

	if (!branches.nr)
		goto cleanup;

	/* Calculate Levenshtein distances */
	for (i = 0; i < branches.nr; i++) {
		const char *branch_name = branches.items[i].string;
		int distance;

		/* Give prefix matches a very good score */
		if (starts_with(branch_name, attempted_name)) {
			distance = 0;
		} else {
			distance = levenshtein(attempted_name, branch_name, 0, 2, 1, 3);
		}

		branches.items[i].util = (void *)(intptr_t)distance;

		if (distance < best_similarity)
			best_similarity = distance;
	}

	/* Only show suggestions if they're similar enough */
	if (!SIMILAR_ENOUGH(best_similarity))
		goto cleanup;

	/* Count and display similar branches */
	for (i = 0; i < branches.nr && suggestion_count < MAX_SUGGESTIONS; i++) {
		int distance = (int)(intptr_t)branches.items[i].util;

		if (distance <= best_similarity && SIMILAR_ENOUGH(distance)) {
			if (suggestion_count == 0) {
				fprintf(stderr, "%s\n", _("hint: Did you mean one of these?"));
			}
			fprintf(stderr, "hint:     %s\n", branches.items[i].string);
			suggestion_count++;
		}
	}

cleanup:
	string_list_clear(&branches, 0);
}
