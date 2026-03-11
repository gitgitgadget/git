#!/bin/sh

test_description='checkout/switch --autostash tests'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_success 'setup' '
	echo file0content >file0 &&
	echo file1content >file1 &&
	git add . &&
	test_tick &&
	git commit -m "initial commit" &&
	git branch other-branch &&
	echo file1main >file1 &&
	git add . &&
	test_tick &&
	git commit -m "modify file1 on main" &&
	git checkout other-branch &&
	echo file1other >file1 &&
	git add . &&
	test_tick &&
	git commit -m "modify file1 on other-branch" &&
	echo file2content >file2 &&
	git add . &&
	test_tick &&
	git commit -m "add file2 on other-branch" &&
	git checkout main
'

test_expect_success 'switch --autostash skips stash when no conflict' '
	git branch branch1 other-branch &&
	echo dirty >file0 &&
	git switch --autostash branch1 >actual 2>&1 &&
	test_grep ! "Created autostash" actual &&
	echo dirty >expected &&
	test_cmp expected file0 &&
	git switch main
'

test_expect_success 'checkout --autostash skips stash when no conflict' '
	git branch branch2 other-branch &&
	echo dirty >file0 &&
	git checkout --autostash branch2 >actual 2>&1 &&
	test_grep ! "Created autostash" actual &&
	echo dirty >expected &&
	test_cmp expected file0 &&
	git checkout main
'

test_expect_success 'switch: checkout.autostash config skips stash when no conflict' '
	git branch branch3 other-branch &&
	echo dirty >file0 &&
	test_config checkout.autostash true &&
	git switch branch3 >actual 2>&1 &&
	test_grep ! "Created autostash" actual &&
	echo dirty >expected &&
	test_cmp expected file0 &&
	git switch main
'

test_expect_success 'checkout: checkout.autostash config skips stash when no conflict' '
	git branch branch4 other-branch &&
	echo dirty >file0 &&
	test_config checkout.autostash true &&
	git checkout branch4 >actual 2>&1 &&
	test_grep ! "Created autostash" actual &&
	echo dirty >expected &&
	test_cmp expected file0 &&
	git checkout main
'

test_expect_success '--no-autostash overrides checkout.autostash' '
	git branch branch5 other-branch &&
	echo dirty >file1 &&
	test_config checkout.autostash true &&
	test_must_fail git switch --no-autostash branch5 2>stderr &&
	test_grep ! "Created autostash" stderr &&
	git checkout -- file1
'

test_expect_success '--autostash overrides checkout.autostash=false' '
	git branch branch6 other-branch &&
	echo dirty >file0 &&
	test_config checkout.autostash false &&
	git switch --autostash branch6 >actual 2>&1 &&
	test_grep ! "Created autostash" actual &&
	echo dirty >expected &&
	test_cmp expected file0 &&
	git switch main
'

test_expect_success 'autostash with non-conflicting dirty index' '
	git branch branch7 other-branch &&
	echo dirty-index >file0 &&
	git add file0 &&
	git switch --autostash branch7 >actual 2>&1 &&
	test_grep ! "Created autostash" actual &&
	echo dirty-index >expected &&
	test_cmp expected file0 &&
	git checkout -- file0 &&
	git switch main
'

test_expect_success 'autostash bypasses conflicting local changes' '
	git branch branch8 other-branch &&
	echo dirty >file1 &&
	test_must_fail git switch branch8 2>stderr &&
	test_grep "Your local changes" stderr &&
	git switch --autostash branch8 >actual 2>&1 &&
	test_grep "Created autostash" actual &&
	test_grep "Applying autostash resulted in conflicts" actual &&
	test_grep "Your changes are safe in the stash" actual &&
	git stash drop &&
	git reset --hard &&
	git switch main
'

test_expect_success 'autostash is a no-op with clean worktree' '
	git branch branch9 other-branch &&
	git switch --autostash branch9 >actual 2>&1 &&
	test_grep ! "Created autostash" actual &&
	git switch main
'

test_expect_success '--autostash with --merge skips stash when no conflict' '
	git branch branch10 other-branch &&
	echo dirty >file0 &&
	git switch --autostash --merge branch10 >actual 2>&1 &&
	test_grep ! "Created autostash" actual &&
	echo dirty >expected &&
	test_cmp expected file0 &&
	git switch main
'

test_expect_success 'autostash with staged conflicting changes' '
	git branch branch11 other-branch &&
	echo staged-change >file1 &&
	git add file1 &&
	git switch --autostash branch11 >actual 2>&1 &&
	test_grep "Created autostash" actual &&
	test_grep "Applying autostash resulted in conflicts" actual &&
	test_grep "Your changes are safe in the stash" actual &&
	git stash drop &&
	git reset --hard &&
	git switch main
'

test_expect_success '--autostash with --force preserves dirty changes' '
	git branch branch12 other-branch &&
	echo dirty-force >file1 &&
	git switch --autostash --force branch12 >actual 2>&1 &&
	test_grep "Created autostash" actual &&
	test_grep "Applying autostash resulted in conflicts" actual &&
	test_grep "Your changes are safe in the stash" actual &&
	git stash drop &&
	git reset --hard &&
	git switch main
'

test_expect_success '--autostash with new branch creation skips stash' '
	echo dirty >file0 &&
	git switch --autostash -c branch13 >actual 2>&1 &&
	test_grep ! "Created autostash" actual &&
	echo dirty >expected &&
	test_cmp expected file0 &&
	git switch main &&
	git branch -D branch13
'

test_expect_success 'autostash with conflicting changes that apply cleanly' '
	git branch branch14 other-branch &&
	echo file1other >file1 &&
	git switch --autostash branch14 >actual 2>&1 &&
	test_grep "Created autostash" actual &&
	test_grep "Applied autostash" actual &&
	echo file1other >expected &&
	test_cmp expected file1 &&
	git switch main
'

test_done
