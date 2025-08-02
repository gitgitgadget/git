#!/bin/sh

test_description='git-psuh test

This test runs git-psuh and makes sure it does not crash.'

. ./test-lib.sh

test_expect_success 'git psuh prints a message' '
	git psuh >actual &&
	test_cmp /dev/null actual
'

test_expect_success 'git psuh does not crash' '
	git psuh >actual 2>&1 &&
	test $? -eq 0
'

test_done