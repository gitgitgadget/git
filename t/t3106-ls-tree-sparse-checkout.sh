#!/bin/sh

test_description='ls-tree with sparse filter patterns'

. ./test-lib.sh

check_agrees_with_ls_files () {
	REPO=repo  &&
	git -C $REPO submodule deinit -f --all &&
	git -C $REPO cat-file -p $filter_oid >"$REPO/.git/info/sparse-checkout" &&
	git -C $REPO sparse-checkout init --cone &&
	git -C $REPO submodule init &&
	git -C $REPO ls-files -t >out &&
	sed -n "/^S /!s/^. //p" out >expect &&
	test_cmp expect actual
}

check_same_result_in_bare_repo () {
	FILTER_OID=$1 &&
	git -C repo cat-file -p $FILTER_OID >out &&
	git -C bare hash-object -w --stdin <out &&
	git -C bare ls-tree --name-only --filter-sparse-oid=$FILTER_OID -r HEAD >bare-result &&
	test_cmp expect bare-result
}

test_expect_success 'setup' '
	git init submodule &&
	test_commit -C submodule file &&
	git init repo &&
	(
		cd repo &&
		mkdir dir &&
		test_commit dir/sub-file &&
		test_commit dir/sub-file2 &&
		mkdir dir2 &&
		test_commit dir2/sub-file1 &&
		test_commit dir2/sub-file2 &&
		test_commit top-file &&
		git clone ../submodule submodule &&
		git submodule add ./submodule &&
		git submodule absorbgitdirs &&
		git commit -m"add submodule" &&
		git sparse-checkout init --cone
	) &&
	git clone --bare ./repo bare
'

test_expect_success 'toplevel filter only shows toplevel file' '
	filter_oid=$(git -C repo hash-object -w --stdin <<-\EOF
	/*
	!/*/
	EOF
	) &&
	cat >expect <<-EOF &&
	.gitmodules
	submodule
	top-file.t
	EOF
	git -C repo ls-tree --name-only --filter-sparse-oid=$filter_oid -r HEAD >actual &&
	test_cmp expect actual &&
	check_agrees_with_ls_files &&
	check_same_result_in_bare_repo $filter_oid
'

test_expect_success 'non cone single file filter' '
	filter_oid=$(git -C repo hash-object -w --stdin <<-\EOF
	/dir/sub-file.t
	EOF
	) &&
	cat >expect <<-EOF &&
	dir/sub-file.t
	EOF
	git -C repo ls-tree --name-only --filter-sparse-oid=$filter_oid -r HEAD >actual &&
	test_cmp expect actual &&
	check_agrees_with_ls_files &&
	check_same_result_in_bare_repo $filter_oid
'

test_expect_success 'cone filter matching one dir' '
	filter_oid=$(git -C repo hash-object -w --stdin <<-\EOF
	/*
	!/*/
	/dir/
	EOF
	) &&
	cat >expect <<-EOF &&
	.gitmodules
	dir/sub-file.t
	dir/sub-file2.t
	submodule
	top-file.t
	EOF
	git -C repo ls-tree --name-only --filter-sparse-oid=$filter_oid -r HEAD >actual &&
	test_cmp expect actual &&
	check_agrees_with_ls_files &&
	check_same_result_in_bare_repo $filter_oid
'

test_done
