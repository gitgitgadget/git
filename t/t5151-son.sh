#!/bin/sh

test_description='Test git son command.'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_success 'setup parent repository' '
	echo "parent content" >file.txt &&
	git add file.txt &&
	git commit -m "Initial parent commit"
'

test_expect_success 'son creates child repository' '
	git son my-child &&
	test -d my-child &&
	test -d my-child/.git
'

test_expect_success 'son sets parent remote in child' '
	(
		cd my-child &&
		git remote get-url parent
	)
'

test_expect_success 'son adds child to parent .gitignore' '
	grep "my-child/" .gitignore
'

test_expect_success 'son child has initial commit' '
	(
		cd my-child &&
		test $(git log --oneline | wc -l) -eq 1
	)
'

test_expect_success 'son fails if target already exists' '
	test_must_fail git son my-child
'

test_expect_success 'son with --branch requires --inherit' '
	test_must_fail git son --branch main branch-child
'

test_expect_success 'son with --branch leaves no directory on failure' '
	! test -e branch-child
'

test_expect_success 'son with --inherit fetches parent history' '
	git init --bare "$TRASH_DIRECTORY/parent.git" &&
	git push "$TRASH_DIRECTORY/parent.git" main &&
	git remote add origin "file://$TRASH_DIRECTORY/parent.git" &&
	git son --inherit inherited-child &&
	(
		cd inherited-child &&
		git log --oneline parent/main
	)
'

test_done
