#!/bin/sh

test_description='git update-index --refresh-stat-only'

. ./test-lib.sh

test_expect_success 'setup' '
	test_commit initial base-file base
'

test_expect_success '--refresh-stat-only updates stat info without rehashing' '
	test_commit refresh-stat refresh-stat original &&
	git ls-files --stage -- refresh-stat >expect &&
	git ls-files --debug refresh-stat | grep mtime >before &&
	printf "modified\n" >refresh-stat &&
	test-tool chmtime -100000 refresh-stat &&
	test_must_fail git diff-files --quiet -- refresh-stat &&
	git update-index --refresh-stat-only &&
	git ls-files --debug refresh-stat | grep mtime >after &&
	! test_cmp before after &&
	git ls-files --stage -- refresh-stat >actual &&
	test_cmp expect actual &&
	git diff-files --quiet -- refresh-stat
'

test_expect_success '--refresh-stat-only ignores assume-unchanged' '
	test_commit assume-unchanged assume-unchanged old &&
	git update-index --assume-unchanged assume-unchanged &&
	printf "new\n" >assume-unchanged &&
	test-tool chmtime -100000 assume-unchanged &&
	GIT_TEST_PRELOAD_INDEX=1 git update-index --refresh-stat-only &&
	git update-index --no-assume-unchanged assume-unchanged &&
	git diff-files --quiet -- assume-unchanged
'

test_expect_success '--refresh-stat-only with missing file and --ignore-missing' '
	test_commit missing-ignore missing-ignore content &&
	rm missing-ignore &&
	git update-index --ignore-missing --refresh-stat-only &&
	git checkout -- missing-ignore
'

test_expect_success '--refresh-stat-only reports error on missing file without --ignore-missing' '
	test_commit missing-error missing-error content &&
	rm missing-error &&
	test_must_fail git update-index --refresh-stat-only >out 2>err &&
	test_grep "needs update" out &&
	git checkout -- missing-error
'

test_expect_success '--refresh-stat-only with -q is quiet' '
	test_commit missing-quiet missing-quiet content &&
	rm missing-quiet &&
	git update-index -q --ignore-missing --refresh-stat-only >out 2>err &&
	test_must_be_empty out &&
	test_must_be_empty err
'

test_done
