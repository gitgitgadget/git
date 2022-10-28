#!/bin/sh

test_description='reset --hard unmerged'

TEST_PASSES_SANITIZE_LEAK=true
. ./test-lib.sh

test_expect_success setup '

	mkdir before later &&
	>before/1 &&
	>before/2 &&
	>hello &&
	>later/3 &&
	git add before hello later &&
	git commit -m world &&

	H=$(git rev-parse :hello) &&
	git rm --cached hello &&
	echo "100644 $H 2	hello" | git update-index --index-info &&

	rm -f hello &&
	mkdir -p hello &&
	>hello/world &&
	test "$(git ls-files -o)" = hello/world

'

test_expect_success 'reset --hard should restore unmerged ones' '

	git reset --hard &&
	git ls-files --error-unmatch before/1 before/2 hello later/3 &&
	test -f hello

'

test_expect_success 'reset --hard did not corrupt index or cache-tree' '

	T=$(git write-tree) &&
	rm -f .git/index &&
	git add before hello later &&
	U=$(git write-tree) &&
	test "$T" = "$U"

'

test_expect_success 'reset --hard in safe mode on unborn branch with staged files results in a warning' '
	git config reset.safe on &&
	touch a &&
	git add a &&
	test_must_fail git reset --hard

'

test_expect_success 'reset --hard in safe mode after a commit without staged changes works fine' '
	git config reset.safe on &&
	touch b &&
	git add b &&
	git commit -m "initial" &&
	git reset --hard

'

test_expect_success 'reset --hard in safe mode after a commit with staged changes results in a warning' '
	git config reset.safe on &&
	touch c d &&
	git add c &&
	git commit -m "initial" &&
	git add d &&
	test_must_fail git reset --hard

'

test_done
