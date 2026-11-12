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

#include "xinclude.h"


#define XDL_KPDIS_RUN 4
#define XDL_MAX_EQLIMIT 1024
#define XDL_SIMSCAN_WINDOW 100
#define XDL_GUESS_NLINES1 256
#define XDL_GUESS_NLINES2 20

#define DISCARD 0
#define KEEP 1
#define INVESTIGATE 2

typedef struct s_xdlclass {
	uint64_t line_hash;
	const uint8_t *ptr;
	size_t size;
	long len1, len2;
} xdlclass_t;

/*
 * A slot holds a class index plus one; zero denotes an empty slot.
 * Keep hashes in the contiguous class array rather than in each slot.
 */
typedef uint32_t xdlslot_t;

typedef struct s_xdlclassifier {
	unsigned int tbits;
	size_t tmask;
	xdlslot_t *table;
	xdlclass_t *rcrecs;
	long alloc;
	long count;
	long flags;
} xdlclassifier_t;


#define XDL_CLASS_MAX ((long)0x7ffffffe)

/*
 * SIZE estimates the record count; TSIZE estimates the number of classes
 * actually inserted into the table. Both arrays grow when necessary.
 */
static int xdl_init_classifier_sized(xdlclassifier_t *cf, long size, long tsize,
				     long flags) {
	unsigned int bits;

	cf->flags = flags;
	bits = xdl_hashbits((unsigned int)tsize) + 1;
	if (bits > 31)
		bits = 31;
	cf->tbits = bits;
	cf->tmask = ((size_t)1 << bits) - 1;

	if (!XDL_CALLOC_ARRAY(cf->table, cf->tmask + 1))
		return -1;

	cf->alloc = size;
	if (!XDL_ALLOC_ARRAY(cf->rcrecs, cf->alloc)) {
		xdl_free(cf->table);
		cf->table = NULL;
		return -1;
	}

	cf->count = 0;
	return 0;
}


static void xdl_free_classifier(xdlclassifier_t *cf) {
	xdl_free(cf->rcrecs);
	xdl_free(cf->table);
}


static inline size_t xdl_slot_index(xdlclassifier_t *cf, uint64_t line_hash) {
	return (size_t)(line_hash >> (64 - cf->tbits));
}


static int xdl_grow_table(xdlclassifier_t *cf) {
	xdlslot_t *ntab;
	unsigned int nbits = cf->tbits + 1;
	size_t nmask;
	long i;

	if (nbits > 32 || nbits >= sizeof(size_t) * CHAR_BIT)
		return -1;
	nmask = ((size_t)1 << nbits) - 1;
	if (!XDL_CALLOC_ARRAY(ntab, nmask + 1))
		return -1;

	cf->tbits = nbits;
	cf->tmask = nmask;
	for (i = 0; i < cf->count; i++) {
		size_t hi = xdl_slot_index(cf, cf->rcrecs[i].line_hash);

		while (ntab[hi])
			hi = (hi + 1) & nmask;
		ntab[hi] = (xdlslot_t)(i + 1);
	}
	xdl_free(cf->table);
	cf->table = ntab;
	return 0;
}


static inline int xdl_bytes_equal(uint8_t const *a, uint8_t const *b, size_t n) {
	uint64_t x, y;
	size_t i;

	if (n < sizeof(uint64_t)) {
		for (i = 0; i < n; i++)
			if (a[i] != b[i])
				return 0;
		return 1;
	}
	for (i = 0; i + sizeof(uint64_t) < n; i += sizeof(uint64_t)) {
		memcpy(&x, a + i, sizeof(x));
		memcpy(&y, b + i, sizeof(y));
		if (x != y)
			return 0;
	}
	memcpy(&x, a + n - sizeof(x), sizeof(x));
	memcpy(&y, b + n - sizeof(y), sizeof(y));
	return x == y;
}


