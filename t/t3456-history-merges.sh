#!/bin/sh

test_description='git replay and git history across merge commits'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_failure 'preserves resolutions; does not extrapolate' '
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
	test_expect_failure "previously resolved conflicts with $style" '
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

test_expect_failure 'newly introduced conflict in a replayed merge' '
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

test_expect_failure 'newly introduced binary conflict is reported' '
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
	test_expect_failure "new inner conflicts are reported in $path" '
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

test_expect_failure 'changed original conflict inputs prevent replay' '
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

test_expect_failure 'new modify/delete conflicts prevent replay' '
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

test_done
