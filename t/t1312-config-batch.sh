#!/bin/sh

test_description='Test git config-batch'

. ./test-lib.sh

test_expect_success 'no commands' '
	echo | git config-batch >out &&
	test_must_be_empty out
'

test_expect_success 'unknown_command' '
	echo unknown_command >expect &&
	echo "bogus 1 line of tokens" >in &&
	git config-batch >out <in &&
	test_cmp expect out
'

test_expect_success 'failed to parse version' '
	echo "bogus BAD_VERSION line of tokens" >in &&
	test_must_fail git config-batch 2>err <in &&
	test_grep BAD_VERSION err
'

test_done
