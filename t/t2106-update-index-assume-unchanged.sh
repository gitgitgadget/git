#!/bin/sh

test_description='git update-index --assume-unchanged test.
'

. ./test-lib.sh

test_expect_success 'setup' '
	: >file &&
	git add file &&
	git commit -m initial &&
	git branch other &&
	echo upstream >file &&
	git add file &&
	git commit -m upstream
'

test_expect_success 'do not switch branches with dirty file' '
	git reset --hard &&
	git checkout other &&
	echo dirt >file &&
	git update-index --assume-unchanged file &&
	test_must_fail git checkout - 2>err &&
	test_grep overwritten err
'

test_expect_success '--really-refresh overrides assume-unchanged under preload' '
	git reset --hard &&
	test_commit really-refresh really-refresh original &&
	git update-index --assume-unchanged really-refresh &&
	printf "modified\n" >really-refresh &&
	test-tool chmtime -100000 really-refresh &&
	test_must_fail env GIT_TEST_PRELOAD_INDEX=1 \
		git update-index --really-refresh >out 2>err &&
	test_grep "needs update" out
'

test_done
