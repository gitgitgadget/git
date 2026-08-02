#ifndef TR2_BANNED_H
#define TR2_BANNED_H

/*
 * This header lists functions that must not be used by Trace2 because they
 * can cause Git to terminate. It must be included after all other headers.
 */

#undef xsnprintf
#define xsnprintf(...) BANNED(xsnprintf)

#undef xstrdup
#define xstrdup(str) BANNED(xstrdup)

#undef xcalloc
#define xcalloc(nmemb, size) BANNED(xcalloc)

#undef xstrfmt
#define xstrfmt(...) BANNED(xstrfmt)

#undef xgethostname
#define xgethostname(buf, len) BANNED(xgethostname)

#undef ALLOC_ARRAY
#define ALLOC_ARRAY(x, alloc) BANNED(ALLOC_ARRAY)

#undef ALLOC_GROW
#define ALLOC_GROW(x, nr, alloc) BANNED(ALLOC_GROW)

#endif /* TR2_BANNED_H */
