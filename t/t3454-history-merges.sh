#!/bin/sh

test_description='git replay and git history across merge commits

Exercises the merge-replay path in `git history reword`,
`git history split`, and `git replay` using the `test-tool
historian` fixture builder so each scenario is described in a
small declarative input rather than a sprawling sequence of
plumbing commands.
'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

# Replace the commit's message via a fake editor and run reword.
reword_to () {
	GIT_EDITOR="echo \"$1\" >" \
	git history reword "$2"
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
	commit A main "A" a:a
	commit B topic "B (introduces g)" parent=A a:a g:g
	commit C main "C (introduces h)" parent=A a:a h:h
	commit M main "Merge topic" parent=C parent=B a:a g:g h:h
	EOF
}

test_expect_failure 'clean merge: both sides touch unrelated files' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_clean_merge &&

		reword_to "AA" A &&

		# The merge is still a 2-parent merge with the same subject
		# and tree (clean replay leaves content unchanged).
		test_cmp_rev HEAD^{tree} M^{tree} &&
		test_cmp_rev HEAD^^{tree} M^^{tree} &&
		test_cmp_rev HEAD^2^{tree} M^2^{tree} &&

		echo "Merge topic" >expect &&
		git log -1 --format=%s HEAD >actual &&
		test_cmp expect actual
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
	commit A main "A" a:a_v1
	commit B topic "B (line2 on topic)" parent=A a:a_topic
	commit C main "C (line2 on main)" parent=A a:a_main
	commit M main "Merge topic" parent=C parent=B a:a_resolution
	EOF
}

test_expect_failure 'non-trivial merge: textual manual resolution is preserved' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_textual_resolution &&

		reword_to "AA" A &&

		test_write_lines line1 line2-merged-by-hand line3 >expect &&
		git show HEAD:a >actual &&
		test_cmp expect actual
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
	commit A main "A" a:a_v1
	commit B topic "B (line2 on topic)" parent=A a:a_topic
	commit C main "C (line2 on main)" parent=A a:a_main
	commit M main "Merge topic" parent=C parent=B a:a_resolution
	EOF
}

test_expect_failure 'non-trivial merge: semantic edit outside conflict region is preserved' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_semantic_edit &&

		reword_to "AA" A &&

		test_write_lines line1 line2-merged line3 line4 line5-touched \
			>expect &&
		git show HEAD:a >actual &&
		test_cmp expect actual
	)
'

build_octopus () {
	test-tool historian <<-\EOF
	blob a "x"
	commit A main "A" a:a
	commit B b1 "B" parent=A a:a
	commit C b2 "C" parent=A a:a
	commit D b3 "D" parent=A a:a
	commit O main "octopus" parent=A parent=B parent=C parent=D a:a
	EOF
}

test_expect_failure 'octopus merge in the rewrite path is rejected' '
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
	# Setup a topology where the rewrite range crosses a 2-parent merge
	# whose first parent sits outside that range:
	#
	#               _------- O (a=O)
	#              /          \
	#             /            M (parent1=O, parent2=R, a=M, s=top)
	#            /            /
	#   X (a=X) - A (a=A) -- R (a=R) -- T (a=T, s=top)
	#       |
	#       reword target
	#
	# The walk for `history reword A` excludes A and its ancestors,
	# so O sits outside the rewrite range and is not the boundary
	# either. Replaying M correctly requires that first parent to
	# remain at O (preserve, not replant).
	blob X X
	blob O O
	blob A A
	blob R R
	blob T T
	blob M M
	blob top "marker"
	commit X main "X" a:X
	commit O side "O" parent=X a:O
	commit A main "A" parent=X a:A
	commit R main "R" parent=A a:R
	commit T off "T" parent=R a:T s:top
	commit M main "Merge side into main" parent=O parent=R a:M s:top
	EOF
}

