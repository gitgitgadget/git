#ifndef BLAME_TREE_H
#define BLAME_TREE_H

#include "hash.h"
#include "strvec.h"
#include "string-list.h"
#include "revision.h"
#include "commit.h"

struct blame_tree_options {
	struct object_id oid;
	const char *prefix;
	unsigned int recursive;
	struct strvec args;
};

#define BLAME_TREE_OPTIONS_INIT(...) { \
	.args = STRVEC_INIT, \
	__VA_ARGS__ \
}

void blame_tree_opts_release(struct blame_tree_options *bto);

struct blame_tree {
	struct string_list paths;
	struct rev_info rev;
	struct repository *repository;
};
#define BLAME_TREE_INIT { \
	.paths = STRING_LIST_INIT_DUP, \
	.rev = REV_INFO_INIT, \
}

void blame_tree_init(struct repository *r, struct blame_tree *bt,
		     const struct blame_tree_options *opts);

void blame_tree_release(struct blame_tree *);

typedef void (*blame_tree_fn)(const char *path, const struct commit *commit,
			      void *data);
int blame_tree_run(struct blame_tree *bt, blame_tree_fn cb, void *data);

#endif /* BLAME_TREE_H */
