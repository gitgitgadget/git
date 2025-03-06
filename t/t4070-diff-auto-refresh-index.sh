#!/bin/sh
#
# Copyright (c) 2025 Benjamin Woodruff
#

test_description='diff.autoRefreshIndex config option'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-diff.sh

test_expect_success 'index is updated when autoRefreshIndex is true' '
	>tracked &&
	git add tracked &&

	# stat() must change (but not file contents) to trigger an index update
	test_set_magic_mtime tracked &&

	# check the mtime of .git/index does not change without autoRefreshIndex
	test_set_magic_mtime .git/index &&
	git config diff.autoRefreshIndex false &&
	git diff &&
	test_is_magic_mtime .git/index &&

	# but it does change when autoRefreshIndex is true (the default)
	git config diff.autoRefreshIndex true &&
	git diff &&
	! test_is_magic_mtime .git/index
'

test_expect_success '--no-optional-locks overrides autoRefreshIndex' '
	>tracked &&
	git add tracked &&
	test_set_magic_mtime tracked &&

	# `--no-optional-locks` overrides `autoRefreshIndex`
	test_set_magic_mtime .git/index &&
	git config diff.autoRefreshIndex true &&
	git --no-optional-locks diff &&

	# sanity check that without `--no-optional-locks` it still updates
	test_is_magic_mtime .git/index &&
	git diff &&
	! test_is_magic_mtime .git/index
'

test_done
