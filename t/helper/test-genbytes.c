#define DISABLE_SIGN_COMPARE_WARNINGS

#include "test-tool.h"
#include "git-compat-util.h"
#include "hex-ll.h"

/*
 * Write "count" copies of "byte" to stdout, or an endless stream of them if
 * "count" is negative.
 */
static int generate_bytes(char byte, intmax_t count)
{
	char buf[256 * 1024];
	ssize_t n;

	memset(buf, byte, sizeof(buf));

	/* Writing out individual bytes is slow... */
	while (count < 0)
		if (xwrite(1, buf, ARRAY_SIZE(buf)) < 0)
			die_errno("write error");

	while (count > 0) {
		n = xwrite(1, buf,
			   count < ARRAY_SIZE(buf)
			   ? count : ARRAY_SIZE(buf));

		if (n < 0)
			die_errno("write error");

		count -= n;
	}

	return 0;
}

static intmax_t parse_count(int argc, const char **argv, int pos)
{
	return pos < argc ? strtoimax(argv[pos], NULL, 0) : -1;
}

int cmd__genbytes(int argc, const char **argv)
{
	if (argc < 2 || argc > 3 || strlen(argv[1]) != 2 ||
	    strspn(argv[1], "0123456789abcdef") != 2) {
		fprintf(stderr, "usage: %s <hex-byte> [<count>]\n", argv[0]);
		return 1;
	}

	return generate_bytes(hex2chr(argv[1]), parse_count(argc, argv, 2));
}
