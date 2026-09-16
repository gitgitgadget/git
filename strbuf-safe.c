#include "git-compat-util.h"
#include "strbuf-safe.h"
#include "banned-die.h"

/*
 * A safe version of ALLOC_GROW from git-compat-util.h and
 * xrealloc() from wrapper.c.
 */
#define SAFE_ALLOC_GROW(x, nr, alloc) \
	do { \
		if ((nr) > alloc) { \
			if (alloc_nr(alloc) < (nr)) \
				alloc = (nr); \
			else \
				alloc = alloc_nr(alloc); \
			if (srealloc((void **)&(x), alloc)) \
				return MEMORY_ERROR; \
		} \
	} while (0)

enum safe_result sstrbuf_grow(struct strbuf *sb, size_t extra)
{
	int new_buf = !sb->alloc;
	size_t new_len = st_add3(sb->len, extra, 1);
	if (new_buf)
		sb->buf = NULL;

	SAFE_ALLOC_GROW(sb->buf, new_len, sb->alloc);

	if (new_buf)
		sb->buf[0] = '\0';

	return SUCCESS;
}
