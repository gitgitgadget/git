#!/bin/sh

test_description='sparse-index and sparse-checkout are incompatible'

. ./test-lib.sh

r="repo-1"
test_expect_success 'repo-1 : start with split-index and then enable sparse-index' '
	git init $r &&
	(
		cd $r &&

		git config core.splitIndex true &&
		git config splitIndex.maxPercentChange 0 &&

		mkdir initial &&
		test_commit initial/initial &&

		mkdir one &&
		test_commit one/one &&

		mkdir two &&
		test_commit two/two &&

		mkdir three &&
		test_commit three/three &&

		# Try to turn on sparse-checkout and -index. Technically, this
		# should fail because we already have split-index turned on.
		#
		# TODO Set this to test_must_fail ...
		git sparse-checkout init --cone --sparse-index &&

		#################################################################
		# Everything below here is flakey because of the bad state that
		# we have created. The following errors have been observed, but
		# others may be possible.
		#################################################################

		# After creating an incompatible state, the first status USUALLY throws a
		# BUG() assert.
		#
		# BUG: cache-tree.c:908: directory 'one/' is present in index, but not sparse
		test_expect_code 134 git status >../$r.out 2>../$r.err &&
		grep -e "BUG: .* is present in index, but not sparse" ../$r.err &&

		# Debug dump enough state to show that both think they are turned on.  Oddly
		# enough something causes the cache-tree error to go away.
		git config --list --show-origin &&
		git ls-files --debug &&
		git ls-files --sparse &&
		git ls-files -t &&
		git status &&
		ls -la &&

		rm -f .git/index.lock &&

		# Try to populate some cones in the worktree.  This fails in a weird way.
		test_expect_code 128 git sparse-checkout set two three >../$r.out 2>../$r.err &&
		grep "error: unable to create file three/: File exists" ../$r.err &&
		grep "error: unable to create file two/: File exists" ../$r.err &&
		grep "fatal: index cache-tree records empty sub-tree" ../$r.err &&

		# Debug dump enough state to show what changed.
		git config --list --show-origin &&
		git ls-files --debug &&
		git ls-files --sparse &&
		git ls-files -t &&
		ls -la
	)
'

r="repo-2"
test_expect_success FSMONITOR_DAEMON 'repo-2 : start with split-index and fsmonitor and then enable sparse-index' '
	git init $r &&
	test_when_finished "git -C \"$PWD/$r\" \
		fsmonitor--daemon stop" &&
	(
		cd $r &&

		git config core.splitIndex true &&
		git config splitIndex.maxPercentChange 0 &&

		git config core.fsmonitor true &&

		mkdir initial &&
		test_commit initial/initial &&

		mkdir one &&
		test_commit one/one &&

		mkdir two &&
		test_commit two/two &&

		mkdir three &&
		test_commit three/three &&

		# Try to turn on sparse-checkout and -index. Technically, this
		# should fail because we already have split-index turned on.
		#
		# TODO Set this to test_must_fail ...
		git sparse-checkout init --cone --sparse-index &&

		#################################################################
		# Everything below here is flakey because of the bad state that
		# we have created. The following errors have been observed, but
		# others may be possible.
		#################################################################

		# After creating an incompatible state, the first status USUALLY throws a
		# BUG() assert.
		#
		# BUG: cache-tree.c:908: directory 'one/' is present in index, but not sparse
		test_expect_code 134 git status >../$r.out 2>../$r.err &&
		grep -e "BUG: .* is present in index, but not sparse" ../$r.err &&

		# Debug dump enough state to show that both think they are turned on.  Oddly
		# enough something causes the cache-tree error to go away.
		git config --list --show-origin &&
		git ls-files --debug &&
		git ls-files --sparse &&
		git ls-files -t &&
		git status &&
		ls -la &&

		rm -f .git/index.lock &&

		# Try to populate some cones in the worktree.  This fails in a weird way.
		test_expect_code 128 git sparse-checkout set two three >../$r.out 2>../$r.err &&
		grep "error: unable to create file three/: File exists" ../$r.err &&
		grep "error: unable to create file two/: File exists" ../$r.err &&
		grep "fatal: index cache-tree records empty sub-tree" ../$r.err &&

		# Debug dump enough state to show what changed.
		git config --list --show-origin &&
		git ls-files --debug &&
		git ls-files --sparse &&
		git ls-files -t &&
		ls -la
	)
