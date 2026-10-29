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

test_expect_success 'core.ignorestat marks updated paths assume-unchanged' '
	git reset --hard &&
	git update-index --no-assume-unchanged file &&
	test_config core.ignorestat true &&
	echo changed >file &&
	git update-index file &&
	test "$(git ls-files -v file)" = "h file"
'

test_done
