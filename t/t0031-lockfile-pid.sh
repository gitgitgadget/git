#!/bin/sh

test_description='lock file PID info tests

Tests for PID info file alongside lock files.
The feature is opt-in via core.lockfilePid config setting.
'

. ./test-lib.sh

test_expect_success 'stale lock detected when PID is not running' '
	git init repo &&
	(
		cd repo &&
		touch .git/index.lock &&
		printf "pid 99999" >.git/index.pid.lock &&
		test_must_fail git -c core.lockfilePid=index add . 2>err &&
		test_grep "process 99999, which is no longer running" err &&
		test_grep "Remove the stale lock file" err
	)
'

test_expect_success 'PID info not shown by default' '
	git init repo2 &&
	(
		cd repo2 &&
		touch .git/index.lock &&
		printf "pid 99999" >.git/index.pid.lock &&
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
		# Get the correct PID for this platform
		shell_pid=$$ &&
		if test_have_prereq MINGW && test -f /proc/$shell_pid/winpid
		then
			# In Git for Windows, Bash uses MSYS2 PIDs but git.exe
			# uses Windows PIDs. Use the Windows PID.
			shell_pid=$(cat /proc/$shell_pid/winpid)
		fi &&
		# Create a lock and PID file with current shell PID (which is running)
		touch .git/index.lock &&
		printf "pid %d" "$shell_pid" >.git/index.pid.lock &&
		# Verify our PID is shown in the error message
		test_must_fail git -c core.lockfilePid=index add file 2>err &&
		test_grep "held by process $shell_pid" err
	)
'

test_expect_success 'PID info file cleaned up on successful operation when enabled' '
	git init repo4 &&
	(
		cd repo4 &&
		echo content >file &&
		git -c core.lockfilePid=index add file &&
		# After successful add, no lock or PID files should exist
		test_path_is_missing .git/index.lock &&
		test_path_is_missing .git/index.pid.lock
	)
'

test_expect_success 'no PID file created by default' '
	git init repo5 &&
	(
		cd repo5 &&
		echo content >file &&
		git add file &&
		# PID file should not be created when feature is disabled
		test_path_is_missing .git/index.pid.lock
	)
'

test_expect_success 'core.lockfilePid=all enables for all components' '
	git init repo6 &&
	(
		cd repo6 &&
		touch .git/index.lock &&
		printf "pid 99999" >.git/index.pid.lock &&
		test_must_fail git -c core.lockfilePid=all add . 2>err &&
		test_grep "process 99999" err
	)
'

test_expect_success 'multiple components can be specified' '
	git init repo8 &&
	(
		cd repo8 &&
		touch .git/index.lock &&
		printf "pid 99999" >.git/index.pid.lock &&
		test_must_fail git -c core.lockfilePid=index,config add . 2>err &&
		test_grep "process 99999" err
	)
'

test_expect_success 'core.lockfilePid=none does not create PID file' '
	git init repo9 &&
	(
		cd repo9 &&
		echo content >file &&
		git -c core.lockfilePid=none add file &&
		# PID file should not be created when feature is disabled
		test_path_is_missing .git/index.pid.lock
	)
'

test_expect_success 'existing PID files are read even when feature disabled' '
	git init repo10 &&
	(
		cd repo10 &&
		touch .git/index.lock &&
		printf "pid 99999" >.git/index.pid.lock &&
		# Even with lockfilePid disabled, existing PID files are read
		# to help diagnose stale locks
		test_must_fail git add . 2>err &&
		test_grep "process 99999" err
	)
'

test_expect_success 'negative component syntax excludes specific components' '
	git init repo11 &&
	(
		cd repo11 &&
		echo content >file &&
		# Enable all components except index
		git -c core.lockfilePid=all,-index add file &&
		# PID file should not be created for index when excluded
		test_path_is_missing .git/index.pid.lock
	)
'

test_done