static int xdl_classify_record(unsigned int pass, xdlclassifier_t *cf,
			       xrecord_t *rec, uint64_t line_hash) {
	size_t hi, mask = cf->tmask;
	xdlslot_t slot;
	xdlslot_t *table = cf->table;
	xdlclass_t *rcrec;
	long idx;

	hi = xdl_slot_index(cf, line_hash);
	while ((slot = table[hi])) {
		rcrec = &cf->rcrecs[slot - 1];
		if (rcrec->line_hash == line_hash &&
		    (cf->flags & XDF_WHITESPACE_FLAGS
		     ? xdl_recmatch((const char *)rcrec->ptr, (long)rcrec->size,
				    (const char *)rec->ptr, (long)rec->size, cf->flags)
		     : (rcrec->size == rec->size &&
			xdl_bytes_equal(rcrec->ptr, rec->ptr, rec->size))))
			goto found;
		hi = (hi + 1) & mask;
	}

	if (cf->count >= XDL_CLASS_MAX)
		return -1;
	idx = cf->count++;
	if (XDL_ALLOC_GROW(cf->rcrecs, cf->count, cf->alloc))
		return -1;
	rcrec = &cf->rcrecs[idx];
	rcrec->line_hash = line_hash;
	rcrec->ptr = rec->ptr;
	rcrec->size = rec->size;
	rcrec->len1 = rcrec->len2 = 0;
	table[hi] = (xdlslot_t)(idx + 1);
	if ((size_t)cf->count * 2 > mask + 1 && xdl_grow_table(cf) < 0)
		return -1;

found:
	(pass == 1) ? rcrec->len1++ : rcrec->len2++;
	rec->minimal_perfect_hash = (size_t)(rcrec - cf->rcrecs);
	return 0;
}


#define XDL_HMUL 0x9e3779b97f4a7c15ULL

static inline uint64_t xdl_hmix(uint64_t h, uint64_t w) {
	h = (h << 23) | (h >> 41);
	return (h ^ w) * XDL_HMUL;
}

/*
 * Equal byte sequences hash equally regardless of their buffer alignment.
 * A matching hash is always followed by a byte comparison.
 */
static inline uint64_t xdl_hash_line_fast(uint8_t const **data,
					 uint8_t const *top) {
	uint8_t const *start = *data, *ptr = start;
	uint64_t h = 0, w;
	unsigned int s;

#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	while (top - ptr >= 8) {
		uint64_t x, m;

		memcpy(&w, ptr, 8);
		x = w ^ 0x0a0a0a0a0a0a0a0aULL;
		m = (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
		if (m) {
			unsigned int n = (unsigned int)__builtin_ctzll(m) >> 3;

			if (n)
				h = xdl_hmix(h, w & (~0ULL >> (64 - 8 * n)));
			ptr += n;
			*data = ptr + 1;
			goto done;
		}
		h = xdl_hmix(h, w);
		ptr += 8;
	}
#endif
	w = 0;
	s = 0;
	for (; ptr < top && *ptr != '\n'; ptr++) {
		w |= (uint64_t)*ptr << s;
		s += 8;
		if (s == 64) {
			h = xdl_hmix(h, w);
			w = 0;
			s = 0;
		}
	}
	if (s)
		h = xdl_hmix(h, w);
	*data = ptr < top ? ptr + 1 : ptr;

#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
done:
#endif
	h ^= (uint64_t)(ptr - start);
	h *= XDL_HMUL;
	h ^= h >> 29;
	h *= 0xbf58476d1ce4e5b9ULL;
	h ^= h >> 32;
	return h;
}


static void xdl_free_ctx(xdfile_t *xdf)
{
	xdl_free(xdf->reference_index);
	xdl_free(xdf->changed - 1);
	xdl_free(xdf->recs);
}


