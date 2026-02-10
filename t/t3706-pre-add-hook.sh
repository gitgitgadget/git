#!/bin/sh

test_description='pre-add hook tests

These tests run git add with and without pre-add hooks to ensure functionality. Largely derived from t7503 (pre-commit and pre-merge-commit hooks) and t5571 (pre-push hooks).'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_success 'with no hook' '
	test_when_finished "rm -f actual" &&
	echo content >file &&
	git add file &&
	test_path_is_missing actual
'

test_expect_success POSIXPERM 'with non-executable hook' '
	test_when_finished "rm -f actual" &&
	test_hook pre-add <<-\EOF &&
	echo should-not-run >>actual
	exit 1
	EOF
	chmod -x .git/hooks/pre-add &&

	echo content >file &&
	git add file &&
	test_path_is_missing actual
'

test_expect_success '--no-verify with no hook' '
	echo content >file &&
	git add --no-verify file &&
	test_path_is_missing actual
'

test_expect_success 'with succeeding hook' '
	test_when_finished "rm -f actual expected" &&
	echo "pre-add" >expected &&
	test_hook pre-add <<-\EOF &&
	echo pre-add >>actual
	EOF

	echo content >file &&
	git add file &&
	test_cmp expected actual
'

test_expect_success 'with failing hook' '
	test_when_finished "rm -f actual" &&
	test_hook pre-add <<-\EOF &&
	echo pre-add-rejected >>actual
	exit 1
	EOF

	echo content >file &&
	test_must_fail git add file
'

test_expect_success '--no-verify with failing hook' '
	test_when_finished "rm -f actual" &&
	test_hook pre-add <<-\EOF &&
	echo should-not-run >>actual
	exit 1
	EOF

	echo content >file &&
	git add --no-verify file &&
	test_path_is_missing actual
'

test_expect_success 'hook receives GIT_INDEX_FILE environment variable' '
	test_when_finished "rm -f actual expected" &&
	echo "hook-saw-env" >expected &&
	test_hook pre-add <<-\EOF &&
	if test -z "$GIT_INDEX_FILE"
	then
		echo hook-missing-env >>actual
	else
		echo hook-saw-env >>actual
	fi
	EOF

	echo content >file &&
	git add file &&
	test_cmp expected actual
'

test_expect_success 'with --dry-run (show-only) the hook is not invoked' '
	test_when_finished "rm -f actual" &&
	test_hook pre-add <<-\EOF &&
	echo should-not-run >>actual
	exit 1
	EOF

	echo content >file &&
	git add --dry-run file &&
	test_path_is_missing actual
'

test_expect_success 'hook is invoked with git add -u' '
	test_when_finished "rm -f actual expected file" &&
	echo "initial" >file &&
	git add file &&
	git commit -m "initial" &&
	echo "pre-add" >expected &&
	test_hook pre-add <<-\EOF &&
	echo pre-add >>actual
	EOF

	echo modified >file &&
	git add -u &&
	test_cmp expected actual
'

test_done
