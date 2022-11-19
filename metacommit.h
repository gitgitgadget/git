#ifndef METACOMMIT_H
#define METACOMMIT_H

#include "hash.h"
#include "oid-array.h"
#include "repository.h"
#include "string-list.h"

/* If specified, non-fast-forward changes are permitted. */
#define UPDATE_OPTION_FORCE 0x0001
/**
 * If specified, no attempt will be made to append to existing changes.
 * Normally, if a metacommit points to a commit in its replace or origin
 * list and an existing change points to that same commit as its content, the
 * new metacommit will attempt to append to that same change. This may replace
 * the commit parent with one or more metacommits from the head of the appended
 * changes. This option disables this behavior, and will always create a new
 * change rather than reusing existing changes.
 */
#define UPDATE_OPTION_NOAPPEND 0x0002

/* Metacommit Data */

struct metacommit_data {
	struct object_id content;
	struct oid_array replace;
	struct oid_array origin;
	int abandoned;
};

#define METACOMMIT_DATA_INIT \
	{                    \
		0            \
	}

extern void clear_metacommit_data(struct metacommit_data *state);

/**
 * Records the relationships described by the given metacommit in the
 * repository.
 *
 * If override_change is NULL (the default), an attempt will be made
 * to append to existing changes wherever possible instead of creating new ones.
 * If override_change is non-null, only the given change ref will be updated.
 *
 * options is a bitwise combination of the UPDATE_OPTION_* flags.
 */
int record_metacommit(struct repository *repo,
		      const struct metacommit_data *metacommit,
		      const char *override_change, int options,
		      struct strbuf *err, struct string_list *changes);

/**
 * Should be invoked after a command that has "modify" semantics - commands that
 * create a new commit based on an old commit and treat the new one as a
 * replacement for the old one. This method records the replacement in the
 * change graph, such that a future evolve operation will rebase children of
 * the old commit onto the new commit.
 */
void modify_change(struct repository *repo, const struct object_id *old_commit,
		   const struct object_id *new_commit, struct strbuf *err);

/**
 * Creates a new metacommit object with the given content. Writes the object
 * id of the newly-created commit to result.
 */
int write_metacommit(struct repository *repo, struct metacommit_data *state,
		     struct object_id *result);

#endif
