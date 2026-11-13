#!/bin/sh

test_description='git replay and git history across merge commits'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_success 'preserves resolutions; does not extrapolate' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		test-tool historian <<-\EOF &&
		# M renames caller2; the new base adds a caller3.
		blob dh "void harry(void);"
		blob dm "void hermione(void);"
		blob h "harry();"
		blob m "hermione();"
		commit A main A def:dh caller1:h
		commit B topic B parent=A def:dm caller1:m
		commit C main C parent=A def:dh caller1:h caller2:h
		commit M main M parent=C parent=B def:dm caller1:m caller2:m
		commit N target N parent=C def:dh caller1:h caller2:h caller3:h
		EOF
		git replay --ref-action=print --onto N \
			--ancestry-path C..main >out &&
		test_line_count = 1 out &&
		new_tip=$(cut -d " " -f 3 out) &&
		test_cmp_rev "$new_tip:def" B:def &&
		test_cmp_rev "$new_tip:caller1" B:caller1 &&
		test_cmp_rev "$new_tip:caller2" M:caller2 &&
		test_cmp_rev "$new_tip:caller3" N:caller3
	)
'

build_resolution_merge () {
	test-tool historian <<-\EOF
	# The old pair has base A, the rewritten pair has base R.
	blob a L1 L2 L3
	blob c L1 L2 _3
	blob d L1 L2 +3
	blob m L1 L2 M3
	blob b old-b
	blob e new-b
	commit R main R a:a b:b
	commit A main A parent=R a:a b:b
	commit B main B parent=A a:a b:b
	commit C main C parent=B a:c b:b
	commit D feature D parent=A a:d b:b
	commit M main M parent=C parent=D a:m b:b
	commit E target E parent=R a:a b:e
	EOF
}

for style in merge diff3
do
	test_expect_success "previously resolved conflicts with $style" '
		test_when_finished "rm -rf repo" &&
		git init repo &&
		(
			cd repo &&
			git config merge.conflictStyle "$style" &&
			build_resolution_merge &&
			git replay --ref-action=print --branches \
				--onto E --ancestry-path B..M >out &&
			test_line_count = 1 out &&
			new_tip=$(cut -d " " -f 3 out) &&
			test_cmp_rev "$new_tip:a" M:a &&
			test_cmp_rev "$new_tip:b" E:b
		)
	'
done

build_diverging_conflicts () {
	test-tool historian <<-\EOF
	# M resolves line 3, but the new parent also conflicts on line 5.
	blob a L1 L2 L3 L4 L5
	blob c L1 L2 _3 L4 L5
	blob d _1 L2 +3 L4 _5
	blob m _1 L2 M3 L4 _5
	blob e L1 L2 L3 L4 +5
	commit A main A a:a
	commit B main B parent=A a:a
	commit C main C parent=B a:c
	commit D feature D parent=A a:d
	commit M main M parent=C parent=D a:m
	commit E target E parent=A a:e
	EOF
}

test_expect_success 'newly introduced conflict in a replayed merge' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_diverging_conflicts &&
		test_must_fail git replay --ref-action=print --branches \
			--onto E --ancestry-path B..M >out 2>err &&
		test_grep "CONFLICT (content)" out &&
		test_grep "Merge conflict in a" out
	)
'

build_new_conflict_binary () {
	test-tool historian <<-\EOF
	blob attrs "* -merge"
	blob bo original
	blob bd dee
	blob be ehh
	blob mo original
	blob mc changed
	commit A main A .gitattributes:attrs bin:bo misc:mo
	commit B main B parent=A .gitattributes:attrs bin:bo misc:mo
	commit C main C parent=B .gitattributes:attrs bin:bo misc:mc
	commit D feature D parent=A .gitattributes:attrs bin:bd misc:mo
	commit M main M parent=C parent=D .gitattributes:attrs bin:bd misc:mc
	commit E target E parent=A .gitattributes:attrs bin:be misc:mo
	EOF
}

test_expect_success 'newly introduced binary conflict is reported' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_new_conflict_binary &&
		test_must_fail git replay --ref-action=print --branches \
			--onto E --ancestry-path B..M >out 2>err &&
		test_grep "CONFLICT (content)" out &&
		test_grep "Merge conflict in bin" out
	)
'

build_inner_conflicts () {
	test-tool historian <<-\EOF
	blob a0 base
	blob a1 side
	blob a2 onto
	commit A main A dir/a:a0
	commit C main C parent=A dir/a:a0 misc:a0
	commit D side D parent=A dir/a:a1
	commit M main M parent=C parent=D dir/a:a1 misc:a0
	commit R renamed R parent=C parent=D renamed/a:a1 misc:a0
	commit E target E parent=A dir/a:a2
	EOF
}

