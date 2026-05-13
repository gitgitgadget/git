#!/bin/sh

test_description='Combination of submodules and multiple worktrees'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

base_path=$(pwd -P)

test_expect_success 'setup: create origin repos'  '
	git config --global protocol.file.allow always &&
	git init origin/sub &&
	test_commit -C origin/sub file1 &&
	git init origin/main &&
	test_commit -C origin/main first &&
	git -C origin/main submodule add ../sub &&
	git -C origin/main commit -m "add sub" &&
	test_commit -C origin/sub "file1 updated" file1 file1updated file1updated &&
	git -C origin/main/sub pull &&
	git -C origin/main add sub &&
	git -C origin/main commit -m "sub updated"
'

test_expect_success 'setup: clone superproject to create main worktree' '
	git clone --recursive "$base_path/origin/main" main
'

rev1_hash_main=$(git --git-dir=origin/main/.git show --pretty=format:%h -q "HEAD~1")
rev1_hash_sub=$(git --git-dir=origin/sub/.git show --pretty=format:%h -q "HEAD~1")

test_expect_success 'add superproject worktree' '
	git -C main worktree add "$base_path/worktree" "$rev1_hash_main"
'

test_expect_failure 'submodule is checked out just after worktree add (without flag)' '
	git -C worktree diff --submodule main"^!" >out &&
	grep "file1 updated" out
'

test_expect_success 'worktree add --recurse-submodules initializes submodules' '
	git -C main worktree add --recurse-submodules \
		"$base_path/worktree-recurse" "$rev1_hash_main" &&
	git -C worktree-recurse diff --submodule main"^!" >out &&
	grep "file1 updated" out
'

test_expect_success 'submodule in --recurse-submodules worktree uses per-worktree gitdir' '
	# The per-worktree submodule gitdir must live under the worktree entry,
	# not under $GIT_COMMON_DIR/modules/, so it is cleaned up with the
	# worktree and does not disturb the main worktree submodule.
	sub_gitdir="$base_path/main/.git/modules/sub/worktrees/worktree-recurse" &&
	test -d "$sub_gitdir" &&
	# .git pointer in the working tree must reference the per-worktree gitdir
	echo "gitdir: ../../main/.git/modules/sub/worktrees/worktree-recurse" \
		>expect-gitfile &&
	cat "$base_path/worktree-recurse/sub/.git" >actual-gitfile &&
	test_cmp expect-gitfile actual-gitfile &&
	# The per-worktree gitdir must have a commondir file pointing at the
	# shared submodule repo, not its own object store.
	echo "../.." >expect-commondir &&
	test_cmp expect-commondir "$sub_gitdir/commondir" &&
	test_path_is_missing "$sub_gitdir/objects" &&
	# The working tree is populated (test_commit creates <name>.t files)
	test -f "$base_path/worktree-recurse/sub/file1.t"
'

test_expect_success 'add superproject worktree and initialize submodules' '
	git -C main worktree add "$base_path/worktree-submodule-update" "$rev1_hash_main" &&
	git -C worktree-submodule-update submodule update
'

test_expect_success 'submodule is checked out just after submodule update in linked worktree' '
	git -C worktree-submodule-update diff --submodule main"^!" >out &&
	grep "file1 updated" out
'

test_expect_success 'add superproject worktree and manually add submodule worktree' '
	git -C main worktree add "$base_path/linked_submodule" "$rev1_hash_main" &&
	git -C main/sub worktree add "$base_path/linked_submodule/sub" "$rev1_hash_sub"
'

test_expect_success 'submodule is checked out after manually adding submodule worktree' '
	git -C linked_submodule diff --submodule main"^!" >out &&
	grep "file1 updated" out
'

test_expect_success 'checkout --recurse-submodules uses $GIT_DIR for submodules in a linked worktree' '
	git -C main worktree add "$base_path/checkout-recurse" --detach  &&
	git -C checkout-recurse submodule update --init &&
	echo "gitdir: ../../main/.git/modules/sub/worktrees/checkout-recurse" >expect-gitfile &&
	cat checkout-recurse/sub/.git >actual-gitfile &&
	test_cmp expect-gitfile actual-gitfile &&
	git -C main/sub rev-parse HEAD >expect-head-main &&
	git -C checkout-recurse checkout --recurse-submodules HEAD~1 &&
	cat checkout-recurse/sub/.git >actual-gitfile &&
	git -C main/sub rev-parse HEAD >actual-head-main &&
	test_cmp expect-gitfile actual-gitfile &&
	test_cmp expect-head-main actual-head-main
