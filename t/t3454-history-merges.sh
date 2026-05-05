#!/bin/sh

test_description='git history reword across merge commits

Exercises the merge-replay path in `git history reword` using the
`test-tool historian` test fixture builder so each scenario is
described in a small declarative input rather than a sprawling
sequence of plumbing commands. The interesting cases are:

  * a clean merge with each side touching unrelated files;
  * a non-trivial merge whose conflicting line was resolved by hand
    (textually) and whose resolution must be preserved through the
    replay;
  * a non-trivial merge with a manual *semantic* edit (an additional
    change outside the conflict region) that must also be preserved.
'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

# Replace the commit's message via a fake editor and run reword.
reword_to () {
	new_msg="$1"
	target="$2"
	write_script fake-editor.sh <<-EOF &&
	echo "$new_msg" >"\$1"
	EOF
	test_set_editor "$(pwd)/fake-editor.sh" &&
	git history reword "$target" &&
	rm fake-editor.sh
}

build_clean_merge () {
	test-tool historian <<-\EOF
	# Setup:
	#       A (a) --- C (a, h) ----+--- M (a, g, h)
	#        \                    /
	#         +-- B (a, g) ------+
	#
	# Topic touches `g` only; main touches `h` only. The auto-merge
	# at M is clean.
	blob a "shared content"
	blob g guarded
	blob h host
	commit A main "A" a=a
	commit B topic "B (introduces g)" from=A a=a g=g
	commit C main "C (introduces h)" a=a h=h
	commit M main "Merge topic" merge=B a=a g=g h=h
	EOF
}

test_expect_success 'clean merge: both sides touch unrelated files' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_clean_merge &&

		reword_to "AA" A &&

		# The merge is still a 2-parent merge with the same subject
		# and tree (clean replay leaves content unchanged).
		test_cmp_rev HEAD^{tree} M^{tree} &&

		echo "Merge topic" >expect-subject &&
		git log -1 --format=%s HEAD >subject &&
		test_cmp expect-subject subject &&

		git rev-list --merges HEAD~..HEAD >merges &&
		test_line_count = 1 merges
	)
'

build_textual_resolution () {
	test-tool historian <<-\EOF
	# Both sides change the same line of `a`; the user resolved with
	# their own combined text, recorded directly as the merge tree.
	blob a_v1 line1 line2 line3
	blob a_main line1 line2-main line3
	blob a_topic line1 line2-topic line3
	blob a_resolution line1 line2-merged-by-hand line3
	commit A main "A" a=a_v1
	commit B topic "B (line2 on topic)" from=A a=a_topic
	commit C main "C (line2 on main)" a=a_main
	commit M main "Merge topic" merge=B a=a_resolution
	EOF
}

test_expect_success 'non-trivial merge: textual manual resolution is preserved' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_textual_resolution &&

		reword_to "AA" A &&

		git show HEAD:a >after &&
		test_write_lines line1 line2-merged-by-hand line3 >expect &&
		test_cmp expect after
	)
'

build_semantic_edit () {
	test-tool historian <<-\EOF
	# Topic and main conflict on line2 of `a`. The user's resolution
	# at M not only picks combined text on line2 but ALSO touches
	# line5 (a "semantic" edit outside any conflict region) -- this
	# kind of edit is invisible to a naive pick-one-side strategy and
	# must be preserved by replay.
	blob a_v1 line1 line2 line3 line4 line5
	blob a_main line1 line2-main line3 line4 line5
	blob a_topic line1 line2-topic line3 line4 line5
	blob a_resolution line1 line2-merged line3 line4 line5-touched
	commit A main "A" a=a_v1
	commit B topic "B (line2 on topic)" from=A a=a_topic
	commit C main "C (line2 on main)" a=a_main
	commit M main "Merge topic" merge=B a=a_resolution
	EOF
}

test_expect_success 'non-trivial merge: semantic edit outside conflict region is preserved' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_semantic_edit &&

		reword_to "AA" A &&

		git show HEAD:a >after &&
		test_write_lines line1 line2-merged line3 line4 line5-touched \
			>expect &&
		test_cmp expect after
	)
'

build_octopus () {
	test-tool historian <<-\EOF
	blob a "x"
	commit A main "A" a=a
	commit B b1 "B" from=A a=a
	commit C b2 "C" from=A a=a
	commit D b3 "D" from=A a=a
	commit O main "octopus" merge=B merge=C merge=D a=a
	EOF
}

test_expect_success 'octopus merge in the rewrite path is rejected' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_octopus &&

		test_must_fail git -c core.editor=true history reword \
			--dry-run A 2>err &&
		test_grep "octopus" err
	)
'

build_with_boundary_other_than_onto () {
	test-tool historian <<-\EOF
	# Setup an "evil merge" topology where the rewrite range crosses
	# a 2-parent merge whose first parent sits outside that range:
	#
	#   side -- O (a=v0)
	#            \
	#             M (parent1=O, parent2=R, a=v0, s=top)
	#            /
	#   A (a=v0) -- R (a=v0) -- T (a=v0, s=top)
	#       |
	#       reword target
	#
	# The walk for `history reword A` excludes A and its ancestors,
	# so O sits outside the rewrite range and is not the boundary
	# either. Replaying M correctly requires that first parent to
	# remain at O (preserve, not replant).
	blob v0 line1 line2 line3
	blob top "marker"
	commit X side "X" v0=v0
	commit O side "O" v0=v0
	commit A main "A" from=X v0=v0
	commit R main "R" v0=v0
	commit M main "Merge side into main" from=O merge=R v0=v0 s=top
	commit T main "T" v0=v0 s=top
	EOF
}