# A descendant merge whose first parent sits outside the rewrite
# range is a topology that any reasonable replay of merges has to
# handle correctly: the first parent must be preserved verbatim,
# while the in-range second parent is rewritten. Without that, the
# replayed merge would silently graft itself onto a different
# ancestry than the author chose, which is far worse than a loud
# failure.
test_expect_failure 'merge whose first parent sits outside the rewrite range keeps that parent' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_with_boundary_other_than_onto &&

		reword_to "AA" A &&

		# The replayed M (i.e. the tip of main) is still a 2-parent
		# merge.  Its first parent is the original O (preserved,
		# outside the rewrite range), its second parent is the
		# rewritten R.
		test_cmp_rev ! main M &&
		test_cmp_rev main^ O &&
		test_cmp_rev ! main^2 R &&
		test_cmp_rev main^2^{tree} R^{tree} &&

		# The `off` branch is changed, the `side` branch is not
		test_cmp_rev side O &&
		test_cmp_rev ! off T
	)
'

build_function_rename () {
	test-tool historian <<-\EOF
	# Truly exercise the R/O/N algorithm: The topic renames harry() to
	# hermione() (def plus caller1). main adds caller2 calling harry();
	# the original merge M manually renames caller2 to hermione(). The
	# "newer" base on a side branch contains caller2 AND a brand-new
	# caller3 calling harry(); replaying onto `newer` therefore introduces
	# caller3 into the merged tree.
	blob def_harry "void harry(void);"
	blob def_hermione "void hermione(void);"
	blob harry_call "harry();"
	blob hermione_call "hermione();"
	commit A main "A" def:def_harry caller1:harry_call
	commit B topic "B (rename)" parent=A def:def_hermione caller1:hermione_call
	commit C main "C (caller2 calls harry)" parent=A def:def_harry caller1:harry_call caller2:harry_call
	commit M main "Merge topic" parent=C parent=B def:def_hermione caller1:hermione_call caller2:hermione_call
	commit NEW newer "newer base with caller3" parent=C def:def_harry caller1:harry_call caller2:harry_call caller3:harry_call
	EOF
}

# This case checks two things at once. First, the manual semantic edit in M
# (renaming caller2) must be preserved when we replay onto a different base;
# that is the case `git replay` needs to handle correctly, even though nothing
# in the conflict markers tells us about it. Second, a file that only enters
# the tree via the rewritten parents (caller3, present on the `newer` base) is
# _not_ renamed by the replay. The replay propagates the textual diffs the user
# actually made in M; it does _not_ infer the user's symbol-level intent
# ("rename every caller of harry").  This is a known and intentional
# limitation. Symbol-aware refactoring is out of scope here, just as it is for
# plain rebases.
test_expect_failure 'preserves resolutions; does not extrapolate' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_function_rename &&

		# Replay M onto the newer base.
		git replay --ref-action=print \
			--onto NEW --ancestry-path C..main >out &&
		new_tip=$(cut -f 3 -d " " out) &&

		# def and caller1 came from B (clean cherry-pick of the rename
		# commit) and must reflect the rename.
		git show $new_tip:def >actual &&
		test_grep hermione actual &&

		git show $new_tip:caller1 >actual &&
		test_grep hermione actual &&

		# caller2 existed in the original M; its manual rename to
		# hermione() is the semantic edit the replay must preserve.
		git show $new_tip:caller2 >actual &&
		test_grep hermione actual &&

		# caller3 only exists on the newer base, so it was brought
		# in by N (the auto-merge of the rebased parents). The
		# replay has no way to know the user intended to rename
		# every caller; caller3 keeps harry(). The resulting tree
		# is therefore _not_ symbol-correct and needs a follow-up
		# edit. This is the documented limitation.
		git show $new_tip:caller3 >actual &&
		test_grep ! hermione actual
	)
'

