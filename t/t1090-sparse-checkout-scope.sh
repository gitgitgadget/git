#!/bin/sh

test_description='sparse checkout scope tests'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

TEST_CREATE_REPO_NO_TEMPLATE=1
. ./test-lib.sh

test_expect_success 'setup' '
	echo "initial" >a &&
	echo "initial" >b &&
	echo "initial" >c &&
	git add a b c &&
	git commit -m "initial commit"
'

test_expect_success 'create feature branch' '
	git checkout -b feature &&
	echo "modified" >b &&
	echo "modified" >c &&
	git add b c &&
	git commit -m "modification"
'

test_expect_success 'perform sparse checkout of main' '
	git config --local --bool core.sparsecheckout true &&
	mkdir .git/info &&
	echo "!/*" >.git/info/sparse-checkout &&
	echo "/a" >>.git/info/sparse-checkout &&
	echo "/c" >>.git/info/sparse-checkout &&
	git checkout main &&
	test_path_is_file a &&
	test_path_is_missing b &&
	test_path_is_file c
'

test_expect_success 'merge feature branch into sparse checkout of main' '
	git merge feature &&
	test_path_is_file a &&
	test_path_is_missing b &&
	test_path_is_file c &&
	test "$(cat c)" = "modified"
'

test_expect_success 'return to full checkout of main' '
	git checkout feature &&
	echo "/*" >.git/info/sparse-checkout &&
	git checkout main &&
	test_path_is_file a &&
	test_path_is_file b &&
	test_path_is_file c &&
	test "$(cat b)" = "modified"
'

test_expect_success 'skip-worktree on files outside sparse patterns' '
	git sparse-checkout disable &&
	git sparse-checkout set --no-cone "a*" &&
	git checkout-index --all --ignore-skip-worktree-bits &&

	git ls-files -t >output &&
	! grep ^S output >actual &&
	test_must_be_empty actual &&

	test_config sparse.expectFilesOutsideOfPatterns true &&
	cat <<-\EOF >expect &&
	S b
	S c
	EOF
	git ls-files -t >output &&
	grep ^S output >actual &&
	test_cmp expect actual
'

test_expect_success 'in partial clone, sparse checkout only fetches needed blobs' '
	test_create_repo server &&
	git clone --template= "file://$(pwd)/server" client &&

	test_config -C server uploadpack.allowfilter 1 &&
	test_config -C server uploadpack.allowanysha1inwant 1 &&
	echo a >server/a &&
	echo bb >server/b &&
	mkdir server/c &&
	echo ccc >server/c/c &&
	git -C server add a b c/c &&
	git -C server commit -m message &&

	test_config -C client core.sparsecheckout 1 &&
	mkdir client/.git/info &&
	echo "!/*" >client/.git/info/sparse-checkout &&
	echo "/a" >>client/.git/info/sparse-checkout &&
	git -C client fetch --filter=blob:none origin &&
	git -C client checkout FETCH_HEAD &&

	git -C client rev-list HEAD \
		--quiet --objects --missing=print >unsorted_actual &&
	(
		printf "?" &&
		git hash-object server/b &&
		printf "?" &&
		git hash-object server/c/c
	) >unsorted_expect &&
	sort unsorted_actual >actual &&
	sort unsorted_expect >expect &&
	test_cmp expect actual
'

test_expect_success 'setup two' '
	git init repo &&
	(
		cd repo &&
		mkdir in out1 out2 &&
		for i in $(test_seq 6)
		do
			echo "in $i" >in/"$i" &&
			echo "out1 $i" >out1/"$i" &&
			echo "out2 $i" >out2/"$i" || return 1
		done &&
		git add in out1 out2 &&
		git commit -m init &&
		for i in $(test_seq 6)
		do
			echo "in $i" >>in/"$i" &&
			echo "out1 $i" >>out1/"$i" || return 1
		done &&
		git add in out1 &&
		git commit -m change &&
		git sparse-checkout set "in"
	)
'

reset_sparse_checkout_state() {
	git -C repo reset --hard HEAD &&
	git -C repo sparse-checkout reapply
}

reset_and_change_index() {
	reset_sparse_checkout_state &&
	# add new ce
	oid=$(echo "new thing" | git -C repo hash-object --stdin -w) &&
	git -C repo update-index --add --cacheinfo 100644 $oid in/7 &&
	git -C repo update-index --add --cacheinfo 100644 $oid out1/7 &&
	# rm ce
	git -C repo update-index --remove in/6 &&
	git -C repo update-index --remove out1/6 &&
	# modify ce
	git -C repo update-index --cacheinfo 100644 $oid out1/5 &&
	# mv ce1 -> ce2
	oid=$(git -C repo ls-files --format="%(objectname)" in/4) &&
	git -C repo update-index --add --cacheinfo 100644 $oid in/8 &&
	git -C repo update-index --remove in/4 &&
	oid=$(git -C repo ls-files --format="%(objectname)" out1/4) &&
	git -C repo update-index --add --cacheinfo 100644 $oid out1/8 &&
	git -C repo update-index --remove out1/4 &&
	# chmod ce
	git -C repo update-index --chmod +x in/3 &&
	git -C repo update-index --chmod +x out1/3
}

