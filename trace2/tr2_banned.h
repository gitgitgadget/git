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

#endif /* TR2_BANNED_H */
