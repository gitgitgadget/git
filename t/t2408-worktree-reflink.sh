#!/bin/sh

test_description='copy-on-write (reflink) population of new worktrees'

. ./test-lib.sh

test_expect_success REFLINK 'test-tool reflink clones file contents' '
	echo content >src &&
	test-tool reflink src dst &&
	test_cmp src dst
'

test_expect_success 'test-tool reflink fails on missing source' '
	test_must_fail test-tool reflink does-not-exist dst2 &&
	test_path_is_missing dst2
'

test_done