static int xdl_prepare_ctx(unsigned int pass, mmfile_t *mf, long narec, xpparam_t const *xpp,
			   xdlclassifier_t *cf, xdfile_t *xdf) {
	long bsize;
	uint64_t hav;
	uint8_t const *blk, *cur, *top, *prev;
	xrecord_t *crec;

	xdf->reference_index = NULL;
	xdf->changed = NULL;
	xdf->recs = NULL;

	if (!XDL_ALLOC_ARRAY(xdf->recs, narec))
		goto abort;

	xdf->nrec = 0;
	if ((cur = blk = xdl_mmfile_first(mf, &bsize))) {
		for (top = blk + bsize; cur < top; ) {
			prev = cur;
			if (xpp->flags & XDF_WHITESPACE_FLAGS) {
				hav = xdl_hash_record_with_whitespace(&cur, top, xpp->flags);
				/*
				 * The whitespace hash has weak high bits for
				 * short lines. This bijective mix preserves
				 * equality while distributing the table slots.
				 */
				hav ^= hav >> 32;
				hav *= XDL_HMUL;
				hav ^= hav >> 29;
			} else {
				hav = xdl_hash_line_fast(&cur, top);
			}
			if ((long)xdf->nrec + 1 > narec) {
				long want = (long)xdf->nrec + 1;
				long used = (long)(cur - blk);

				/* Extrapolate from bytes already consumed. */
				if (used > 0 && want <= LONG_MAX / bsize) {
					long est = want * bsize / used;

					if (est <= LONG_MAX - est / 32 - 16)
						est += est / 32 + 16;
					if (est > want)
						want = est;
				}
				if (XDL_ALLOC_GROW(xdf->recs, want, narec))
					goto abort;
			}
			crec = &xdf->recs[xdf->nrec++];
			crec->ptr = prev;
			crec->size = cur - prev;
			if (xdl_classify_record(pass, cf, crec, hav) < 0)
				goto abort;
		}
	}

	if (!XDL_CALLOC_ARRAY(xdf->changed, xdf->nrec + 2))
		goto abort;

	xdf->changed += 1;
	xdf->nreff = 0;
	xdf->dstart = 0;
	xdf->dend = xdf->nrec - 1;

	return 0;

abort:
	xdl_free_ctx(xdf);
	return -1;
}


void xdl_free_env(xdfenv_t *xe) {

	xdl_free_ctx(&xe->xdf2);
	xdl_free_ctx(&xe->xdf1);
}


static bool xdl_clean_mmatch(uint8_t const *action, ptrdiff_t i, ptrdiff_t len) {
	ptrdiff_t r, rdis0, rpdis0, rdis1, rpdis1;
	ptrdiff_t s = 0, e = len - 1;

	/*
	 * Limits the window that is examined during the similar-lines
	 * scan. The loops below stops when action[i - r] == KEEP
	 * (line that has no match), but there are corner cases where
	 * the loop proceed all the way to the extremities by causing
	 * huge performance penalties in case of big files.
	 */
	if (i - s > XDL_SIMSCAN_WINDOW)
		s = i - XDL_SIMSCAN_WINDOW;
	if (e - i > XDL_SIMSCAN_WINDOW)
		e = i + XDL_SIMSCAN_WINDOW;

	/*
	 * Scans the lines before 'i' to find a run of lines that either
	 * have no match (action[j] == DISCARD) or have multiple matches
	 * (action[j] == INVESTIGATE). Note that we always call this
	 * function with action[i] == INVESTIGATE, so the current line
	 * (i) is already a multimatch line.
	 */
	for (r = 1, rdis0 = 0, rpdis0 = 1; (i - r) >= s; r++) {
		if (action[i - r] == DISCARD)
			rdis0++;
		else if (action[i - r] == INVESTIGATE)
			rpdis0++;
		else if (action[i - r] == KEEP)
			break;
		else
			BUG("Illegal value for action[i - r]");
	}
	/*
	 * If the run before the line 'i' found only multimatch lines,
	 * we return false and hence we don't make the current line (i)
	 * discarded. We want to discard multimatch lines only when
	 * they appear in the middle of runs with nomatch lines
	 * (action[j] == DISCARD).
	 */
	if (rdis0 == 0)
		return 0;
	for (r = 1, rdis1 = 0, rpdis1 = 1; (i + r) <= e; r++) {
		if (action[i + r] == DISCARD)
			rdis1++;
		else if (action[i + r] == INVESTIGATE)
			rpdis1++;
		else if (action[i + r] == KEEP)
			break;
		else
			BUG("Illegal value for action[i + r]");
	}
	/*
	 * If the run after the line 'i' found only multimatch lines,
	 * we return false and hence we don't make the current line (i)
	 * discarded.
	 */
	if (rdis1 == 0)
		return false;
	rdis1 += rdis0;
	rpdis1 += rpdis0;

	return rpdis1 * XDL_KPDIS_RUN < (rpdis1 + rdis1);
}