'

r="repo-3"
test_expect_success 'repo-3 : start with split-index and then enable sparse-index' '
	git init $r &&
	(
		cd $r &&

		git config core.splitIndex true &&
		git config splitIndex.maxPercentChange 0 &&

		mkdir initial &&
		test_commit initial/initial &&

		mkdir one &&
		test_commit one/one &&

		mkdir two &&
		test_commit two/two &&

		mkdir three &&
		test_commit three/three &&

		# Try to turn on sparse-checkout but NOT sparse-index. Technically, this
		# should fail because we already have split-index turned on.
		#
		# TODO Set this to test_must_fail ...
		git sparse-checkout init --cone --no-sparse-index &&

		#################################################################
		# Everything below here is flakey because of the bad state that
		# we have created. The following errors have been observed, but
		# others may be possible.
		#################################################################

		# Debug dump enough state to show what is turned on.
		git config --list --show-origin &&
		git ls-files --debug &&
		git ls-files --sparse &&
		git ls-files -t &&
		git status &&
		ls -la &&

		rm -f .git/index.lock &&

		# Try to populate some cones in the worktree.
		#
		# This creates the expected files in both directories.
		git sparse-checkout set two three &&

		# Debug dump enough state to show what changed.
		git config --list --show-origin &&
		git ls-files --debug &&
		git ls-files --sparse &&
		git ls-files -t &&
		ls -la &&

		# Now if we ad hoc add sparse-index things get weird.  The first status
		# succeeds, but corrupts the index which causes the second to fail hard.
		git -c index.split=true status &&
		git -c index.split=true status >../$r.out 2>../$r.err

#		test_expect_code 128 git -c index.split=true status >../$r.out 2>../$r.err &&
#		grep "fatal: index cache-tree records empty sub-tree" ../$r.err
	)
'

r="repo-4"
test_expect_success FSMONITOR_DAEMON 'repo-4 : start with split-index and fsmonitor and then enable sparse-index' '
	git init $r &&
	test_when_finished "git -C \"$PWD/$r\" \
		fsmonitor--daemon stop" &&
	(
		cd $r &&

		git config core.splitIndex true &&
		git config splitIndex.maxPercentChange 0 &&

		git config core.fsmonitor true &&

		mkdir initial &&
		test_commit initial/initial &&

		echo x >initial/x1 &&
		echo x >initial/x2 &&
		echo x >initial/x3 &&
		echo x >initial/x4 &&
		echo x >initial/x5 &&
		echo x >initial/x6 &&
		echo x >initial/x7 &&
		echo x >initial/x8 &&
		echo x >initial/x9 &&
		echo x >initial/y1 &&
		echo x >initial/y2 &&
		echo x >initial/y3 &&
		echo x >initial/y4 &&
		echo x >initial/y5 &&
		echo x >initial/y6 &&
		echo x >initial/y7 &&
		echo x >initial/y8 &&
		echo x >initial/y9 &&
		git add . &&
		git commit -m "ballast" &&

		mkdir one &&
		test_commit one/one &&

		mkdir two &&
		test_commit two/two &&

		mkdir three &&
		test_commit three/three &&

		git rev-parse --shared-index-path &&
		# TODO verify line count of 1 on the above

		# Try to turn on sparse-checkout but DO NOT pass --sparse-index
		#
		# TODO Set this to test_must_fail ...
		git sparse-checkout init --cone &&

		#################################################################
		# Everything below here is flakey because of the bad state that
		# we have created. The following errors have been observed, but
		# others may be possible.
		#################################################################

		# The following may succeed on upstream Git, but fails on microsoft/git
		# since the latter assumes index.sparse=true
		#
		test_expect_code 128 git -c index.sparse=true rev-parse --shared-index-path >../$r.out 2>../$r.err &&
		grep -e "fatal: position for replacement .* exceeds base index size .*" ../$r.err
	)
