#!/bin/sh

test_description='lock file PID info tests

Tests for PID info file alongside lock files.
The feature is opt-in via GIT_LOCK_PID_INFO=1.
'

. ./test-lib.sh

test_expect_success 'stale lock detected when PID is not running' '
	git init repo &&
	(
		cd repo &&
		touch .git/index.lock &&
		echo "99999" >.git/index.lock.pid &&
		test_must_fail env GIT_LOCK_PID_INFO=1 git add . 2>err &&
		test_grep "process 99999, which is no longer running" err &&
		test_grep "Remove the stale lock file" err
	)
'

test_expect_success 'PID info not shown by default' '
	git init repo2 &&
	(
		cd repo2 &&
		touch .git/index.lock &&
		echo "99999" >.git/index.lock.pid &&
		test_must_fail git add . 2>err &&
		# Should not crash, just show normal error without PID
		test_grep "Unable to create" err &&
		! test_grep "is held by process" err
	)
'

test_expect_success 'running process detected when PID is alive' '
	git init repo3 &&
	(
		cd repo3 &&
		echo content >file &&
		# Create a lock and PID file with current shell PID (which is running)
		touch .git/index.lock &&
		echo $$ >.git/index.lock.pid &&
		# Verify our PID is shown in the error message
		test_must_fail env GIT_LOCK_PID_INFO=1 git add file 2>err &&
		test_grep "held by process $$" err
	)
'

test_expect_success 'PID info file cleaned up on successful operation when enabled' '
	git init repo4 &&
	(
		cd repo4 &&
		echo content >file &&
		env GIT_LOCK_PID_INFO=1 git add file &&
		# After successful add, no lock or PID files should exist
		! test -f .git/index.lock &&
		! test -f .git/index.lock.pid
	)
'

test_expect_success 'no PID file created by default' '
	git init repo5 &&
	(
		cd repo5 &&
		echo content >file &&
		git add file &&
		# PID file should not be created when feature is disabled
		! test -f .git/index.lock.pid
	)
'

test_done