/*
 * Try to reduce the problem complexity, discard records that have no
 * matches on the other file. Also, lines that have multiple matches
 * might be potentially discarded if they appear in a run of discardable.
 */
static int xdl_cleanup_records(xdlclassifier_t *cf, xdfile_t *xdf1, xdfile_t *xdf2) {
	ptrdiff_t i, mlim1, mlim2;
	uint8_t *action1 = NULL, *action2 = NULL;
	bool need_min = !!(cf->flags & XDF_NEED_MINIMAL);
	int ret = 0;
	ptrdiff_t off = xdf1->dstart;
	ptrdiff_t len1 = xdf1->dend - off + 1;
	ptrdiff_t len2 = xdf2->dend - off + 1;
	bool investigate1 = false, investigate2 = false;

	/*
	 * Create temporary arrays that will help us decide if
	 * changed[i] should remain false, or become true. Every slot is
	 * initialized below, so one allocation serves both sides.
	 */
	if (len1 > PTRDIFF_MAX - len2 - 1 ||
	    !XDL_ALLOC_ARRAY(action1, len1 + len2 + 1) ||
	    !XDL_ALLOC_ARRAY(xdf1->reference_index, len1) ||
	    !XDL_ALLOC_ARRAY(xdf2->reference_index, len2))
	{
		ret = -1;
		goto cleanup;
	}
	action2 = action1 + len1;

	/*
	 * Initialize temporary arrays with DISCARD, KEEP, or INVESTIGATE.
	 */
	if (need_min) {
		/* i.e. infinity */
		mlim1 = PTRDIFF_MAX;
	} else {
		mlim1 = xdl_bogosqrt((uint64_t)xdf1->nrec);
		if (mlim1 > XDL_MAX_EQLIMIT)
			mlim1 = XDL_MAX_EQLIMIT;
	}
	{
		xrecord_t const *recs = xdf1->recs + off;

		for (i = 0; i < len1; i++) {
			ptrdiff_t nm = cf->rcrecs[recs[i].minimal_perfect_hash].len2;
			uint8_t action;

			if (!nm)
				action = DISCARD;
			else if (nm < mlim1)
				action = KEEP;
			else {
				action = INVESTIGATE;
				investigate1 = true;
			}
			action1[i] = action;
		}
	}

	if (need_min) {
		/* i.e. infinity */
		mlim2 = PTRDIFF_MAX;
	} else {
		mlim2 = xdl_bogosqrt((uint64_t)xdf2->nrec);
		if (mlim2 > XDL_MAX_EQLIMIT)
			mlim2 = XDL_MAX_EQLIMIT;
	}
	{
		xrecord_t const *recs = xdf2->recs + off;

		for (i = 0; i < len2; i++) {
			ptrdiff_t nm = cf->rcrecs[recs[i].minimal_perfect_hash].len1;
			uint8_t action;

			if (!nm)
				action = DISCARD;
			else if (nm < mlim2)
				action = KEEP;
			else {
				action = INVESTIGATE;
				investigate2 = true;
			}
			action2[i] = action;
		}
	}

	/*
	 * Use temporary arrays to decide if changed[i] should remain
	 * false, or become true.
	 */
	{
		size_t nreff = 0;
		size_t *ref = xdf1->reference_index;
		bool *changed = xdf1->changed + off;

		for (i = 0; i < len1; i++) {
			uint8_t action = action1[i];

			if (investigate1 && action == INVESTIGATE)
				action = xdl_clean_mmatch(action1, i, len1) ? DISCARD : KEEP;
			if (action == KEEP)
				ref[nreff++] = i + off;
			else
				changed[i] = true;
		}
		xdf1->nreff = nreff;
	}

	{
		size_t nreff = 0;
		size_t *ref = xdf2->reference_index;
		bool *changed = xdf2->changed + off;

		for (i = 0; i < len2; i++) {
			uint8_t action = action2[i];

			if (investigate2 && action == INVESTIGATE)
				action = xdl_clean_mmatch(action2, i, len2) ? DISCARD : KEEP;
			if (action == KEEP)
				ref[nreff++] = i + off;
			else
				changed[i] = true;
		}
		xdf2->nreff = nreff;
	}

cleanup:
	xdl_free(action1);

	return ret;
}


/*
 * Early trim initial and terminal matching records.
 */
