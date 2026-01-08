#!/bin/sh

test_description='Test git config-batch'

. ./test-lib.sh

test_expect_success 'help text' '
	test_must_fail git config-batch -h >out &&
	grep usage out
'

test_done
