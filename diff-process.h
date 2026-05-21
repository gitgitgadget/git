#ifndef DIFF_PROCESS_H
#define DIFF_PROCESS_H

struct userdiff_driver;
struct xdl_hunk;

/*
 * Query a diff process for hunks describing the changes
 * between old_buf and new_buf.
 *
 * The backend is a long-running subprocess configured via
 * diff.<driver>.process.  It receives file content via
 * pkt-line and returns hunks with 1-based line numbers.
 *
 * On success, sets *hunks_out and *nr_hunks_out to a newly allocated
 * array (caller must free) and returns 0.
 *
 * On failure, returns -1.  The caller should fall back to the
 * builtin diff algorithm.
 */
int diff_process_get_hunks(struct userdiff_driver *drv,
			   const char *path,
			   const char *old_buf, long old_size,
			   const char *new_buf, long new_size,
			   struct xdl_hunk **hunks_out,
			   size_t *nr_hunks_out);

#endif /* DIFF_PROCESS_H */
