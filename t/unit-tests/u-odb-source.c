#include "unit-test.h"
#include "odb/source.h"
#include "odb/source-files.h"

/*
 * Verify that odb_source_files_try() returns the files backend
 * for ODB_SOURCE_FILES and NULL for other source types.
 */
void test_odb_source__try_returns_files_for_files_type(void)
{
	struct odb_source_files files_src;
	memset(&files_src, 0, sizeof(files_src));
	files_src.base.type = ODB_SOURCE_FILES;

	cl_assert(odb_source_files_try(&files_src.base) == &files_src);
}

void test_odb_source__try_returns_null_for_unknown_type(void)
{
	struct odb_source other_src;
	memset(&other_src, 0, sizeof(other_src));
	other_src.type = ODB_SOURCE_UNKNOWN;

	cl_assert(odb_source_files_try(&other_src) == NULL);
}
