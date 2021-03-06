#ifndef APPROXIMATE_PICKS_H
#define APPROXIMATE_PICKS_H

struct commit;

/*
 * Determine if the commit message for commit claims this is a cherry-pick or
 * revert of another commit, which exists in this repo.  Possible return
 * values:
 *
 * If commit message claims it is a cherry-pick:
 *   *is_revert: 0
 *   *pick_of:   points to what commit is a cherry-pick of
 *   *base:      points to parent of *pick_of (possibly NULL)
 * If commit message claims it is a revert:
 *   *is_revert: 1
 *   *pick_of:   points to what commit is a revert of
 *   *base:      points to appropriate parent of *pick_of (possibly NULL)
 * Otherwise:
 *   *is_revert: -1
 *   *pick_of:   NULL
 *   *base:      NULL
 */
void get_message_pick(struct commit *commit, int *is_revert,
		      struct commit **pick_of, struct commit **base);

#endif /* APPROXIMATE_PICKS_H */