'

test_expect_success 'per-worktree submodule gitdir uses commondir; shared config is unchanged' '
	# The shared submodule repo core.worktree points at the main worktree.
	echo "../../../sub" >expect-main &&
	git -C main/sub config --get core.worktree >actual-main &&
	test_cmp expect-main actual-main &&

	# The per-worktree submodule gitdir has a commondir file pointing at
	# the shared submodule repo, not its own objects or refs.
	linked_sm_gitdir="main/.git/modules/sub/worktrees/checkout-recurse" &&
	test -f "$linked_sm_gitdir/commondir" &&
	echo "../.." >expect-commondir &&
	test_cmp expect-commondir "$linked_sm_gitdir/commondir" &&

	# Checking out a commit that removes the submodule leaves the shared
	# submodule repo intact.
	git -C checkout-recurse checkout --recurse-submodules first &&
	git -C main/sub config --get core.worktree >actual-main &&
	test_cmp expect-main actual-main
'

test_expect_success 'worktree remove cleans up per-worktree submodule gitdir' '
	git -C main worktree add "$base_path/remove-recurse" "$rev1_hash_main" &&
	git -C remove-recurse submodule update --init &&
	test -d "main/.git/modules/sub/worktrees/remove-recurse" &&
	git -C main worktree remove remove-recurse &&
	test_path_is_missing "main/.git/worktrees/remove-recurse" &&
	test_path_is_missing "remove-recurse" &&
	# The per-worktree submodule gitdir must also be removed.
	test_path_is_missing "main/.git/modules/sub/worktrees/remove-recurse" &&
	# The shared submodule repo must not be affected.
	test -d "main/.git/modules/sub" &&
	git -C main/sub log --oneline -1
'

test_expect_success 'unsetting core.worktree does not prevent running commands directly against the submodule repository' '
	git -C main/.git/modules/sub/worktrees/checkout-recurse log
'

test_expect_success 'auto-absorb: submodule with in-tree gitdir is absorbed on first linked-worktree submodule init' '
	# Clone the superproject without initializing the submodule, then
	# clone the submodule in-tree (legacy layout: .git/ is a directory,
	# not absorbed into $GIT_DIR/modules/).
	git clone "$base_path/origin/main" main-intree &&
	test_when_finished "rm -rf main-intree worktree-absorb" &&
	git -C main-intree submodule init &&
	git clone "$base_path/origin/sub" main-intree/sub &&
	git -C main-intree/sub checkout "$rev1_hash_sub" &&
	# The submodule gitdir is in-tree: a directory, not a pointer file.
	test -d "main-intree/sub/.git" &&
	test_path_is_missing "main-intree/.git/modules" &&

	# Initialize the submodule in a linked worktree: absorb_in_main_worktree
	# should relocate the in-tree gitdir to modules/sub, then set up the
	# per-worktree gitdir with commondir indirection.
	git -C main-intree worktree add "$base_path/worktree-absorb" "$rev1_hash_main" &&
	git -C worktree-absorb submodule update --init &&

	# Absorption happened: submodule gitdir now lives under modules/.
	test -d "main-intree/.git/modules/sub" &&
	# Per-worktree gitdir with commondir exists.
	test -f "main-intree/.git/modules/sub/worktrees/worktree-absorb/commondir" &&
	# Submodule working tree is populated.
	test -f "worktree-absorb/sub/file1.t"
'

test_expect_success 'worktree remove handles submodule with slash in name (nested modules path)' '
	# Create a superproject with a submodule whose path contains a slash so
	# that its gitdir lives at modules/nested/sub/ rather than modules/sub/.
	git init nested-super &&
	test_when_finished "rm -rf nested-super nested-worktree" &&
	git init nested-super/nested/sub &&
	test_commit -C nested-super/nested/sub file1 &&
	(
		cd nested-super &&
		git -c protocol.file.allow=always submodule add ./nested/sub nested/sub &&
		git commit -m "add nested/sub"
	) &&

	git -C nested-super worktree add "$base_path/nested-worktree" HEAD &&
	git -C nested-worktree submodule update --init &&

	# The per-worktree gitdir should be at modules/nested/sub/ with commondir.
	test -f "nested-super/.git/modules/nested/sub/worktrees/nested-worktree/commondir" &&

	# worktree remove must succeed: all submodule gitdirs are per-worktree.
	git -C nested-super worktree remove "$base_path/nested-worktree" &&
	test_path_is_missing "nested-super/.git/worktrees/nested-worktree" &&
	test_path_is_missing "nested-worktree"
'

test_done