for branch in main renamed
do
	case $branch in
	main) path=dir/a ;;
	renamed) path=renamed/a ;;
	esac
	test_expect_success "new inner conflicts are reported in $path" '
		test_when_finished "rm -rf repo" &&
		git init repo &&
		(
			cd repo &&
			build_inner_conflicts &&
			test_must_fail git replay --ref-action=print --onto E \
				--ancestry-path "C..$branch" >out 2>err &&
			test_grep "CONFLICT (content)" out &&
			test_grep "Merge conflict in $path" out
		)
	'
done

test_expect_success 'changed original conflict inputs prevent replay' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		test-tool historian <<-\EOF &&
		blob a0 base
		blob al left
		blob ar right
		blob am manual
		blob af updated
		commit A main A a:a0
		commit B main B parent=A a:al
		commit C side C parent=A a:ar
		commit M main M parent=B parent=C a:am
		commit F target F parent=B a:af
		EOF
		test_must_fail git replay --ref-action=print --onto F \
			--ancestry-path B..main >out 2>err &&
		test_grep "CONFLICT (content)" out &&
		test_grep "Merge conflict in a" out
	)
'

test_expect_success 'new modify/delete conflicts prevent replay' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		test-tool historian <<-\EOF &&
		# Both original merge trees omit f; the new parent changes it.
		blob f0 v0
		blob f1 v1
		blob g g
		blob h h
		commit B0 main B0 f:f0 g:g
		commit A main A parent=B0 f:f0 g:g
		commit P1 main P1 parent=A f:f0 g:g h:h
		commit P2 side P2 parent=B0 g:g
		commit M main M parent=P1 parent=P2 g:g h:h
		commit X target X parent=A f:f1 g:g
		EOF
		test_must_fail git replay --ref-action=print --onto X \
			--ancestry-path A..main >out 2>err &&
		test_grep CONFLICT out &&
		test_grep "Merge conflict in f" out
	)
'

test_expect_success 'a new copy of an old conflict prevents replay' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		test-tool historian <<-\EOF &&
		# The rewrite adds a second byte-identical conflict block.
		blob a0 x foo=1 s1 s2 s3 s4 s5 foo=1 w
		blob c x foo=2 s1 s2 s3 s4 s5 foo=1 w
		blob d x foo=3 s1 s2 s3 s4 s5 foo=3 w
		blob m x foo=4 s1 s2 s3 s4 s5 foo=3 w
		blob e x foo=1 s1 s2 s3 s4 s5 foo=2 w
		commit A main A a:a0
		commit B main B parent=A a:a0
		commit C main C parent=B a:c
		commit D feature D parent=A a:d
		commit M main M parent=C parent=D a:m
		commit E target E parent=A a:e
		EOF
		test_must_fail git replay --ref-action=print --onto E \
			--ancestry-path B..main >out 2>err &&
		test_grep "CONFLICT (content)" out &&
		test_grep "Merge conflict in a" out
	)
'

test_expect_success 'an obsolete resolution does not prevent replay' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		test-tool historian <<-\EOF &&
		# X includes the recorded resolution and adds another line.
		blob a L1 L2 L3 L4
		blob b L1 L2 _3 L4
		blob c L1 L2 +3 L4
		blob m _1 L2 _3 L4
		blob d L1 L2 +3 L4 L5
		blob x _1 L2 _3 L4 L5
		commit A main A a:a
		commit B side B parent=A a:b
		commit C main C parent=A a:c
		commit M main M parent=B parent=C a:m
		commit D target D parent=C a:d
		commit X target X parent=D a:x
		EOF
		git replay --ref-action=print --onto X \
			--ancestry-path C..main >out &&
		test_line_count = 1 out &&
		new_tip=$(cut -d " " -f 3 out) &&
		test_cmp_rev "$new_tip^1" B &&
		test_cmp_rev "$new_tip^2" X &&
		test_cmp_rev "$new_tip:a" X:a
	)
'

test_expect_success 'literal conflict marker text is retained' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		test-tool historian <<-\EOF &&
		blob a L1 L2 L3
		blob c L1 L2 _3
		blob d _1 L2 +3
		blob m _1 L2 M3
		blob markers "<<<<<<< ours" x "=======" y ">>>>>>> theirs"
		commit A main A a:a
		commit B main B parent=A a:a
		commit C main C parent=B a:c
		commit D feature D parent=A a:d
		commit M main M parent=C parent=D a:m
		commit E target E parent=A a:a doc/markers:markers
		EOF
		git replay --ref-action=print --onto E \
			--ancestry-path B..main >out &&
		test_line_count = 1 out &&
		new_tip=$(cut -d " " -f 3 out) &&
		test_cmp_rev "$new_tip:doc/markers" E:doc/markers
	)
'