build_diverging_conflicts () {
	test-tool historian <<-\EOF
	# Topology where only one parent of the merge sits on the
	# rewrite path:
	#
	#   _-- D --_
	#  /         \
	# A - B - C - M
	#  \
	#   E
	#
	# D is on branch `feature`, child of A.
	# E is on branch `target`, child of A.
	# M = merge(C, D), manual resolution.
	#
	# D and E conflict in different ways than C and D, therefore
	# the resolution in M cannot cover the first conflict and
	# replaying B..M onto E must fail.
	blob a L1 L2 L3 L4 L5
	blob c L1 L2 _3 L4 L5
	blob d _1 L2 +3 L4 _5
	blob m _1 L2 M3 L4 _5
	blob e L1 L2 L3 L4 +5
	commit A main "A" a:a
	commit B main "B (no change)" parent=A a:a
	commit C main "C (changes line 3 only)" parent=B a:c
	commit D feature "D (changes all odd lines)" parent=A a:d
	commit M main "Merge feature into main" parent=C parent=D a:m
	commit E target "E (changes line 5)" parent=A a:e
	EOF
}

test_expect_failure 'newly-introduced conflict in a replayed merge' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_diverging_conflicts &&

		test_must_fail git replay --ref-action=print --branches \
			--onto E --ancestry-path B..M >out 2>err &&

		# Discriminate against the "git replay refuses 2-parent
		# merges entirely" failure mode that earlier commits
		# in this series exhibit: the test must fail because of
		# a real content conflict on `b`, not for any other
		# reason.
		test_grep "CONFLICT (content)" err &&
		test_grep "Merge conflict in b" err
	)
'

build_new_conflict_binary () {
	test-tool historian <<-\EOF
	# New conflicts are routed through the binary merge driver.
	blob attrs "* -text"
	blob bin_orig original
	blob bin_D dee
	blob bin_E ehh
	blob misc_orig "misc orig"
	blob misc_C "misc changed by C"
	commit A main "A" .gitattributes:attrs bin:bin_orig misc:misc_orig
	commit B main "B (no change)" parent=A .gitattributes:attrs bin:bin_orig misc:misc_orig
	commit C main "C (changes misc)" parent=B .gitattributes:attrs bin:bin_orig misc:misc_C
	commit D feature "D (changes bin)" parent=A .gitattributes:attrs bin:bin_D misc:misc_orig
	commit M main "Merge feature" parent=C parent=D .gitattributes:attrs bin:bin_D misc:misc_C
	commit E target "E (changes bin)" parent=A .gitattributes:attrs bin:bin_E misc:misc_orig
	EOF
}

test_expect_failure 'newly-introduced binary conflict is reported' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_new_conflict_binary &&

		test_must_fail git replay --ref-action=print --branches \
			--onto E --ancestry-path B..M >out 2>err &&

		# Discriminate against the "git replay refuses 2-parent
		# merges entirely" failure mode (no conflict message at
		# all). The replay must fail because of a real content
		# conflict on `bin`, not for any other reason.
		test_grep "CONFLICT (content)" err &&
		test_grep "Merge conflict in bin" err
	)
'

build_vanishing_conflict () {
	test-tool historian <<-\EOF
	#     B
	#   /   \
	# A - C - M
	#       \
	#         D - X (= cherry-picked B)
	#
	# X cherry-picks B on top of D that replaying C..M onto X
	# no longer has a merge conflict
	blob a L1 L2 L3 L4
	blob b L1 L2 _3 L4
	blob c L1 L2 +3 L4
	blob m _1 L2 _3 L4
	blob d L1 L2 +3 L4 L5
	blob x _1 L2 _3 L4 L5
	commit A main "A" a:a
	commit B main "B (changes line 3)" parent=A a:b
	commit C feature "C (changes line 3 differently)" parent=A a:c
	commit M feature "Merge C and B (fix lines 1 and 3)" parent=B parent=C a:m
	commit D feature "D (changes line 3 like B, adds a line)" parent=C a:d
	commit X feature "X (cherry-picks B with fixup)" parent=D a:x
	EOF
}

test_expect_failure 'vanishing conflict in a replayed merge' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_vanishing_conflict &&

		git replay --ref-action=print --branches \
			--onto X --ancestry-path C..M >out &&
		new_tip=$(cut -f 3 -d " " out) &&

		# Since X already includes the changes of B, it is tree-same
		# to the replayed merge
		test_cmp_rev "$new_tip:a" X:a
	)
'

