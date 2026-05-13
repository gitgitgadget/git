/*
 *  LibXDiff by Davide Libenzi ( File Differential Library )
 *  Copyright (C) 2003  Davide Libenzi
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, see
 *  <http://www.gnu.org/licenses/>.
 *
 *  Davide Libenzi <davidel@xmailserver.org>
 *
 */

#if !defined(XDIFF_H)
#define XDIFF_H

#ifdef __cplusplus
extern "C" {
#endif /* #ifdef __cplusplus */

/* xpparm_t.flags */
#define XDF_NEED_MINIMAL (1 << 0)

#define XDF_IGNORE_WHITESPACE (1 << 1)
#define XDF_IGNORE_WHITESPACE_CHANGE (1 << 2)
#define XDF_IGNORE_WHITESPACE_AT_EOL (1 << 3)
#define XDF_IGNORE_CR_AT_EOL (1 << 4)
#define XDF_WHITESPACE_FLAGS (XDF_IGNORE_WHITESPACE | \
			      XDF_IGNORE_WHITESPACE_CHANGE | \
			      XDF_IGNORE_WHITESPACE_AT_EOL | \
			      XDF_IGNORE_CR_AT_EOL)

#define XDF_IGNORE_BLANK_LINES (1 << 7)

#define XDF_PATIENCE_DIFF (1 << 14)
#define XDF_HISTOGRAM_DIFF (1 << 15)
#define XDF_DIFF_ALGORITHM_MASK (XDF_PATIENCE_DIFF | XDF_HISTOGRAM_DIFF | XDF_NEED_MINIMAL)
#define XDF_DIFF_ALG(x) ((x) & XDF_DIFF_ALGORITHM_MASK)

#define XDF_INDENT_HEURISTIC (1 << 23)

/* xdemitconf_t.flags */
#define XDL_EMIT_FUNCNAMES (1 << 0)
#define XDL_EMIT_NO_HUNK_HDR (1 << 1)
#define XDL_EMIT_FUNCCONTEXT (1 << 2)

/* merge simplification levels */
#define XDL_MERGE_MINIMAL 0
#define XDL_MERGE_EAGER 1
#define XDL_MERGE_ZEALOUS 2
#define XDL_MERGE_ZEALOUS_ALNUM 3

/* merge favor modes */
#define XDL_MERGE_FAVOR_OURS 1
#define XDL_MERGE_FAVOR_THEIRS 2
#define XDL_MERGE_FAVOR_UNION 3

/* merge output styles */
#define XDL_MERGE_DIFF3 1
#define XDL_MERGE_ZEALOUS_DIFF3 2

typedef struct s_mmfile {
	char *ptr;
	long size;
} mmfile_t;

typedef struct s_mmbuffer {
	char *ptr;
	long size;
} mmbuffer_t;

typedef struct s_xpparam {
	unsigned long flags;

	/* -I<regex> */
	regex_t **ignore_regex;
	size_t ignore_regex_nr;

	/* See Documentation/diff-options.adoc. */
	char **anchors;
	size_t anchors_nr;
} xpparam_t;

typedef struct s_xdemitcb {
	void *priv;
	int (*out_hunk)(void *,
			long old_begin, long old_nr,
			long new_begin, long new_nr,
			const char *func, long funclen);
	int (*out_line)(void *, mmbuffer_t *, int);
} xdemitcb_t;

typedef long (*find_func_t)(const char *line, long line_len, char *buffer, long buffer_size, void *priv);

typedef int (*xdl_emit_hunk_consume_func_t)(long start_a, long count_a,
					    long start_b, long count_b,
					    void *cb_data);

typedef struct s_xdemitconf {
	long ctxlen;
	long interhunkctxlen;
	unsigned long flags;
	find_func_t find_func;
	void *find_func_priv;
	xdl_emit_hunk_consume_func_t hunk_func;
} xdemitconf_t;

typedef struct s_bdiffparam {
	long bsize;
} bdiffparam_t;


#define xdl_malloc(x) xmalloc(x)
#define xdl_calloc(n, sz) xcalloc(n, sz)
#define xdl_free(ptr) free(ptr)
#define xdl_realloc(ptr,x) xrealloc(ptr,x)

void *xdl_mmfile_first(mmfile_t *mmf, long *size);
long xdl_mmfile_size(mmfile_t *mmf);

int xdl_diff(mmfile_t *mf1, mmfile_t *mf2, xpparam_t const *xpp,
	     xdemitconf_t const *xecfg, xdemitcb_t *ecb);

/*
 * A half-open byte range identifying one conflict-marker block
 * (from the leading "<<<<<<<" line through the trailing newline of
 * the matching ">>>>>>>" line) inside a buffer produced by
 * xdl_merge.
 */
struct xdl_conflict_interval {
	long start;	/* offset of the first byte of the opener line */
	long end;	/* offset one past the trailing newline of the closer line */
};

/*
 * A growable list of xdl_conflict_interval records, in order of
 * appearance in the produced buffer. Initialize all-zero; release
 * with xdl_conflict_intervals_release.
 */
struct xdl_conflict_intervals {
	struct xdl_conflict_interval *items;
	long nr;
	long alloc;
};

void xdl_conflict_intervals_release(struct xdl_conflict_intervals *intervals);

typedef struct s_xmparam {
	xpparam_t xpp;
	int marker_size;
	int level;
	int favor;
	int style;
	const char *ancestor;	/* label for orig */
	const char *file1;	/* label for mf1 */
	const char *file2;	/* label for mf2 */
	/*
	 * Side channel for detecting a fresh conflict introduced by
	 * mf2 (side2) that has no counterpart in orig (the merge
	 * base). Useful when xdl_merge is the outer merge of a
	 * replayed merge commit: orig and mf2 may themselves be the
	 * output of earlier xdl_merge calls that emitted
	 * conflict-marker hunks into their buffers, and the normal
	 * three-way merge cannot tell those literal marker bytes
	 * apart from any other text.
	 *
	 * out_intervals (output): when non-NULL, xdl_merge populates
	 * it with the byte range of every conflict-marker hunk it
	 * writes into the result buffer. Use this on the inner
	 * merges whose output will later be fed to an outer merge as
	 * orig or mf2. The caller initializes the struct all-zero
	 * and releases it with xdl_conflict_intervals_release.
	 *
	 * in_orig_intervals, in_side2_intervals (inputs): when
	 * in_side2_intervals is non-NULL, xdl_merge counts every
	 * recorded mf2 hunk that has no byte-equal counterpart in
	 * orig as an additional conflict in the returned status; if
	 * in_orig_intervals is NULL, every mf2 hunk is counted. Pass
	 * NULL on side2 to opt out.
	 */
	struct xdl_conflict_intervals *out_intervals;
	struct xdl_conflict_intervals *in_orig_intervals;
	struct xdl_conflict_intervals *in_side2_intervals;
} xmparam_t;

#define DEFAULT_CONFLICT_MARKER_SIZE 7

int xdl_merge(mmfile_t *orig, mmfile_t *mf1, mmfile_t *mf2,
		xmparam_t const *xmp, mmbuffer_t *result);

#ifdef __cplusplus
}
#endif /* #ifdef __cplusplus */

#endif /* #if !defined(XDIFF_H) */
