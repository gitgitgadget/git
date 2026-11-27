#!/bin/sh

test_description='.gitlocalignore support (core.gitLocalIgnore)'

TEST_PASSES_SANITIZE_LEAK=true
. ./test-lib.sh

# off by default: .gitlocalignore is not consulted at all
test_expect_success 'off by default: .gitlocalignore is ignored' '
	echo content >a.txt &&
	printf "a.txt\n" >.gitlocalignore &&
	test_must_fail git check-ignore a.txt
'

test_expect_success 'core.gitLocalIgnore enables .gitlocalignore' '
	test_config core.gitLocalIgnore true &&
	echo content >b.txt &&
	printf "b.txt\n" >.gitlocalignore &&
	git check-ignore b.txt
'

test_expect_success '.gitlocalignore is self-excluding' '
	test_config core.gitLocalIgnore true &&
	printf "b.txt\n" >.gitlocalignore &&
	git check-ignore .gitlocalignore
'

test_expect_success 'info/exclude outranks .gitlocalignore' '
	test_config core.gitLocalIgnore true &&
	mkdir -p .git/info &&
	echo "c.txt" >.git/info/exclude &&
	echo content >c.txt &&
	printf "c.txt\n" >.gitlocalignore &&
	git check-ignore -v c.txt >out &&
	grep "info/exclude" out
'

test_expect_success '.gitlocalignore outranks core.excludesFile' '
	test_config core.gitLocalIgnore true &&
	echo content >d.txt &&
	printf "d.txt\n" >.gitlocalignore &&
	echo "d.txt" >.git/excludes-extra &&
	test_config core.excludesFile "$(pwd)/.git/excludes-extra" &&
	git check-ignore -v d.txt >out &&
	grep "\.gitlocalignore" out
'

test_expect_success 'nested .gitlocalignore contents are not read' '
	test_config core.gitLocalIgnore true &&
	rm -f .gitlocalignore &&
	mkdir -p sub &&
	echo content >sub/e.txt &&
	printf "e.txt\n" >sub/.gitlocalignore &&
	test_must_fail git check-ignore sub/e.txt
'

test_expect_success 'ignored files stay out of git status' '
	test_config core.gitLocalIgnore true &&
	echo content >f.txt &&
	printf "f.txt\n" >.gitlocalignore &&
	git status --porcelain >out &&
	! grep f.txt out
'

test_expect_success 'git add refuses .gitlocalignore unless forced' '
	test_config core.gitLocalIgnore true &&
	printf "f.txt\n" >.gitlocalignore &&
	test_must_fail git add .gitlocalignore &&
	git add -f .gitlocalignore &&
	git ls-files --error-unmatch .gitlocalignore
'

test_done
