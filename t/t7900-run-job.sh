#!/bin/sh

test_description='git run-job

Testing the background jobs, in the foreground
'

GIT_TEST_COMMIT_GRAPH=0
GIT_TEST_MULTI_PACK_INDEX=0

. ./test-lib.sh

test_expect_success 'help text' '
	test_must_fail git run-job -h 2>err &&
	test_i18ngrep "usage: git run-job" err
'

test_expect_success 'commit-graph job' '
	git init server &&
	(
		cd server &&
		chain=.git/objects/info/commit-graphs/commit-graph-chain &&
		git checkout -b master &&
		for i in $(test_seq 1 15)
		do
			test_commit $i || return 1
		done &&
		git run-job commit-graph 2>../err &&
		test_must_be_empty ../err &&
		test_line_count = 1 $chain &&
		for i in $(test_seq 16 19)
		do
			test_commit $i || return 1
		done &&
		git run-job commit-graph 2>../err &&
		test_must_be_empty ../err &&
		test_line_count = 2 $chain &&
		for i in $(test_seq 20 23)
		do
			test_commit $i || return 1
		done &&
		git run-job commit-graph 2>../err &&
		test_must_be_empty ../err &&
		test_line_count = 1 $chain
	)
'

test_expect_success 'fetch job' '
	git clone "file://$(pwd)/server" client &&

	# Before fetching, build a client commit-graph
	git -C client run-job commit-graph &&
	chain=client/.git/objects/info/commit-graphs/commit-graph-chain &&
	test_line_count = 1 $chain &&

	git -C client branch -v --remotes >before-refs &&
	test_commit -C server 24 &&

	git -C client run-job fetch &&
	git -C client branch -v --remotes >after-refs &&
	test_cmp before-refs after-refs &&
	test_cmp server/.git/refs/heads/master \
		 client/.git/refs/hidden/origin/master &&

	# the hidden ref should trigger a new layer in the commit-graph
	git -C client run-job commit-graph &&
	test_line_count = 2 $chain
'

test_expect_success 'loose-objects job' '
	ls client/.git/objects >obj-dir-before &&
	test_file_not_empty obj-dir-before &&
	ls client/.git/objects/pack/*.pack >packs-before &&
	test_line_count = 1 packs-before &&

	# The first run creates a pack-file
	# but does not delete loose objects.
	git -C client run-job loose-objects &&
	ls client/.git/objects >obj-dir-between &&
	test_cmp obj-dir-before obj-dir-between &&
	ls client/.git/objects/pack/*.pack >packs-between &&
	test_line_count = 2 packs-between &&

	# The second run deletes loose objects
	# but does not create a pack-file.
	git -C client run-job loose-objects &&
	ls client/.git/objects >obj-dir-after &&
	cat >expect <<-\EOF &&
	info
	pack
	EOF
	test_cmp expect obj-dir-after &&
	ls client/.git/objects/pack/*.pack >packs-after &&
	test_cmp packs-between packs-after
'

test_expect_success 'pack-files job' '
	packDir=.git/objects/pack &&

	# Create three disjoint pack-files with size BIG, small, small.

	echo HEAD~2 | git -C client pack-objects --revs $packDir/test-1 &&

	test_tick &&
	git -C client pack-objects --revs $packDir/test-2 <<-\EOF &&
	HEAD~1
	^HEAD~2
	EOF

	test_tick &&
	git -C client pack-objects --revs $packDir/test-3 <<-\EOF &&
	HEAD
	^HEAD~1
	EOF

	rm -f client/$packDir/pack-* &&
	rm -f client/$packDir/loose-* &&

	ls client/$packDir/*.pack >packs-before &&
	test_line_count = 3 packs-before &&

	# the job repacks the two into a new pack, but does not
	# delete the old ones.
	git -C client run-job pack-files &&
	ls client/$packDir/*.pack >packs-between &&
	test_line_count = 4 packs-between &&

	# the job deletes the two old packs, and does not write
	# a new one because only one pack remains.
	git -C client run-job pack-files &&
	ls client/$packDir/*.pack >packs-after &&
	test_line_count = 2 packs-after &&
	cat packs-after | grep "pack/test-1-" &&
	cat packs-after | grep "pack/pack-"
'

test_done
