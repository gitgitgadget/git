# Design Spec: Batch Blob Prefetch for `git cherry` in Partial Clones

## Problem

In a partial clone with `--filter=blob:none`, `git cherry` compares
commits using patch IDs.  Patch IDs are computed in two phases:

1. Header-only: hashes file paths and mode changes only (no blob reads)
2. Full: hashes actual diff content (requires reading blobs)

Phase 2 only runs when two commits have matching header-only IDs
(i.e. they modify the same set of files with the same modes).  This
is common — any two commits touching the same file(s) will collide.

When phase 2 needs a blob that isn't local, it triggers an on-demand
promisor fetch.  Each fetch is a separate network round-trip.  With
many collisions, this means many sequential fetches.

## Solution Overview

Add a preparatory pass before the existing comparison loop in
`cmd_cherry()` that:

1. Identifies which commit pairs will collide on header-only IDs
2. Collects all blob OIDs those commits will need
3. Batch-prefetches them in one fetch

After this pass, the existing comparison loop runs as before, but
all needed blobs are already local, so no on-demand fetches occur.

## Detailed Design

### 1. No struct changes to patch_id

The existing `struct patch_id` and `patch_id_neq()` are not
modified.  `is_null_oid()` remains the sentinel for "full ID not
yet computed".  No `has_full_patch_id` boolean, no extra fields.

Key insight: `init_patch_id_entry()` stores only `oidhash()` (the
first 4 bytes of the header-only ID) in the hashmap bucket key.
The real `patch_id_neq()` comparison function is invoked only when
`hashmap_get()` or `hashmap_get_next()` finds entries with a
matching oidhash — and that comparison triggers blob reads.

The prefetch needs to detect exactly those oidhash collisions
*without* triggering blob reads.  We achieve this by temporarily
swapping the hashmap's comparison function.

### 2. The prefetch function (in builtin/log.c)

This function takes the repository, the head-side commit list (as
built by the existing revision walk in `cmd_cherry()`), and the
patch_ids structure (which contains the upstream entries).

#### 2.1 Early exit

If the repository has no promisor remote, return immediately.
Use `repo_has_promisor_remote()` from promisor-remote.h.

#### 2.2 Swap in a trivial comparison function

Save `ids->patches.cmpfn` (the real `patch_id_neq`) and replace
it with a trivial function that always returns 0 ("equal").

```
static int patch_id_match(const void *unused_cmpfn_data,
                          const struct hashmap_entry *a,
                          const struct hashmap_entry *b,
                          const void *unused_keydata)
{
    return 0;
}
```

With this cmpfn in place, `hashmap_get()` and `hashmap_get_next()`
will match every entry in the same oidhash bucket — exactly the
same set that would trigger `patch_id_neq()` during normal lookup.
No blob reads occur because we never call the real comparison
function.

#### 2.3 For each head-side commit, probe for collisions

For each commit in the head-side list:

- Use `patch_id_iter_first(commit, ids)` to probe the upstream
  hashmap.  This handles `init_patch_id_entry()` + hashmap lookup
  internally.  With our swapped cmpfn, it returns any upstream
  entry whose oidhash matches — i.e. any entry that *would*
  trigger `patch_id_neq()` during the real comparison loop.
  (Merge commits are already handled — `patch_id_iter_first()`
  returns NULL for them via `patch_id_defined()`.)
- If there's a match: collect blob OIDs from the head-side commit
  (see section 3).
- Then walk `patch_id_iter_next()` to find ALL upstream entries
  in the same bucket.  For each, collect blob OIDs from that
  upstream commit too.  (Multiple upstream commits can share the
  same oidhash bucket.)
- Collect blob OIDs from the first upstream match too (from
  `patch_id_iter_first()`).

We need blobs from BOTH sides because `patch_id_neq()` computes
full patch IDs for both the upstream and head-side commit when
comparing.

#### 2.4 Restore the original comparison function

Set `ids->patches.cmpfn` back to the saved value (patch_id_neq).
This MUST happen before returning — the subsequent
`has_commit_patch_id()` loop needs the real comparison function.

#### 2.5 Batch prefetch

