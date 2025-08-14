#!/bin/sh

test_description='git-psuh test
This test runs git-psuh and makes sure it does not crash.'

. ./test-lib.sh

test_expect_success 'runs correctly with no args and good output' '
    git config user.name "Test User" &&
    git psuh >actual &&
    grep "Your name:" actual
'

test_done
