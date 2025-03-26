#!/bin/sh

test_description='git survey'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

TEST_PASSES_SANITIZE_LEAK=0
export TEST_PASSES_SANITIZE_LEAK

. ./test-lib.sh

test_expect_success 'create a semi-interesting repo' '
	test_commit_bulk 10
'

test_expect_success 'git survey (default)' '
	git survey >out 2>err &&
	test_line_count = 0 err
'

test_done
