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

test_done
