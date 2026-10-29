#include "test-tool.h"
#include "wrapper.h"

int cmd__reflink(int argc, const char **argv)
{
	if (argc != 3)
		usage("test-tool reflink <src> <dst>");
	if (reflink_file(argv[1], argv[2], 0666)) {
		fprintf(stderr, "reflink failed: %s\n", strerror(errno));
		return 1;
	}
	return 0;
}
