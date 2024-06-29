#ifndef GIT_LIST_OBJECTS_FILTER_EXTENSIONS_H
#define GIT_LIST_OBJECTS_FILTER_EXTENSIONS_H

/**
 * The List-Objects-Filter Extensions API can be used to develop filter
 * extensions for git-upload-pack/git-rev-list/etc.
 *
 * See contrib/filter-extensions/README.md for more details and examples.
 *
 * The API defines three functions to implement a filter operation. Note that
 * each filter implementing this API must compiled into Git as a static library.
 * There is some plumbing in the Makefile to help with this via
 * FILTER_EXTENSIONS.
 *
 * 1. You write a filter and compile it into your custom build of git.
 *    See list_objects_filter_ext_filter_fn.
 * 2. A filter request is received that specifically names the filter extension
 *    that you have written, ie: "--filter=extension:<name>[=<arg>]"
 * 3. Your list_objects_filter_ext_init_fn() is called.
 * 4. Your list_objects_filter_ext_filter_fn() is called for each object
 *    at least once.
 * 5. Your list_objects_filter_ext_free_fn() is called.
 */

#include "list-objects-filter.h"


/* Whether to add or remove a specific object from any current omitset. */
enum list_objects_filter_omit {
       LOFO_KEEP = -1,
       LOFO_IGNORE = 0,
       LOFO_OMIT = 1,
};

/*
 * This is a corollary to `list_objects_filter__init()` and constructs the
 * filter, parsing and validating any user-provided `filter_arg` (via
 * `--filter=extension:<name>=<arg>`). Use `context` for any filter-allocated
 * context data.
 *
 * Return 0 on success and non-zero on error.
 */
typedef
int list_objects_filter_ext_init_fn(
    const struct repository *r,
    const char* filter_arg,
    void **context
);

/*
 * This is a corollary to `list_objects_filter__free()`, destroying the filter
 * and any filter-allocated context data.
 */
typedef
void list_objects_filter_ext_free_fn(
    const struct repository *r,
    void *context
);

/*
 * This is a corollary to `list_objects_filter__filter_object()`, and
 * decides how to handle the object `obj`.
 *
 * omit provides a flag determining whether to explicitly add or remove
 * the object from any current omitset.
 */
typedef
enum list_objects_filter_result list_objects_filter_ext_filter_fn(
	const struct repository *r,
	const enum list_objects_filter_situation filter_situation,
	struct object *obj,
	const char *pathname,
	const char *filename,
	enum list_objects_filter_omit *omit,
	void *context
);

/*
 * To implement a filter extension called "mine", you should define
 * a const struct filter_extension called filter_extension_mine,
 * in the following manner:
 *
 * const struct filter_extension filter_extension_mine = {
 *     "mine",
 *     &my_init_fn,
 *     &my_filter_object_fn,
 *     &my_free_fn
 * };
 *
 * See contrib/filter-extensions/README.md for more details and examples.
 */

struct filter_extension {
    const char *name;
    list_objects_filter_ext_init_fn* init_fn;
    list_objects_filter_ext_filter_fn* filter_object_fn;
    list_objects_filter_ext_free_fn* free_fn;
};

/*
 * The filter_extensions array is defined in list_objects_filter_extensions.c
 * which is generated at compile time from the FILTER_EXTENSIONS variable.
 */
extern const struct filter_extension *filter_extensions[];


#endif /* GIT_LIST_OBJECTS_FILTER_EXTENSIONS_H */
