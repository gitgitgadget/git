#!/bin/sh

test_description='git-psuh test
This test runs git-psuh and makes sure it does not crash or have other errors.'

. ./test-lib.sh

# This is the main test from the tutorial, ensuring the command
# runs and prints the expected output after setting the necessary config.
test_expect_success 'runs correctly with no args and good output' '
	git config user.name "Test User" &&
	git psuh >actual &&
	grep "Your name: Test User" actual
'

# A simpler test just to ensure the command exits successfully.
test_expect_success 'git psuh does not crash' '
	git psuh
'

test_done