If the oidset is non-empty, populate an oid_array from it using
`oidset_iter_first()`/`oidset_iter_next()`, then call
`promisor_remote_get_direct(repo, oid_array.oid, oid_array.nr)`.

This is a single network round-trip regardless of how many blobs.

#### 2.6 Cleanup

Free the oid_array and the oidset.

### 3. Collecting blob OIDs from a commit (helper function)

Given a commit, enumerate the blobs its diff touches.  Takes an
oidset to insert into (provides automatic dedup — consecutive
commits often share blob OIDs, e.g. B:foo == C^:foo when C's
parent is B).

- Compute the diff: `diff_tree_oid()` for commits with a parent,
  `diff_root_tree_oid()` for root commits.  Then `diffcore_std()`.
- These populate the global `diff_queued_diff` queue.
- For each filepair in the queue:
  - Check the userdiff driver for the file path.  If the driver
    explicitly declares the file as binary (`drv->binary != -1`),
    skip it.  Reason: patch-ID uses `oid_to_hex()` for binary
    files (see diff.c around line 6652) and never reads the blob.
    Use `userdiff_find_by_path()` (NOT `diff_filespec_load_driver`
    which is static in diff.c).
  - For both sides of the filepair (p->one and p->two): if the
    side is valid (`DIFF_FILE_VALID`) and has a non-null OID,
    check the dedup oidset — `oidset_insert()` handles dedup
    automatically (returns 1 if newly inserted, 0 if duplicate).
- Clear the diff queue with `diff_queue_clear()` (from diffcore.h,
  not diff.h).

Note on `drv->binary`: The value -1 means "not set" (auto-detect
at read time by reading the blob); 0 means explicitly text (will
be diffed, blob reads needed); positive means explicitly binary
(patch-ID uses `oid_to_hex()`, no blob read needed).

The correct skip condition is `drv && drv->binary > 0` — skip
only known-binary files.  Do NOT use `drv->binary != -1`, which
would also skip explicitly-text files that DO need blob reads.
(The copilot reference implementation uses `!= -1`, which is
technically wrong but harmless in practice since explicit text
attributes are rare.)

### 4. Call site in cmd_cherry()

Insert the call between the revision walk loop (which builds the
head-side commit list) and the comparison loop (which calls
`has_commit_patch_id()`).

### 5. Required includes in builtin/log.c

- promisor-remote.h  (for repo_has_promisor_remote,
                       promisor_remote_get_direct)
- userdiff.h         (for userdiff_find_by_path)
- oidset.h           (for oidset used in blob OID dedup)
- diffcore.h         (for diff_queue_clear)

## Edge Cases

- No promisor remote: early return, zero overhead
- No collisions: probes the hashmap for each head-side commit but
  finds no bucket matches, no blobs collected, no fetch issued
- Merge commits in head-side list: skipped (no patch ID defined)
- Root commits (no parent): use diff_root_tree_oid instead of
  diff_tree_oid
- Binary files (explicit driver): skipped, patch-ID doesn't read
  them
- The cmpfn swap approach matches at oidhash granularity (4 bytes),
  which is exactly what the hashmap itself uses to trigger
  patch_id_neq().  This means we prefetch for every case the real
  code would trigger, plus rare false-positive oidhash collisions
  (harmless: we fetch a few extra blobs that won't end up being
  compared).  No under-fetching is possible.

## Testing

See t/t3500-cherry.sh on the copilot-faster-partial-clones branch
for two tests:

Test 5: "cherry batch-prefetches blobs in partial clone"
  - Creates server with 3 upstream + 3 head-side commits modifying
    the same file (guarantees collisions)
  - Clones with --filter=blob:none
  - Runs `git cherry` with GIT_TRACE2_PERF
  - Asserts exactly 1 fetch (batch) instead of 6 (individual)

Test 6: "cherry prefetch omits blobs for cherry-picked commits"
  - Creates a cherry-pick scenario (divergent branches, shared
    commit cherry-picked to head side)
  - Verifies `git cherry` correctly identifies the cherry-picked
    commit as "-" and head-only commits as "+"
  - Important: the head side must diverge before the cherry-pick
    so the cherry-pick creates a distinct commit object (otherwise
    the commit hash is identical and it's in the symmetric
    difference, not needing patch-ID comparison at all)
