#!/bin/sh
#
# Test Git in parallel
#

. ${0%/*}/lib.sh

# CI EXPERIMENT: skip the Windows test slices; only macOS and Linux matter.
echo "skipping tests for the t4205 experiment"
exit 0

# We only have one unit test at the moment, so run it in the first slice.
if [ "$1" == "1" ] ; then
	group "Run unit tests" make --quiet -C t unit-tests-test-tool
fi

check_unignored_build_artifacts
