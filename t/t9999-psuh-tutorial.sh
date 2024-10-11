#!/bin/sh

test_description='git-psuh test

This test runs git-psuh and makes sure it does not crash'

. ./test-lib.sh

test_expect_success "runs corrrectly with no args and good output" '
        git psuh >actual &&
        grep Pony actual
'
test_done