static int xdl_trim_ends(xdfile_t *xdf1, xdfile_t *xdf2) {
	long i, lim;
	xrecord_t *recs1, *recs2;

	recs1 = xdf1->recs;
	recs2 = xdf2->recs;
	for (i = 0, lim = (long)XDL_MIN(xdf1->nrec, xdf2->nrec); i < lim;
	     i++, recs1++, recs2++)
		if (recs1->minimal_perfect_hash != recs2->minimal_perfect_hash)
			break;

	xdf1->dstart = xdf2->dstart = i;

	recs1 = xdf1->recs + xdf1->nrec - 1;
	recs2 = xdf2->recs + xdf2->nrec - 1;
	for (lim -= i, i = 0; i < lim; i++, recs1--, recs2--)
		if (recs1->minimal_perfect_hash != recs2->minimal_perfect_hash)
			break;

	xdf1->dend = (long)xdf1->nrec - i - 1;
	xdf2->dend = (long)xdf2->nrec - i - 1;

	return 0;
}


static int xdl_optimize_ctxs(xdlclassifier_t *cf, xdfile_t *xdf1, xdfile_t *xdf2) {

	if (xdl_trim_ends(xdf1, xdf2) < 0 ||
	    xdl_cleanup_records(cf, xdf1, xdf2) < 0) {

		return -1;
	}

	return 0;
}

int xdl_prepare_env(mmfile_t *mf1, mmfile_t *mf2, xpparam_t const *xpp,
		    xdfenv_t *xe) {
	long enl1, enl2, sample;
	xdlclassifier_t cf;

	memset(&cf, 0, sizeof(cf));

	/*
	 * Estimates only control allocations, not record boundaries.
	 * Both the record array and classifier can grow when necessary.
	 */
	sample = (XDF_DIFF_ALG(xpp->flags) == XDF_HISTOGRAM_DIFF
		  ? XDL_GUESS_NLINES2 : XDL_GUESS_NLINES1);

	enl1 = xdl_guess_lines(mf1, sample) + 1;
	{
		long sz1 = xdl_mmfile_size(mf1), sz2 = xdl_mmfile_size(mf2);

		if (sz1 > 0 && sz2 > 0 &&
		    sz1 <= LONG_MAX / 4 && sz2 <= LONG_MAX / 4 &&
		    sz2 <= 4 * sz1 && sz1 <= 4 * sz2 &&
		    sz2 <= LONG_MAX / enl1)
			enl2 = enl1 * sz2 / sz1 + 1;
		else
			enl2 = xdl_guess_lines(mf2, sample) + 1;
	}

	if (xdl_init_classifier_sized(&cf, enl1 + enl2 + 1, enl1, xpp->flags) < 0)
		return -1;

	if (xdl_prepare_ctx(1, mf1, enl1, xpp, &cf, &xe->xdf1) < 0) {

		xdl_free_classifier(&cf);
		return -1;
	}
	if (xe->xdf1.nrec && xdl_mmfile_size(mf1) > 0) {
		long sz1 = xdl_mmfile_size(mf1), sz2 = xdl_mmfile_size(mf2);
		long nr1 = (long)xe->xdf1.nrec;

		if (sz2 <= LONG_MAX / nr1) {
			long est = nr1 * sz2 / sz1;

			if (est <= LONG_MAX - est / 32 - 16)
				est += est / 32 + 16;
			if (est > enl2)
				enl2 = est;
		}
	}
	if (xdl_prepare_ctx(2, mf2, enl2, xpp, &cf, &xe->xdf2) < 0) {

		xdl_free_ctx(&xe->xdf1);
		xdl_free_classifier(&cf);
		return -1;
	}

	if ((XDF_DIFF_ALG(xpp->flags) != XDF_PATIENCE_DIFF) &&
	    (XDF_DIFF_ALG(xpp->flags) != XDF_HISTOGRAM_DIFF) &&
	    xdl_optimize_ctxs(&cf, &xe->xdf1, &xe->xdf2) < 0) {

		xdl_free_ctx(&xe->xdf2);
		xdl_free_ctx(&xe->xdf1);
		xdl_free_classifier(&cf);
		return -1;
	}

	xdl_free_classifier(&cf);

	return 0;
}