'

r="repo-5"
test_expect_success FSMONITOR_DAEMON 'repo-5 : start with split-index and fsmonitor and then enable sparse-index' '
	git init $r &&
	test_when_finished "git -C \"$PWD/$r\" \
		fsmonitor--daemon stop" &&
	(
		cd $r &&

		git config core.splitIndex true &&
		git config splitIndex.maxPercentChange 0 &&

		git config core.fsmonitor true &&

		mkdir initial &&
		test_commit initial/initial &&

		echo x >initial/x1 &&
		echo x >initial/x2 &&
		echo x >initial/x3 &&
		echo x >initial/x4 &&
		echo x >initial/x5 &&
		echo x >initial/x6 &&
		echo x >initial/x7 &&
		echo x >initial/x8 &&
		echo x >initial/x9 &&
		echo x >initial/y1 &&
		echo x >initial/y2 &&
		echo x >initial/y3 &&
		echo x >initial/y4 &&
		echo x >initial/y5 &&
		echo x >initial/y6 &&
		echo x >initial/y7 &&
		echo x >initial/y8 &&
		echo x >initial/y9 &&
		git add . &&
		git commit -m "ballast" &&

		mkdir one &&
		test_commit one/one &&

		mkdir two &&
		test_commit two/two &&

		mkdir three &&
		test_commit three/three &&

		git rev-parse --shared-index-path &&
		# TODO verify line count of 1 on the above

		# Try to turn on sparse-checkout.
		#
		# TODO Set this to test_must_fail ...
		git sparse-checkout init --cone &&

		#################################################################
		# Everything below here is flakey because of the bad state that
		# we have created. The following errors have been observed, but
		# others may be possible.
		#################################################################

		git config --list --show-origin &&

		# Sometimes the exit status is 128 and sometimes it is 134.
		(
			git ls-files --sparse >../$r.out 2>../$r.err ||
			grep -e "fatal: position for replacement .* exceeds base index size .*" ../$r.err ||
			grep -e "BUG: .* fsmonitor_dirty has more entries than the index .*" ../$r.err
		) &&

		cat ../$r.err
	)
'

r="repo-6"
test_expect_success FSMONITOR_DAEMON 'repo-6 : start with sparse-index and fsmonitor and then enable split-index' '
	git init $r &&
	test_when_finished "git -C \"$PWD/$r\" \
		fsmonitor--daemon stop" &&
	(
		cd $r &&

		git config core.fsmonitor true &&

		mkdir initial &&
		test_commit initial/initial &&

		echo x >initial/x1 &&
		echo x >initial/x2 &&
		echo x >initial/x3 &&
		echo x >initial/x4 &&
		echo x >initial/x5 &&
		echo x >initial/x6 &&
		echo x >initial/x7 &&
		echo x >initial/x8 &&
		echo x >initial/x9 &&
		echo x >initial/y1 &&
		echo x >initial/y2 &&
		echo x >initial/y3 &&
		echo x >initial/y4 &&
		echo x >initial/y5 &&
		echo x >initial/y6 &&
		echo x >initial/y7 &&
		echo x >initial/y8 &&
		echo x >initial/y9 &&
		git add . &&
		git commit -m "ballast" &&

		mkdir one &&
		test_commit one/one &&

		mkdir two &&
		test_commit two/two &&

		mkdir three &&
		test_commit three/three &&

		git sparse-checkout init --cone --sparse-index &&
		git sparse-checkout set one two &&

		git config --list --show-origin &&


		git config core.splitIndex true &&
		git config splitIndex.maxPercentChange 0 &&

		# Sparse then split correctly fails.

		test_expect_code 128 git update-index --split-index >../$r.out 2>../$r.err &&
		grep "fatal: cannot use split index with a sparse index" ../$r.err
	)
'

test_done