build_criss_cross () {
	test-tool historian <<-\EOF
	blob a0 base
	blob al left
	blob ar right
	blob am manual
	blob b0 1 base 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17
	blob bl 1 left 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17
	blob br 1 right 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17
	blob bd 1 resolved 3 4 5 6 7 8 first 10 11 12 13 14 15 16 17
	blob be 1 resolved 3 4 5 6 7 8 9 10 11 12 13 14 15 16 second
	blob bm 1 resolved 3 4 5 6 7 8 first 10 11 12 13 14 15 16 second
	commit A main A a:a0 b:b0
	commit B left B parent=A a:a0 b:bl
	commit C right C parent=A a:a0 b:br
	commit D main D parent=B parent=C a:al b:bd
	commit E side E parent=C parent=B a:ar b:be
	commit M main M parent=D parent=E a:am b:bm
	commit P rebased P parent=D a:al b:be
	commit Q simple Q parent=B a:al b:bd
	EOF
}

test_expect_success 'virtual-base conflicts do not become replay conflicts' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_criss_cross &&
		git replay --ref-action=print --onto P \
			--ancestry-path D..main >out &&
		test_line_count = 1 out &&
		new_tip=$(cut -d " " -f 3 out) &&
		test_cmp_rev "$new_tip:a" M:a &&
		test_cmp_rev "$new_tip:b" E:b
	)
'

test_expect_success 'diff3 resolution survives a changed merge-base count' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		build_criss_cross &&
		git -c merge.conflictStyle=diff3 replay --ref-action=print \
			--onto Q --ancestry-path D..main >out &&
		test_line_count = 1 out &&
		new_tip=$(cut -d " " -f 3 out) &&
		test_cmp_rev "$new_tip^{tree}" M^{tree}
	)
'

reword_to () {
	GIT_EDITOR="echo \"$1\" >" git history reword "$2"
}

test_expect_failure 'clean merge keeps its topology and content' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		test-tool historian <<-\EOF &&
		blob a shared
		blob g guarded
		blob h host
		commit A main A a:a
		commit B topic B parent=A a:a g:g
		commit C main C parent=A a:a h:h
		commit M main "Merge topic" parent=C parent=B a:a g:g h:h
		EOF
		reword_to AA A &&
		test_cmp_rev ! HEAD M &&
		test_cmp_rev HEAD^{tree} M^{tree} &&
		test_cmp_rev HEAD^1^{tree} M^1^{tree} &&
		test_cmp_rev HEAD^2^{tree} M^2^{tree} &&
		echo "Merge topic" >expect &&
		git log -1 --format=%s HEAD >actual &&
		test_cmp expect actual
	)
'

test_expect_failure 'manual textual merge resolution is preserved' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		test-tool historian <<-\EOF &&
		blob a0 line1 line2 line3
		blob al line1 line2-main line3
		blob ar line1 line2-topic line3
		blob am line1 line2-merged-by-hand line3
		commit A main A a:a0
		commit B topic B parent=A a:ar
		commit C main C parent=A a:al
		commit M main M parent=C parent=B a:am
		EOF
		reword_to AA A &&
		test_cmp_rev ! HEAD M &&
		test_cmp_rev HEAD:a M:a
	)
'

test_expect_failure 'merge-only semantic edit is preserved' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		test-tool historian <<-\EOF &&
		blob a0 line1 line2 line3 line4 line5
		blob al line1 line2-main line3 line4 line5
		blob ar line1 line2-topic line3 line4 line5
		blob am line1 line2-merged line3 line4 line5-touched
		commit A main A a:a0
		commit B topic B parent=A a:ar
		commit C main C parent=A a:al
		commit M main M parent=C parent=B a:am
		EOF
		reword_to AA A &&
		test_cmp_rev ! HEAD M &&
		test_cmp_rev HEAD:a M:a
	)
'

test_expect_failure 'octopus merge in the rewrite path is rejected' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		test-tool historian <<-\EOF &&
		blob a x
		commit A main A a:a
		commit B b1 B parent=A a:a
		commit C b2 C parent=A a:a
		commit D b3 D parent=A a:a
		commit O main O parent=A parent=B parent=C parent=D a:a
		EOF
		test_must_fail git -c core.editor=true history reword \
			--dry-run A 2>err &&
		test_grep octopus err
	)
'

test_expect_failure 'out-of-range first merge parent is preserved' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	(
		cd repo &&
		test-tool historian <<-\EOF &&
		# O stays outside the rewrite range; R is rewritten.
		blob x X
		blob o O
		blob a A
		blob r R
		blob t T
		blob m M
		blob top marker
		commit X main X a:x
		commit O side O parent=X a:o
		commit A main A parent=X a:a
		commit R main R parent=A a:r
		commit T off T parent=R a:t s:top
		commit M main M parent=O parent=R a:m s:top
		EOF
		reword_to AA A &&
		test_cmp_rev ! main M &&
		test_cmp_rev main^ O &&
		test_cmp_rev ! main^2 R &&
		test_cmp_rev main^2^{tree} R^{tree} &&
		test_cmp_rev main^{tree} M^{tree} &&
		test_cmp_rev side O &&
		test_cmp_rev ! off T
	)
'

test_done
