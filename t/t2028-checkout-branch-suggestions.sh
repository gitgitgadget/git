#!/bin/sh

test_description='checkout branch suggestions'

. ./test-lib.sh

test_expect_success 'setup' '
	test_commit initial &&
	git branch feature-authentication &&
	git branch feature-authorization &&
	git branch bugfix-auth-issue
'

test_expect_success 'suggest similar branch names on checkout failure' '
	test_must_fail git checkout feature-auth 2>err &&
	grep "hint: Did you mean one of these?" err &&
	grep "feature-authentication" err &&
	grep "feature-authorization" err
'

test_expect_success 'suggest single branch name on close match' '
	test_must_fail git checkout feature-authent 2>err &&
	grep "hint: Did you mean one of these?" err &&
	grep "feature-authentication" err
'

test_expect_success 'no suggestions for very different names' '
	test_must_fail git checkout completely-different-name 2>err &&
	! grep "hint: Did you mean" err
'

test_expect_success 'no suggestions for paths with slashes' '
	test_must_fail git checkout nonexistent/file.txt 2>err &&
	! grep "hint: Did you mean" err
'

test_done