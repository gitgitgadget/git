#ifndef CHECKOUT_H
#define CHECKOUT_H

#include "cache.h"

/*
 * Check if the branch name uniquely matches a branch name on a remote
 * tracking branch.  Return the name of the remote if such a branch
 * exists, NULL otherwise.
 */
extern const char *unique_tracking_name(const char *name,
					struct object_id *oid,
					int *dwim_remotes_matched);

int detach_head_to(struct object_id *oid, const char *action,
		   const char *reflog_message);

#endif /* CHECKOUT_H */