# A descendant merge whose first parent sits outside the rewrite
# range is a topology that any reasonable replay of merges has to
# handle correctly: the first parent must be preserved verbatim,
# while the in-range second parent is rewritten. Without that, the
# replayed merge would silently graft itself onto a different
# ancestry than the author chose, which is far worse than a loud
# failure.
test_expect_success 'merge whose first parent sits outside the rewrite range keeps that parent' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_with_boundary_other_than_onto &&

		reword_to "AA" A &&

		# The replayed M (now HEAD~) is still a 2-parent merge.
		# Its first parent is the original O (preserved, outside
		# the rewrite range), its second parent is the rewritten
		# R. T was rebased on top of M, so HEAD = T.
		git rev-list --parents -1 HEAD~ >parents &&
		new_p1=$(awk "{print \$2}" parents) &&
		new_p2=$(awk "{print \$3}" parents) &&

		# First parent is preserved verbatim.
		test_cmp_rev O $new_p1 &&

		# Second parent is the rewritten R: a fresh commit whose
		# subject is still "R" but whose OID differs from the
		# original (because its parent A is now reworded).
		echo R >expect &&
		git log -1 --format=%s $new_p2 >actual &&
		test_cmp expect actual &&
		! test_cmp_rev R $new_p2 &&

		# T was rebased on top of the new M, and its tree still
		# contains the s=top marker introduced in the original M.
		echo "marker" >expect &&
		git show HEAD:s >actual &&
		test_cmp expect actual
	)
'

build_function_rename () {
	test-tool historian <<-\EOF
	# Topic renames harry() -> hermione() (defs.h plus caller1). main
	# adds caller2 calling harry(); the original merge M manually
	# renames caller2 to hermione(). The "newer" base on a side branch
	# contains caller2 AND a brand-new caller3 calling harry();
	# replaying onto `newer` therefore introduces caller3 into the
	# merged tree.
	blob defs_harry "void harry(void);"
	blob defs_hermione "void hermione(void);"
	blob harry_call "harry();"
	blob hermione_call "hermione();"
	commit A main "A" defs.h=defs_harry caller1=harry_call
	commit B topic "B (rename)" from=A defs.h=defs_hermione caller1=hermione_call
	commit C main "C (caller2 calls harry)" defs.h=defs_harry caller1=harry_call caller2=harry_call
	commit M main "Merge topic" merge=B defs.h=defs_hermione caller1=hermione_call caller2=hermione_call
	commit NEW newer "newer base with caller3" from=A defs.h=defs_harry caller1=harry_call caller2=harry_call caller3=harry_call
	EOF
}

# This case checks two things at once. First, the manual semantic
# edit in M (renaming caller2) must be preserved when we replay onto
# a different base; that is the case `git history` and `git replay`
# need to handle correctly, even though nothing in the conflict
# markers tells us about it. Second, a file that only enters the
# tree via the rewritten parents (caller3, present on the `newer`
# base) is _not_ renamed by the replay. The replay propagates the
# textual diffs the user actually made in M; it does _not_ infer
# the user's symbol-level intent ("rename every caller of harry").
# This is a known and intentional limitation. Symbol-aware
# refactoring is out of scope here, just as it is for plain rebase.
test_expect_success 'preserves manual rename of pre-existing caller; does not extrapolate to new files' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_function_rename &&

		# Replay (C, B, M) onto the newer base. A `main..M` style
		# range across two unrelated branches is awkward; spin up a
		# temp branch and use --advance.
		git branch tmp main &&
		git replay --ref-action=print --onto NEW A..tmp >result &&
		new_tip=$(cut -f 3 -d " " result) &&

		# defs.h and caller1 came from B (clean cherry-pick of the
		# rename commit) and must reflect the rename.
		echo "void hermione(void);" >expect &&
		git show $new_tip:defs.h >actual &&
		test_cmp expect actual &&

		echo "hermione();" >expect &&
		git show $new_tip:caller1 >actual &&
		test_cmp expect actual &&

		# caller2 existed in the original M; its manual rename to
		# hermione() is the semantic edit the replay must preserve.
		echo "hermione();" >expect &&
		git show $new_tip:caller2 >actual &&
		test_cmp expect actual &&

		# caller3 only exists on the newer base, so it was brought
		# in by N (the auto-merge of the rewritten parents). The
		# replay has no way to know the user intended to rename
		# every caller; caller3 keeps harry(). The resulting tree
		# is therefore _not_ symbol-correct and needs a follow-up
		# edit. This is the documented limitation.
		echo "harry();" >expect &&
		git show $new_tip:caller3 >actual &&
		test_cmp expect actual
	)
'

test_done