build_fixture_with_marker_text () {
	test-tool historian <<-\EOF
	# A file `doc/markers.txt` whose plain content happens to
	# contain the byte sequences `<<<<<<<`, `=======`, and
	# `>>>>>>>` (e.g. a documentation page describing how Git
	# writes conflict markers). The merge replay must NOT mistake
	# those literal bytes for a real conflict on the path.
	#
	# The topology runs an actual merge replay so the regression
	# guard means something: file `a` is the one that genuinely
	# gets merged, and `doc/markers.txt` rides along unchanged
	# across the history. A correct implementation can tell that
	# `doc/markers.txt` was never the subject of an unresolved
	# inner merge and leaves it alone.
	blob a L1 L2 L3
	blob c L1 L2 _3
	blob d _1 L2 +3
	blob m _1 L2 M3
	blob markers "Example" "<<<<<<< ours" "first variant" "=======" "second variant" ">>>>>>> theirs" "EOF"
	commit A main "A" a:a
	commit B main "B (no a change)" parent=A a:a
	commit C main "C (changes a only)" parent=B a:c
	commit D feature "D (changes a only)" parent=A a:d
	commit M main "Merge feature" parent=C parent=D a:m
	commit E target "E (no a change)" parent=A a:a doc:markers
	EOF
}

test_expect_failure 'file containing literal conflict-marker bytes survives a replay-merge' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_fixture_with_marker_text &&

		git replay --ref-action=print --branches \
			--onto E --ancestry-path B..M >out &&
		new_tip=$(cut -f 3 -d " " out) &&
		test -n "$new_tip" &&

		# The fixture file is byte-for-byte identical across the
		# whole history. Nothing about replay should have touched it.
		test_cmp_rev "$new_tip:doc" E:doc
	)
'

build_resolution_preserved_under_diff3 () {
	test-tool historian <<-\EOF
	# Topology where the inner remerge of (C, D) and the inner
	# remerge of (C', D) have DIFFERENT merge bases: the original
	# pair's merge base is A (close, on main), while the rewritten
	# pair's merge base is R (far, where the long-lived target
	# branch diverged before A).
	#
	#       R ---- E    (target branch)
	#        \
	#         A ----+---- B ---- C ---- M    (main)
	#                \                  /
	#                 +------ D --------+    (feature)
	#
	# Under `merge.conflictStyle=diff3`, conflict markers embed
	# the merge base's abbreviated OID. When the inner remerges
	# have different bases, the abbreviated-OID line in the
	# markers differs even though both inner remerges produce
	# byte-identical content for the conflicting region. A naive
	# implementation lets that label difference look like a real
	# delta between the two inner remerges and reports a spurious
	# conflict where the original resolution should have collapsed
	# through. The replay must pin the ancestor label across the
	# two inner remerges so byte-identical conflicting content
	# really compares as byte-identical.
	#
	# E touches a file `b` so that C' (cherry-pick of C onto E)
	# carries a different tree from C; otherwise the trivial
	# tree-equal fast path in the outer pick collapses everything
	# and the diff3 marker text is never exercised.
	blob a L1 L2 L3
	blob c L1 L2 _3
	blob d L1 L2 +3
	blob m L1 L2 M3
	blob b b_orig
	blob e b_E_only
	commit R main "R (root)" a:a b:b
	commit A main "A" parent=R a:a b:b
	commit B main "B (no change)" parent=A a:a b:b
	commit C main "C (changes a)" parent=B a:c b:b
	commit D feature "D (changes a differently)" parent=A a:d b:b
	commit M main "Merge feature into main" parent=C parent=D a:m b:b
	commit E target "E (changes b only)" parent=R a:a b:e
	EOF
}

test_expect_failure 'previously-resolved conflicts with diff3' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		git config merge.conflictStyle diff3 &&
		build_resolution_preserved_under_diff3 &&

		git replay --ref-action=print --branches \
			--onto E --ancestry-path B..M >out &&
		new_tip=$(cut -f 3 -d " " out) &&
		test -n "$new_tip" &&

		test_write_lines L1 L2 M3 >expect &&
		git show "$new_tip:a" >actual &&
		test_cmp expect actual
	)
'

test_done
