#!/bin/sh
#

test_description='regression test for memory leak in git merge'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./lib-bash.sh

# test-lib.sh disables LeakSanitizer by default, but we want it enabled
# for this test
ASAN_OPTIONS=
export ASAN_OPTIONS

. "$GIT_BUILD_DIR/contrib/completion/git-prompt.sh"

test_expect_success 'Merge fails due to local changes' '
	git init &&
	echo x > x.txt &&
	git add . &&
	git commit -m "WIP" &&
	git checkout -b dev &&
	echo y > x.txt &&
	git add . &&
	git commit -m "WIP" &&
	git checkout main &&
	echo z > x.txt &&
	git add . &&
	git commit -m "WIP" &&
	echo a > x.txt &&
	git add . &&
	echo "error: ''Your local changes to the following files would be overwritten by merge:''" >expected &&
	echo "  x.txt" >>expected &&
	echo "Merge with strategy ort failed." >>expected &&
	test_must_fail git merge -s ort dev 2>actual &&
	test_cmp expected actual
'

test_done
