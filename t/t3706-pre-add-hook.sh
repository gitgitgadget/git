#!/bin/sh

test_description='pre-add hook tests

These tests run git add with and without pre-add hooks to ensure functionality. Largely derived from t7503 (pre-commit and pre-merge-commit hooks) and t5571 (pre-push hooks).'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_success 'with no hook' '
	test_when_finished "rm -f actual" &&
	echo content >file &&
	git add file &&
	test_path_is_missing actual
'

test_expect_success POSIXPERM 'with non-executable hook' '
	test_when_finished "rm -f actual" &&
	test_hook pre-add <<-\EOF &&
	echo should-not-run >>actual
	exit 1
	EOF
	chmod -x .git/hooks/pre-add &&

	echo content >file &&
	git add file &&
	test_path_is_missing actual
'

test_expect_success '--no-verify with no hook' '
	echo content >file &&
	git add --no-verify file &&
	test_path_is_missing actual
'

test_expect_success 'with succeeding hook' '
	test_when_finished "rm -f actual expected" &&
	echo "pre-add" >expected &&
	test_hook pre-add <<-\EOF &&
	echo pre-add >>actual
	EOF

	echo content >file &&
	git add file &&
	test_cmp expected actual
'

test_expect_success 'with failing hook' '
	test_when_finished "rm -f actual" &&
	test_hook pre-add <<-\EOF &&
	echo pre-add-rejected >>actual
	exit 1
	EOF

	echo content >file &&
	test_must_fail git add file
'

test_expect_success '--no-verify with failing hook' '
	test_when_finished "rm -f actual" &&
	test_hook pre-add <<-\EOF &&
	echo should-not-run >>actual
	exit 1
	EOF

	echo content >file &&
	git add --no-verify file &&
	test_path_is_missing actual
'

test_expect_success 'hook receives original and proposed index as arguments' '
	test_when_finished "rm -f tracked expected hook-ran" &&
	echo "initial" >tracked &&
	git add tracked &&
	git commit -m "initial" &&
	test_hook pre-add <<-\EOF &&
	test $# -eq 2 &&
	test -f "$1" &&
	test -f "$2" &&
	echo pass >hook-ran
	EOF

	echo "modified" >tracked &&
	git add tracked &&
	echo pass >expected &&
	test_cmp expected hook-ran
'

test_expect_success 'hook handles first add with no existing index' '
	test_when_finished "rm -rf no-index" &&
	test_create_repo no-index &&
	echo ok >no-index/expected &&
	test_hook -C no-index pre-add <<-\EOF &&
	test $# -eq 2 &&
	test ! -e "$1" &&
	test -f "$2" &&
	echo ok >hook-ran
	EOF

	echo first >no-index/file &&
	git -C no-index add file &&
	test_cmp no-index/expected no-index/hook-ran
'

test_expect_success 'hook is not invoked with --dry-run (show-only)' '
	test_when_finished "rm -f actual" &&
	test_hook pre-add <<-\EOF &&
	echo should-not-run >>actual
	exit 1
	EOF

	echo content >file &&
	git add --dry-run file &&
	test_path_is_missing actual
'

test_expect_success 'hook is invoked with git add -u' '
	test_when_finished "rm -f actual expected file" &&
	echo "initial" >file &&
	git add file &&
	git commit -m "initial" &&
	echo "pre-add" >expected &&
	test_hook pre-add <<-\EOF &&
	echo pre-add >>actual
	EOF

	echo modified >file &&
	git add -u &&
	test_cmp expected actual
'

test_expect_success 'hook can compare original and proposed index' '
	test_when_finished "rm -f old-raw new-raw old-list new-list \
			    expected-old expected-new" &&
	echo "initial" >file1 &&
	echo "initial" >file2 &&
	git add file1 file2 &&
	git commit -m "initial" &&
	echo "staged-before" >file1 &&
	git add file1 &&
	test_hook pre-add <<-\EOF &&
	GIT_INDEX_FILE="$1" git diff --cached --name-only HEAD >old-raw &&
	GIT_INDEX_FILE="$2" git diff --cached --name-only HEAD >new-raw &&
	sort old-raw >old-list &&
	sort new-raw >new-list
	EOF

	echo "modified" >file2 &&
	git add file2 &&
	echo file1 >expected-old &&
	printf "%s\n" file1 file2 >expected-new &&
	test_cmp expected-old old-list &&
	test_cmp expected-new new-list
'

test_expect_success 'hook rejection rolls back index unchanged' '
	test_when_finished "rm -f file before after old-raw new-raw \
			    old-list new-list expected-old expected-new" &&
	echo "initial" >file &&
	git add file &&
	git commit -m "initial" &&
	git diff --cached --name-only HEAD >before &&
	test_hook pre-add <<-\EOF &&
	GIT_INDEX_FILE="$1" git diff --cached --name-only HEAD >old-raw &&
	GIT_INDEX_FILE="$2" git diff --cached --name-only HEAD >new-raw &&
	sort old-raw >old-list &&
	sort new-raw >new-list &&
	exit 1
	EOF

	echo "modified" >file &&
	test_must_fail git add file &&
	git diff --cached --name-only HEAD >after &&
	test_cmp before after &&
	: >expected-old &&
	echo file >expected-new &&
	test_cmp expected-old old-list &&
	test_cmp expected-new new-list
'

test_expect_success 'hook example: block .env files' '
	test_when_finished "rm -f .env safe.txt new-paths" &&
	echo "initial" >base &&
	git add base &&
	git commit -m "initial" &&
	test_hook pre-add <<-\EOF &&
	GIT_INDEX_FILE="$2" git diff --cached --name-only HEAD >new-paths &&
	while read path
	do
		case "$path" in
		*.env|.env)
			echo "error: $path must not be staged" >&2
			exit 1
			;;
		esac
	done <new-paths
	EOF

	echo "DB_PASS=secret" >.env &&
	test_must_fail git add .env &&
	echo "safe content" >safe.txt &&
	git add safe.txt
'

test_expect_success 'hook example: block secrets in content' '
	test_when_finished "rm -f config.txt secret" &&
	echo "initial" >config.txt &&
	git add config.txt &&
	git commit -m "initial" &&
	test_hook pre-add <<-\EOF &&
	GIT_INDEX_FILE="$2" git diff --cached HEAD >secret &&
	if grep -qE "(API_KEY|SECRET_KEY|PRIVATE_KEY)=" secret
	then
		echo "error: staged content contains secrets" >&2
		exit 1
	fi
	EOF

	echo "API_KEY=sksksk-live-12345" >config.txt &&
	test_must_fail git add config.txt &&
	echo "LOG_LEVEL=debug" >config.txt &&
	git add config.txt
'

test_done
