#!/bin/sh

test_description='blame-tree tests'

. ./test-lib.sh

test_expect_success 'setup' '
	test_commit 1 file &&
	mkdir a &&
	test_commit 2 a/file &&
	mkdir a/b &&
	test_commit 3 a/b/file
'

test_expect_success 'cannot blame two trees' '
	test_must_fail git blame-tree HEAD HEAD~1
'

check_blame() {
	local indir= &&
	while test $# != 0
	do
		case "$1" in
		-C)
			indir="$2"
			shift
			;;
		*)
			break
			;;
		esac &&
		shift
	done &&

	cat >expect &&
	test_when_finished "rm -f tmp.*" &&
	git ${indir:+-C "$indir"} blame-tree "$@" >tmp.1 &&
	git name-rev --annotate-stdin --name-only --tags \
		<tmp.1 >tmp.2 &&
	tr '\t' ' ' <tmp.2 >tmp.3 &&
	sort tmp.3 >actual &&
	test_cmp expect actual
}

test_expect_success 'blame recursive' '
	check_blame --recursive <<-\EOF
	1 file
	2 a/file
	3 a/b/file
	EOF
'

test_expect_success 'blame non-recursive' '
	check_blame --no-recursive <<-\EOF
	1 file
	3 a
	EOF
'

test_expect_success 'blame subdir' '
	check_blame a <<-\EOF
	3 a
	EOF
'

test_expect_success 'blame subdir recursive' '
	check_blame --recursive a <<-\EOF
	2 a/file
	3 a/b/file
	EOF
'

test_expect_success 'blame from non-HEAD commit' '
	check_blame --no-recursive HEAD^ <<-\EOF
	1 file
	2 a
	EOF
'

test_expect_success 'blame from subdir defaults to root' '
	check_blame -C a --no-recursive <<-\EOF
	1 file
	3 a
	EOF
'

test_expect_success 'blame from subdir uses relative pathspecs' '
	check_blame -C a --recursive b <<-\EOF
	3 a/b/file
	EOF
'

test_expect_failure 'limit blame traversal by count' '
	check_blame --no-recursive -1 <<-\EOF
	3 a
	EOF
'

test_expect_success 'limit blame traversal by commit' '
	check_blame --no-recursive HEAD~2..HEAD <<-\EOF
	3 a
	^1 file
	EOF
'

test_expect_success 'only blame files in the current tree' '
	git rm -rf a &&
	git commit -m "remove a" &&
	check_blame <<-\EOF
	1 file
	EOF
'

test_expect_success 'cross merge boundaries in blaming' '
	git checkout HEAD^0 &&
	git rm -rf . &&
	test_commit m1 &&
	git checkout HEAD^ &&
	git rm -rf . &&
	test_commit m2 &&
	git merge m1 &&
	check_blame <<-\EOF
	m1 m1.t
	m2 m2.t
	EOF
'

test_expect_success 'blame merge for resolved conflicts' '
	git checkout HEAD^0 &&
	git rm -rf . &&
	test_commit c1 conflict &&
	git checkout HEAD^ &&
	git rm -rf . &&
	test_commit c2 conflict &&
	test_must_fail git merge c1 &&
	test_commit resolved conflict &&
	check_blame conflict <<-\EOF
	resolved conflict
	EOF
'

test_done
