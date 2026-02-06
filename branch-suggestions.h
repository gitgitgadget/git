#ifndef BRANCH_SUGGESTIONS_H
#define BRANCH_SUGGESTIONS_H

/**
 * Suggest similar branch names when a branch checkout fails.
 * This function analyzes local branches and suggests ones that are
 * similar to the attempted branch name using fuzzy matching.
 */
void suggest_similar_branch_names(const char *attempted_name);

#endif /* BRANCH_SUGGESTIONS_H */