reset_and_change_worktree() {
	reset_sparse_checkout_state &&
	rm -rf repo/out1 repo/out2 &&
	mkdir repo/out1 repo/out2 &&
	# add new file
	echo "in 7" >repo/in/7 &&
	echo "out1 7" >repo/out1/7 &&
	git -C repo add --sparse in/7 out1/7 &&
	# create out old file
	>repo/out1/6 &&
	# rm file
	rm repo/in/6 &&
	# modify file
	echo "out1 x" >repo/out1/5 &&
	# mv file1 -> file2
	mv repo/in/4 repo/in/3 &&
	# chmod file
	chmod +x repo/in/2 &&
	# add new file, mark skipworktree
	echo "in 8" >repo/in/8 &&
	echo "out1 8" >repo/out1/8 &&
	echo "out2 8" >repo/out2/8 &&
	git -C repo add --sparse in/8 out1/8 out2/8 &&
	git -C repo update-index --skip-worktree in/8 &&
	git -C repo update-index --skip-worktree out1/8 &&
	git -C repo update-index --skip-worktree out2/8 &&
	rm repo/in/8 repo/out1/8
}

# git diff --cached REV

test_expect_success 'git diff --cached --scope=all' '
	reset_and_change_index &&
	cat >expected <<-EOF &&
M	in/1
M	in/2
M	in/3
M	in/4
M	in/5
M	in/6
A	in/7
A	in/8
M	out1/1
M	out1/2
M	out1/3
M	out1/5
D	out1/6
A	out1/7
R050	out1/4	out1/8
	EOF
	git -C repo diff --name-status --cached --scope=all HEAD~ >actual &&
	test_cmp expected actual
'

test_expect_success 'git diff --cached --scope=sparse' '
	reset_and_change_index &&
	cat >expected <<-EOF &&
M	in/1
M	in/2
M	in/3
M	in/4
M	in/5
M	in/6
A	in/7
A	in/8
M	out1/3
M	out1/5
D	out1/6
A	out1/7
R050	out1/4	out1/8
	EOF
	git -C repo diff --name-status --cached --scope=sparse HEAD~ >actual &&
	test_cmp expected actual
'

# git diff REV

test_expect_success 'git diff REVISION --scope=all' '
	reset_and_change_worktree &&
	cat >expected <<-EOF &&
M	in/1
M	in/2
M	in/3
D	in/4
M	in/5
D	in/6
A	in/7
M	out1/1
M	out1/2
M	out1/3
M	out1/4
M	out1/5
M	out1/6
A	out1/7
A	out2/8
	EOF
	git -C repo diff --name-status --scope=all HEAD~ >actual &&
	test_cmp expected actual
'

test_expect_success 'git diff REVISION --scope=sparse' '
	reset_and_change_worktree &&
	cat >expected <<-EOF &&
M	in/1
M	in/2
M	in/3
D	in/4
M	in/5
D	in/6
A	in/7
M	out1/5
M	out1/6
A	out1/7
A	out2/8
	EOF
	git -C repo diff --name-status --scope=sparse HEAD~ >actual &&
	test_cmp expected actual
'

# git diff REV1 REV2

test_expect_success 'git diff two REVISION --scope=all' '
	reset_sparse_checkout_state &&
	cat >expected <<-EOF &&
M	in/1
M	in/2
M	in/3
M	in/4
M	in/5
M	in/6
M	out1/1
M	out1/2
M	out1/3
M	out1/4
M	out1/5
M	out1/6
	EOF
	git -C repo diff --name-status --scope=all HEAD~ HEAD >actual &&
	test_cmp expected actual
'

test_expect_success 'git diff two REVISION --scope=sparse' '
	reset_sparse_checkout_state &&
	cat >expected <<-EOF &&
M	in/1
M	in/2
M	in/3
M	in/4
M	in/5
M	in/6
	EOF
	git -C repo diff --name-status --scope=sparse HEAD~ HEAD >actual &&
	test_cmp expected actual
'

# git diff-index

test_expect_success 'git diff-index --cached --scope=all' '
	reset_and_change_index &&
	cat >expected <<-EOF &&
M	in/1
M	in/2
M	in/3
M	in/4
M	in/5
M	in/6
A	in/7
A	in/8
M	out1/1
M	out1/2
M	out1/3
D	out1/4
M	out1/5
D	out1/6
A	out1/7
A	out1/8
	EOF
	git -C repo diff-index --name-status --cached --scope=all HEAD~ >actual &&
	test_cmp expected actual
'

test_expect_success 'git diff-index --cached --scope=sparse' '
	reset_and_change_index &&
	cat >expected <<-EOF &&
M	in/1
M	in/2
M	in/3
M	in/4
M	in/5
M	in/6
A	in/7
A	in/8
M	out1/3
D	out1/4
M	out1/5
D	out1/6
A	out1/7
A	out1/8
	EOF
	git -C repo diff-index --name-status --cached --scope=sparse HEAD~ >actual &&
	test_cmp expected actual
'

# git diff-tree

test_expect_success 'git diff-tree --scope=all' '
	reset_sparse_checkout_state &&
	cat >expected <<-EOF &&
M	in
M	out1
	EOF
	git -C repo diff-tree --name-status --scope=all HEAD~ HEAD >actual &&
	test_cmp expected actual
'

test_expect_success 'git diff-tree --scope=sparse' '
	reset_sparse_checkout_state &&
	cat >expected <<-EOF &&
M	in
	EOF
	git -C repo diff-tree --name-status --scope=sparse HEAD~ HEAD >actual &&
	test_cmp expected actual
'

test_done
