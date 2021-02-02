#!/bin/sh

test_description='git status color option'

. ./test-lib.sh

test_expect_success setup '
	echo 1 >original &&
	git add .
'

# Normal git status does not pipe colors
test_expect_success 'git status' '
	git status >raw &&
	test_decode_color <raw >out &&
	grep "original$" out
'

# Test new color option with never (expect same as above)
test_expect_success 'git status --color=never' '
	git status --color=never >raw &&
	test_decode_color <raw >out &&
	grep "original$" out
'

# Test new color (default is always)
test_expect_success 'git status --color' '
	git status --color >raw &&
	test_decode_color <raw >out &&
	grep "original<RESET>$" out
'

# Test new color option with always
test_expect_success 'git status --color=always' '
	git status --color=always >raw &&
	test_decode_color <raw >out &&
	grep "original<RESET>$" out
'

# Test verbose (default)
test_expect_success 'git status -v' '
	git status -v >raw &&
	test_decode_color <raw >out &&
	grep "+1" out
'

# Test verbose --color=never
test_expect_success 'git status -v --color=never' '
	git status -v --color=never >raw &&
	test_decode_color <raw >out &&
	grep "+1" out
'

# Test verbose --color (default always)
test_expect_success 'git status -v --color' '
	git status -v --color >raw &&
	test_decode_color <raw >out &&
	grep "<CYAN>@@ -0,0 +1 @@<RESET>" out &&
	grep "<GREEN>+<RESET><GREEN>1<RESET>" out
'

# Test verbose --color=always
test_expect_success 'git status -v --color=always' '
	git status -v --color=always >raw &&
	test_decode_color <raw >out &&
	grep "<CYAN>@@ -0,0 +1 @@<RESET>" out &&
	grep "GREEN>+<RESET><GREEN>1<RESET>" out
'

test_done
