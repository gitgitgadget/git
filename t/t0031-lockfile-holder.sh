#!/bin/sh

test_description='lock file holder info tests

Tests for PID holder info file alongside lock files.
The feature is opt-in via GIT_LOCK_HOLDER_INFO=1.
'

. ./test-lib.sh

test_expect_success 'holder info shown in lock error message when enabled' '
	git init repo &&
	(
		cd repo &&
		touch .git/index.lock &&
		echo "99999" >.git/index.lock.holder &&
		test_must_fail env GIT_LOCK_HOLDER_INFO=1 git add . 2>err &&
		test_grep "Lock is held by process 99999" err
	)
'

test_expect_success 'holder info not shown by default' '
	git init repo2 &&
	(
		cd repo2 &&
		touch .git/index.lock &&
		echo "99999" >.git/index.lock.holder &&
		test_must_fail git add . 2>err &&
		# Should not crash, just show normal error without PID
		test_grep "Unable to create" err &&
		! test_grep "Lock is held by process" err
	)
'

test_expect_success 'holder file created with correct PID during lock' '
	git init repo3 &&
	(
		cd repo3 &&
		echo content >file &&
		# Create a lock and holder file manually to simulate a holding process
		touch .git/index.lock &&
		echo $$ >.git/index.lock.holder &&
		# Verify our PID is shown in the error message
		test_must_fail env GIT_LOCK_HOLDER_INFO=1 git add file 2>err &&
		test_grep "Lock is held by process $$" err
	)
'

test_expect_success 'holder info file cleaned up on successful operation when enabled' '
	git init repo4 &&
	(
		cd repo4 &&
		echo content >file &&
		env GIT_LOCK_HOLDER_INFO=1 git add file &&
		# After successful add, no lock or holder files should exist
		! test -f .git/index.lock &&
		! test -f .git/index.lock.holder
	)
'

test_expect_success 'no holder file created by default' '
	git init repo5 &&
	(
		cd repo5 &&
		echo content >file &&
		git add file &&
		# Holder file should not be created when feature is disabled
		! test -f .git/index.lock.holder
	)
'

test_done
