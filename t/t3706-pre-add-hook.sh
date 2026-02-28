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

test_expect_success 'setup for path-based tests' '
	git add file &&
	git commit -m "initial"
'

test_expect_success 'hook receives index-path and lockfile-path arguments' '
	test_when_finished "git reset --hard &&
			    rm -f staged expect-count arg-count arg-one arg-two \
			    expect-index expect-lockpath" &&
	echo staged >staged &&
	cat >expect-count <<-\EOF &&
	2
	EOF
	test_hook pre-add <<-\EOF &&
	echo "$#" >arg-count &&
	echo "$1" >arg-one &&
	echo "$2" >arg-two &&
	test "$1" != "$2" &&
	test -r "$2"
	EOF
	git add staged &&
	test_cmp expect-count arg-count &&
	printf "%s/index\n" "$(git rev-parse --absolute-git-dir)" >expect-index &&
	test_cmp expect-index arg-one &&
	sed "s/$/.lock/" expect-index >expect-lockpath &&
	test_cmp expect-lockpath arg-two
'

test_expect_success 'hook rejection leaves final index unchanged' '
	test_when_finished "git reset --hard && rm -f reject index.before" &&
	cp .git/index index.before &&
	test_hook pre-add <<-\EOF &&
	exit 1
	EOF
	echo reject >reject &&
	test_must_fail git add reject &&
	test_cmp_bin index.before .git/index &&
	test_path_is_missing .git/index.lock
'

test_expect_success 'missing pre-existing index path treated as empty' '
	test_when_finished "git reset --hard &&
			    rm -f newfile arg-one after.raw after expect-index" &&
	rm -f .git/index &&
	test_hook pre-add <<-\EOF &&
	echo "$1" >arg-one &&
	test ! -e "$1" &&
	GIT_INDEX_FILE="$2" git diff --cached --name-only HEAD >after.raw &&
	sort after.raw >after
	EOF
	echo newfile >newfile &&
	git add newfile &&
	printf "%s/index\n" "$(git rev-parse --absolute-git-dir)" >expect-index &&
	test_cmp expect-index arg-one &&
	grep "^newfile$" after &&
	grep "^file$" after
'

test_expect_success 'hook respects GIT_INDEX_FILE' '
	test_when_finished "git reset --hard &&
			    rm -f arg-one arg-two expect-index expect-lockpath \
			    alt-index alt-index.lock" &&
	test_hook pre-add <<-\EOF &&
	echo "$1" >arg-one &&
	echo "$2" >arg-two
	EOF
	echo changed >>file &&
	GIT_INDEX_FILE=alt-index git add file &&
	echo "$PWD/alt-index" >expect-index &&
	test_cmp expect-index arg-one &&
	echo "$PWD/alt-index.lock" >expect-lockpath &&
	test_cmp expect-lockpath arg-two
'

test_expect_success 'setup for mixed-result tests' '
	echo "*.ignored" >.gitignore &&
	git add .gitignore &&
	git commit -m "add gitignore"
'

test_expect_success 'mixed-result add invokes pre-add hook' '
	test_when_finished "git reset --hard &&
			    rm -f bad.ignored index.before hook-ran proposed" &&
	echo changed >>file &&
	echo ignored >bad.ignored &&
	cp .git/index index.before &&
	test_hook pre-add <<-\EOF &&
	GIT_INDEX_FILE="$2" git diff --cached --name-only HEAD >proposed &&
	grep "^file$" proposed &&
	echo invoked >hook-ran &&
	exit 1
	EOF
	test_must_fail git add file bad.ignored &&
	test_path_is_file hook-ran &&
	test_cmp_bin index.before .git/index &&
	test_path_is_missing .git/index.lock
'

test_expect_success 'mixed-result add stages tracked update on approve' '
	test_when_finished "git reset --hard &&
			    rm -f bad.ignored hook-ran staged proposed" &&
	echo changed >>file &&
	echo ignored >bad.ignored &&
	test_hook pre-add <<-\EOF &&
	GIT_INDEX_FILE="$2" git diff --cached --name-only HEAD >proposed &&
	grep "^file$" proposed &&
	echo invoked >hook-ran
	EOF
	test_must_fail git add file bad.ignored &&
	test_path_is_file hook-ran &&
	git diff --cached --name-only HEAD >staged &&
	grep "^file$" staged &&
	test_path_is_missing .git/index.lock
'

test_expect_success 'post-index-change fires after pre-add approval' '
	test_when_finished "git reset --hard &&
			    rm -f hook-order expect lockfile-present" &&
	test_hook pre-add <<-\EOF &&
	echo pre >>hook-order
	EOF
	test_hook post-index-change <<-\EOF &&
	if test -f ".git/index.lock"
	then
		echo locked >lockfile-present
	fi
	echo post >>hook-order
	EOF
	echo updated >>file &&
	git add file &&
	cat >expect <<-\EOF &&
	pre
	post
	EOF
	test_cmp expect hook-order &&
	test_path_is_missing lockfile-present
'

test_expect_success 'post-index-change is suppressed on pre-add rejection' '
	test_when_finished "git reset --hard &&
			    rm -f index.before hook-order expect" &&
	cp .git/index index.before &&
	test_hook pre-add <<-\EOF &&
	echo pre >>hook-order &&
	exit 1
	EOF
	test_hook post-index-change <<-\EOF &&
	echo post >>hook-order
	EOF
	echo reject >>file &&
	test_must_fail git add file &&
	echo pre >expect &&
	test_cmp expect hook-order &&
	test_cmp_bin index.before .git/index &&
	test_path_is_missing .git/index.lock
'

test_expect_success '--dry-run does not invoke hook' '
	test_when_finished "rm -f hook-ran dry" &&
	test_hook pre-add <<-\EOF &&
	echo invoked >hook-ran
	EOF
	echo dry >dry &&
	git add --dry-run dry &&
	test_path_is_missing hook-ran
'

test_expect_success 'hook runs for git add -u' '
	test_when_finished "git reset --hard && rm -f hook-ran" &&
	test_hook pre-add <<-\EOF &&
	echo invoked >hook-ran
	EOF
	echo changed >>file &&
	git add -u &&
	test_path_is_file hook-ran
'

test_expect_success 'hook example: block .env files' '
	test_when_finished "git reset --hard &&
			    rm -f .env safe.txt new-paths" &&
	test_hook pre-add <<-\EOF &&
	GIT_INDEX_FILE="$2" git diff --cached --name-only HEAD >new-paths &&
	while read path
	do
		case "$path" in
		*.env)
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
	test_when_finished "git reset --hard && rm -f config.txt secret" &&
	test_hook pre-add <<-\EOF &&
	GIT_INDEX_FILE="$2" git diff --cached HEAD >secret &&
	if grep -q "API_KEY=" secret ||
	   grep -q "SECRET_KEY=" secret ||
	   grep -q "PRIVATE_KEY=" secret
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
