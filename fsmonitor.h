#ifndef FSMONITOR_H
#define FSMONITOR_H

extern struct trace_key trace_fsmonitor;

/*
 * Read the fsmonitor index extension and (if configured) restore the
 * CE_FSMONITOR_VALID state.
 */
extern int read_fsmonitor_extension(struct index_state *istate, const void *data, unsigned long sz);

/*
 * Fill the fsmonitor_dirty ewah bits with their state from the index,
 * before it is split during writing.
 */
extern void fill_fsmonitor_bitmap(struct index_state *istate);

/*
 * Write the CE_FSMONITOR_VALID state into the fsmonitor index
 * extension.  Reads from the fsmonitor_dirty ewah in the index.
 */
extern void write_fsmonitor_extension(struct strbuf *sb, struct index_state *istate);

/*
 * Add/remove the fsmonitor index extension
 */
extern void add_fsmonitor(struct index_state *istate);
extern void remove_fsmonitor(struct index_state *istate);

/*
 * Add/remove the fsmonitor index extension as necessary based on the current
 * core.fsmonitor setting.
 */
extern void tweak_fsmonitor(struct index_state *istate);

/*
 * Run the configured fsmonitor integration script and clear the
 * CE_FSMONITOR_VALID bit for any files returned as dirty.  Also invalidate
 * any corresponding untracked cache directory structures. Optimized to only
 * run the first time it is called.
 */
extern void refresh_fsmonitor(struct index_state *istate);

/*
 * Set the given cache entries CE_FSMONITOR_VALID bit. This should be
 * called any time the cache entry has been updated to reflect the
 * current state of the file on disk.
 */
extern void mark_fsmonitor_valid(struct cache_entry *ce);

/*
 * Clear the given cache entry's CE_FSMONITOR_VALID bit and invalidate
 * any corresponding untracked cache directory structures. This should
 * be called any time git creates or modifies a file that should
 * trigger an lstat() or invalidate the untracked cache for the
 * corresponding directory
 */
extern void mark_fsmonitor_invalid(struct index_state *istate, struct cache_entry *ce);

#endif
