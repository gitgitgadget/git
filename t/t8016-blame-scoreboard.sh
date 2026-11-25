#!/bin/sh

test_description='blame scoreboard teardown'

. ./test-lib.sh

test_expect_success 'set up and tear down two scoreboards in one process' '
	test_commit one file &&
	test-tool repository blame-scoreboard file
'

test_done
