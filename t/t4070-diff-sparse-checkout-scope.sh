#!/bin/sh

test_description='diff sparse-checkout scope'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_success 'setup' '
	git init repo &&
	(
		cd repo &&
		mkdir in out1 out2 &&
		for i in {1..6}
		do
			echo $i >in/"$i" &&
			echo $i >out1/"$i" &&
			echo $i >out2/"$i" || return 1
		done &&
		git add . &&
		git commit -m init &&
		for i in {1..6}
		do
			echo $i >>in/"$i" &&
			echo $i >>out1/"$i" || return 1
		done &&
		git add . &&
		git commit -m change &&
		git sparse-checkout set "in"
	)
'

reset_sparse_checkout_state() {
	git -C repo reset --hard HEAD
}

reset_and_change_index() {
	reset_sparse_checkout_state &&
	# add new ce
	oid=$(echo 11 | git -C repo hash-object --stdin -w) &&
	git -C repo update-index --add --cacheinfo 100644 $oid in/7 &&
	git -C repo update-index --add --cacheinfo 100644 $oid out1/7 &&
	# rm ce
	git -C repo update-index --remove in/6 &&
	git -C repo update-index --remove out1/6 &&
	# modify ce
	git -C repo update-index --add --cacheinfo 100644 $oid out1/5 &&
	# mv ce1 -> ce2
	git -C repo mv in/4 in/8 &&
	git -C repo mv --sparse out1/4 out1/8 &&
	mv repo/in/8 repo/in/4 &&
	# chmod ce
	git -C repo update-index --chmod +x in/3 &&
	git -C repo update-index --chmod +x out1/3
}

reset_and_change_worktree() {
	reset_sparse_checkout_state &&
	rm -rf repo/out1 repo/out2 &&
	mkdir repo/out1 repo/out2 &&
	# add new file
	echo 7 >repo/in/7 &&
	echo 7 >repo/out1/7 &&
	git -C repo add --sparse in/7 out1/7 &&
	# create out old file
	>repo/out1/6 &&
	# rm file
	rm repo/in/6 &&
	# modify file
	echo 5 >repo/out1/5 &&
	# mv file1 -> file2
	mv repo/in/4 repo/in/3 &&
	# chmod file
	chmod +x repo/in/2 &&
	# add new file, mark skipworktree
	echo 8 >repo/in/8 &&
	echo 8 >repo/out1/8 &&
	echo 8 >repo/out2/8 &&
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
M	in/5
M	in/6
A	in/7
R050	in/4	in/8
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
M	in/5
M	in/6
A	in/7
R050	in/4	in/8
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
D	in/4
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
D	in/4
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
