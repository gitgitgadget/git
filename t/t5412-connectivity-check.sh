#!/bin/sh

test_description='connectivity check (transfer.connectivityCheck)'
GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_oid_cache <<-\EOF
missing sha1:0000000000000000000000000000000000000001
missing sha256:0000000000000000000000000000000000000000000000000000000000000001
EOF

set_connectivity_check () {
	if test $# -eq 2
	then
		git -C "$1" config transfer.connectivityCheck "$2"
	else
		git config transfer.connectivityCheck "$1"
	fi
}

test_trace2_count_for_incremental () {
	if test "$mode" = incremental
	then
		test_trace2_data_singular connectivity "$@"
	fi
}

# Create a commit with one file changed, without modifying HEAD,
# index, or worktree.  Prints the new commit OID on stdout.
# Usage: commit_with_change <parent> <path> <content>
commit_with_change () {
	new_blob=$(echo "$3" | git hash-object -w --stdin) &&
	TMP_IDX=.git/tmp-idx &&
	GIT_INDEX_FILE=$TMP_IDX git read-tree "$1" &&
	GIT_INDEX_FILE=$TMP_IDX git update-index --replace \
		--cacheinfo "100644,$new_blob,$2" &&
	new_tree=$(GIT_INDEX_FILE=$TMP_IDX git write-tree) &&
	rm -f "$TMP_IDX" &&
	git commit-tree "$new_tree" -p "$1" -m "modify $2"
}

# Run a test inside a directory with connectivity check mode set.
# Usage: test_expect_success_in <dir> "title" 'body'
test_expect_success_in () {
	dir=$1 && shift &&
	case $# in
	2)
		test_expect_success "$1" \
			"( cd $dir && set_connectivity_check \$mode && $2 )"
		;;
	3)
		test_expect_success "$1" "$2" \
			"( cd $dir && set_connectivity_check \$mode && $3 )"
		;;
	*)
		BUG "test_expect_success_in requires 3 or 4 arguments"
		;;
	esac
}

# Check one or more OIDs with test-tool, optionally verifying trace2 counts.
# Usage: check_connected_trace <trace-file> <trees> <blobs> <oid>...
# An empty string for <trees> or <blobs> skips that assertion.
check_connected_trace () {
	trace_file=$1 trees=$2 blobs=$3 &&
	shift 3 &&
	GIT_TRACE2_EVENT="$(pwd)/$trace_file" \
		test-tool check-connected "$@" &&
	if test -n "$trees"
	then
		test_trace2_count_for_incremental trees_walked "$trees" \
			<"$trace_file"
	fi &&
	if test -n "$blobs"
	then
		test_trace2_count_for_incremental blobs_checked "$blobs" \
			<"$trace_file"
	fi
}

# Shared setup: a repo with several root-level files and nested dirs.
# The unchanged/ subtree (10 dirs x 10 files = 100 blobs, 11 trees)
# acts as a canary: any test asserting small tree/blob counts would
# fail dramatically if incremental accidentally walked into it.
#
# Graph:
#   initial  -- root-level files (file-{1..5}.txt)
#   nested   -- adds a/b/c/deep.txt and a/other.txt
#   canary   -- adds unchanged/{dir-1..10}/{file-1..10}.txt

test_expect_success 'setup main repo' '
	git init main-repo &&
	(
		cd main-repo &&
		for i in $(test_seq 1 5)
		do
			echo "file $i" >"file-$i.txt" || return 1
		done &&
		git add file-*.txt &&
		git commit -m "initial" &&

		mkdir -p a/b/c &&
		echo deep >a/b/c/deep.txt &&
		echo other >a/other.txt &&
		git add a/b/c/deep.txt a/other.txt &&
		git commit -m "add nested dirs" &&

		for i in $(test_seq 1 10)
		do
			mkdir -p "unchanged/dir-$i" &&
			for j in $(test_seq 1 10)
			do
				echo "unchanged $i $j" \
					>"unchanged/dir-$i/file-$j.txt" ||
					return 1
			done
		done &&
		git add unchanged/ &&
		git commit -m "add unchanged canary subtree" &&

		test_oid missing >.git/fake-oid
	)
'

test_expect_success 'setup replacement object repo' '
	git init replace-test &&
	(
		cd replace-test &&

		test_commit --no-tag original file.txt &&
		original=$(git rev-parse HEAD) &&
		orig_blob=$(git rev-parse HEAD:file.txt) &&

		# Orphan replacement commit with a different tree
		replacement_tree=$(echo replaced | git hash-object -w --stdin |
			xargs -I{} git mktree <<-EOF
			100644 blob {}	file.txt
			EOF
		) &&
		replacement=$(git commit-tree -m "replacement" \
			"$replacement_tree") &&

		git replace "$original" "$replacement" &&

		# Remove the original blob so only the replacement
		# tree is complete.
		rm .git/objects/$(test_oid_to_path "$orig_blob") &&

		# Drop branch and HEAD so --not --all does not
		# exclude the original commit.
		git update-ref -d refs/heads/main &&
		git update-ref -d HEAD &&

		echo "$original" >.git/test-oid
	)
'

for mode in rev-list incremental
do

# Corruption detection: craft broken object graphs and verify detection.
# All tests use main-repo without modifying its refs or worktree.

test_expect_success_in main-repo "$mode: rejects commit with missing blob" '
	fake_oid=$(cat .git/fake-oid) &&
	bad_tree=$(printf "100644 blob ${fake_oid}\tfile.txt\n" |
		git mktree --missing) &&
	bad_commit=$(git commit-tree "$bad_tree" -p HEAD -m "bad") &&

	test_must_fail test-tool check-connected "$bad_commit" 2>err &&
	test_grep "missing blob object" err
'

test_expect_success_in main-repo "$mode: rejects commit with missing subtree" '
	fake_oid=$(cat .git/fake-oid) &&
	bad_tree=$(printf "40000 tree ${fake_oid}\tdir\n" |
		git mktree --missing) &&
	bad_commit=$(git commit-tree "$bad_tree" -p HEAD -m "bad") &&

	test_must_fail test-tool check-connected "$bad_commit" 2>err &&
	test_grep "bad tree object" err
'

test_expect_success_in main-repo "$mode: rejects missing blob under annotated tag" '
	fake_oid=$(cat .git/fake-oid) &&
	bad_tree=$(printf "100644 blob ${fake_oid}\tfile.txt\n" |
		git mktree --missing) &&
	bad_commit=$(git commit-tree "$bad_tree" -p HEAD -m "bad") &&
	git tag -a -m "annotated" bad-tag "$bad_commit" &&
	tag_oid=$(git rev-parse bad-tag) &&
	git tag -d bad-tag &&

	test_must_fail test-tool check-connected "$tag_oid" 2>err &&
	test_grep "missing blob object" err
'

test_expect_success_in main-repo "$mode: verifies direct tree tip" '
	fake_oid=$(cat .git/fake-oid) &&
	bad_tree=$(printf "100644 blob ${fake_oid}\tfile.txt\n" |
		git mktree --missing) &&

	test_must_fail test-tool check-connected "$bad_tree" 2>err &&
	test_grep "missing blob object" err
'

test_expect_success_in main-repo "$mode: verifies direct blob tip" '
	blob_oid=$(echo "hello" | git hash-object -w --stdin) &&
	test-tool check-connected "$blob_oid"
'

test_expect_success_in main-repo "$mode: rejects missing direct blob tip" '
	test_must_fail test-tool check-connected \
		$(cat .git/fake-oid) 2>err
'

test_expect_success_in main-repo "$mode: accepts tag pointing to existing blob" '
	blob_oid=$(echo "content" | git hash-object -w --stdin) &&
	git tag -a -m "tag a blob" blob-tag "$blob_oid" &&
	tag_oid=$(git rev-parse blob-tag) &&
	git tag -d blob-tag &&

	test-tool check-connected "$tag_oid"
'

test_expect_success_in main-repo PERL_TEST_HELPERS \
	"$mode: rejects blob OID reused as tree entry" '
	blob_oid=$(git rev-parse HEAD:file-1.txt) &&
	bin_oid=$(echo "$blob_oid" | hex2oct) &&

	bad_tree=$(printf "40000 subdir\0$bin_oid" |
		git hash-object -t tree -w --stdin) &&
	bad_commit=$(git commit-tree -p HEAD -m "child" "$bad_tree") &&

	test_must_fail test-tool check-connected "$bad_commit" 2>err &&
	test_grep "not a tree" err
'

test_expect_success_in main-repo PERL_TEST_HELPERS \
	"$mode: rejects tree OID reused as blob entry" '
	tree_oid=$(git rev-parse HEAD:a) &&
	bin_oid=$(echo "$tree_oid" | hex2oct) &&

	bad_tree=$(printf "100644 fakefile\0$bin_oid" |
		git hash-object -t tree -w --stdin) &&
	bad_commit=$(git commit-tree -p HEAD -m "child" "$bad_tree") &&

	test_must_fail test-tool check-connected "$bad_commit" 2>err &&
	test_grep "not a blob" err
'

test_expect_success_in main-repo "$mode: peels nested tag chain" '
	# Create a chain: outer -> inner -> commit
	git tag -a -m "inner tag" inner HEAD &&
	inner_oid=$(git rev-parse inner) &&
	git tag -a -m "outer tag" outer inner &&
	outer_oid=$(git rev-parse outer) &&
	git tag -d outer &&
	git tag -d inner &&

	test-tool check-connected "$outer_oid"
'

test_expect_success_in main-repo "$mode: rejects missing intermediate tag in chain" '
	git tag -a -m "inner tag" inner HEAD &&
	inner_oid=$(git rev-parse inner) &&
	git tag -a -m "outer tag" outer inner &&
	outer_oid=$(git rev-parse outer) &&
	git tag -d outer &&
	git tag -d inner &&

	# Remove the inner tag object
	rm .git/objects/$(test_oid_to_path "$inner_oid") &&

	test_must_fail test-tool check-connected "$outer_oid"
'

test_expect_success_in main-repo "$mode: checks multiple tips" '
	c1=$(commit_with_change HEAD file-1.txt "tip-a") &&
	c2=$(commit_with_change HEAD file-2.txt "tip-b") &&
	git tag -a -m "tagged" multi-tag "$c1" &&
	tag_oid=$(git rev-parse multi-tag) &&
	git tag -d multi-tag &&

	test-tool check-connected "$c2" "$tag_oid"
'

# Tree-diff optimization: verify trace2 counts in incremental mode.
# These tests create commits without modifying refs and check them
# directly with test-tool check-connected.

test_expect_success_in main-repo "$mode: skips unchanged subtrees (single file change)" '
	oid=$(commit_with_change HEAD file-1.txt "changed") &&

	# Only the root tree is walked; a/ subtree is unchanged.
	# 1 changed blob verified, rest pre-trusted from parent.
	check_connected_trace trace-flat.txt 1 1 "$oid"
'

test_expect_success_in main-repo "$mode: visits depth-proportional trees (nested change)" '
	oid=$(commit_with_change HEAD a/b/c/deep.txt "deep-changed") &&

	# root + a + b + c = 4 trees walked, 1 changed blob.
	check_connected_trace trace-nested.txt 4 1 "$oid"
'

test_expect_success_in main-repo "$mode: verifies annotated tag target" '
	oid=$(commit_with_change HEAD file-1.txt "tag-verify") &&
	git tag -a -m "annotated" verify-tag "$oid" &&
	tag_oid=$(git rev-parse verify-tag) &&
	git tag -d verify-tag &&

	check_connected_trace trace-tag.txt 1 1 "$tag_oid"
'

test_expect_success_in main-repo "$mode: reuses tree OID after change-then-revert" '
	c1=$(commit_with_change HEAD file-1.txt "revert-tmp") &&
	c2=$(commit_with_change "$c1" file-1.txt "file 1") &&
	c3=$(commit_with_change "$c2" file-1.txt "revert-final") &&

	# c2 reverts file-1.txt to original content, so its root
	# tree matches HEAD.  Visited-set dedup means it is not
	# re-walked when reached from c3.
	check_connected_trace trace-revert.txt 2 2 "$c3"
'

test_expect_success_in main-repo "$mode: handles repeated blob content across commits" '
	c1=$(commit_with_change HEAD file-1.txt "shared") &&
	c2=$(commit_with_change "$c1" file-1.txt "temp") &&
	c3=$(commit_with_change "$c2" file-1.txt "shared") &&

	# c1 and c3 share the same blob OID for file-1.txt.
	# The visited set deduplicates so the shared blob is
	# only counted once.
	check_connected_trace trace-repeat.txt "" 2 "$c3"
'

# Merge with shared subtree at different paths.
# Needs its own setup because the merge topology cannot be built
# with commit_with_change.

test_expect_success_in main-repo "$mode: skips subtree reused at different path (merge)" '
	# Build two branch commits that add the same subtree
	# content at different paths, without updating any refs.
	blob_a=$(echo a | git hash-object -w --stdin) &&
	blob_b=$(echo b | git hash-object -w --stdin) &&
	blob_c=$(echo c | git hash-object -w --stdin) &&
	shared_tree=$(printf "100644 blob %s\tfile1.txt\n100644 blob %s\tfile2.txt\n100644 blob %s\tfile3.txt\n" \
		"$blob_a" "$blob_b" "$blob_c" | git mktree) &&

	TMP_IDX=.git/tmp-idx &&

	GIT_INDEX_FILE=$TMP_IDX git read-tree HEAD &&
	GIT_INDEX_FILE=$TMP_IDX git read-tree --prefix=shared-a/ "$shared_tree" &&
	tree_a=$(GIT_INDEX_FILE=$TMP_IDX git write-tree) &&
	commit_a=$(git commit-tree "$tree_a" -p HEAD -m "branch-a") &&

	GIT_INDEX_FILE=$TMP_IDX git read-tree HEAD &&
	GIT_INDEX_FILE=$TMP_IDX git read-tree --prefix=shared-b/ "$shared_tree" &&
	tree_b=$(GIT_INDEX_FILE=$TMP_IDX git write-tree) &&
	commit_b=$(git commit-tree "$tree_b" -p HEAD -m "branch-b") &&

	rm -f "$TMP_IDX" &&

	# Merge the two branches (using branch-a tree as the
	# merge result -- the exact content does not matter,
	# only that both parents are walked).
	merge=$(git commit-tree "$tree_a" \
		-p "$commit_a" -p "$commit_b" -m "merge") &&

	oid=$(commit_with_change "$merge" file-1.txt "post-merge") &&

	check_connected_trace trace-reuse.txt 4 4 "$oid"
'

test_expect_success_in main-repo "$mode: traverses octopus merge (3 parents)" '
	c1=$(commit_with_change HEAD file-1.txt "oct-a") &&
	c2=$(commit_with_change HEAD file-2.txt "oct-b") &&
	c3=$(commit_with_change HEAD a/other.txt "oct-c") &&

	# Octopus: merge tree uses c1 as base, all three are parents.
	merge_tree=$(git rev-parse "$c1^{tree}") &&
	octopus=$(git commit-tree "$merge_tree" \
		-p "$c1" -p "$c2" -p "$c3" -m "octopus") &&

	# c1: root tree walked, 1 blob (file-1.txt).
	# c2: root tree walked, 1 blob (file-2.txt).
	# c3: root + a/ walked, 1 blob (a/other.txt).
	# octopus: tree matches c1, already verified -- skipped.
	# Total: 4 trees, 3 blobs.
	check_connected_trace trace-octopus.txt 4 3 "$octopus"
'

test_expect_success_in main-repo "$mode: handles gitlink entries (submodules)" '
	fake_oid=$(cat .git/fake-oid) &&
	TMP_IDX=.git/tmp-idx &&
	GIT_INDEX_FILE=$TMP_IDX git read-tree HEAD &&
	GIT_INDEX_FILE=$TMP_IDX git update-index --add \
		--cacheinfo "160000,$fake_oid,my-submodule" &&
	gitlink_tree=$(GIT_INDEX_FILE=$TMP_IDX git write-tree) &&
	rm -f "$TMP_IDX" &&
	gitlink_commit=$(git commit-tree "$gitlink_tree" -p HEAD \
		-m "add gitlink") &&

	# Gitlink entries are skipped -- the missing submodule
	# commit OID does not cause a failure.
	check_connected_trace trace-gitlink.txt 1 0 "$gitlink_commit"
'

# Replacement objects.

test_expect_success_in replace-test "$mode: accepts with replacement objects" '
	original=$(cat .git/test-oid) &&
	test-tool check-connected "$original"
'

test_expect_success_in replace-test "$mode: rejects without replacement objects" '
	original=$(cat .git/test-oid) &&
	test_must_fail env GIT_NO_REPLACE_OBJECTS=1 \
		test-tool check-connected "$original" 2>err &&
	test_grep "missing blob object" err
'

# Shallow edge cases.

test_expect_success "$mode: rejects missing blob behind shallow boundary" '
	test_when_finished "rm -rf shallow-boundary" &&

	git init shallow-boundary &&
	(
		cd shallow-boundary &&
		set_connectivity_check $mode &&

		test_commit --no-tag "parent P" file.txt content &&
		parent=$(git rev-parse HEAD) &&
		blob_oid=$(git rev-parse HEAD:file.txt) &&

		tree_oid=$(git rev-parse HEAD^{tree}) &&
		child=$(git commit-tree -p "$parent" -m "child S" "$tree_oid") &&

		rm .git/objects/$(test_oid_to_path "$blob_oid") &&

		echo "$child" >shallow_file &&

		test_must_fail test-tool check-connected \
			--shallow-file shallow_file "$child" 2>err &&
		test_grep "missing blob object" err
	)
'

test_expect_success "$mode: rejects malformed shallow file" '
	test_when_finished "rm -rf malformed-shallow" &&

	git init malformed-shallow &&
	(
		cd malformed-shallow &&
		set_connectivity_check $mode &&
		test_commit --no-tag base file.txt content &&
		oid=$(git rev-parse HEAD) &&

		echo "not-a-valid-oid" >bad_shallow &&

		test_expect_code 1 test-tool check-connected \
			--shallow-file bad_shallow "$oid" 2>err &&
		test_grep "bad shallow line" err
	)
'

# Partial clone: promisor objects should be accepted.

test_expect_success "$mode: accepts missing promised blob" '
	test_when_finished "rm -rf prom-src prom-server.git prom-client" &&

	git init prom-src &&
	test_commit -C prom-src --no-tag base file.txt original &&
	test_commit -C prom-src --no-tag "add file2" file2.txt extra &&
	git clone --bare prom-src prom-server.git &&
	git -C prom-server.git config uploadpack.allowfilter true &&
	git -C prom-server.git config uploadpack.allowanysha1inwant true &&

	git clone --no-checkout --filter=blob:none \
		"file://$(pwd)/prom-server.git" prom-client &&
	set_connectivity_check prom-client $mode &&

	(
		cd prom-client &&
		promised_blob=$(git rev-parse HEAD:file2.txt) &&

		test_must_fail env GIT_NO_LAZY_FETCH=1 \
			git cat-file -e "$promised_blob" &&

		new_tree=$(printf "100644 blob %s\tnewname.txt\n" \
			"$promised_blob" |
			git mktree --missing) &&
		new_commit=$(git commit-tree "$new_tree" \
			-p HEAD -m "reuse promised blob") &&

		test-tool check-connected "$new_commit" &&

		# Verify connectivity checking did not lazy-fetch it.
		test_must_fail env GIT_NO_LAZY_FETCH=1 \
			git cat-file -e "$promised_blob"
	)
'

test_expect_success "$mode: accepts missing promised tree" '
	test_when_finished "rm -rf prom-tree-src prom-tree-server.git prom-tree-client" &&

	git init prom-tree-src &&
	mkdir -p prom-tree-src/a/b &&
	test_commit -C prom-tree-src --no-tag "nested dirs" a/b/file.txt deep &&
	git clone --bare prom-tree-src prom-tree-server.git &&
	git -C prom-tree-server.git config uploadpack.allowfilter true &&
	git -C prom-tree-server.git config uploadpack.allowanysha1inwant true &&

	git clone --no-checkout --filter=tree:1 \
		"file://$(pwd)/prom-tree-server.git" prom-tree-client &&
	set_connectivity_check prom-tree-client $mode &&

	(
		cd prom-tree-client &&
		# Subtree "a/" is promised but not present locally.
		promised_tree=$(git ls-tree HEAD | grep "	a$" | cut -f1 | awk "{print \$3}") &&
		test_must_fail env GIT_NO_LAZY_FETCH=1 \
			git cat-file -e "$promised_tree" &&

		# Build a new tree that reuses the promised subtree
		# at a different path.
		new_tree=$(printf "40000 tree %s\trenamed\n" \
			"$promised_tree" |
			git mktree --missing) &&
		new_commit=$(git commit-tree "$new_tree" \
			-p HEAD -m "reuse promised tree") &&

		test-tool check-connected "$new_commit" &&

		# Verify connectivity checking did not lazy-fetch it.
		test_must_fail env GIT_NO_LAZY_FETCH=1 \
			git cat-file -e "$promised_tree"
	)
'

test_expect_success "$mode: verifies local commit in partial clone" '
	test_when_finished "rm -rf pc-src pc-server.git pc-client" &&

	git init pc-src &&
	test_commit -C pc-src --no-tag base file.txt &&
	git clone --bare pc-src pc-server.git &&
	git -C pc-server.git config uploadpack.allowfilter true &&
	git -C pc-server.git config uploadpack.allowanysha1inwant true &&
	git clone --filter=blob:none \
		"file://$(pwd)/pc-server.git" pc-client &&
	set_connectivity_check pc-client $mode &&

	(
		cd pc-client &&
		test_commit --no-tag "local change" file.txt local-content &&
		local_commit=$(git rev-parse HEAD) &&

		test-tool check-connected "$local_commit"
	)
'

# Deepening fetch: verify the operation succeeds with both modes.

test_expect_success "$mode: deepening fetch succeeds" '
	test_when_finished "rm -rf deepen-src deepen-server.git deepen-client" &&

	git init deepen-src &&
	test_commit -C deepen-src --no-tag c1 file.txt &&
	test_commit -C deepen-src --no-tag c2 file.txt &&
	test_commit -C deepen-src --no-tag c3 file.txt &&
	git clone --bare deepen-src deepen-server.git &&
	git clone --depth=1 "file://$(pwd)/deepen-server.git" deepen-client &&
	set_connectivity_check deepen-client $mode &&
	test -f deepen-client/.git/shallow &&
	git -C deepen-client fetch --deepen=2 origin main
'

done

# Algorithm selection: verify fallback and rejection behavior.

test_expect_success 'incremental falls back with replacement objects' '
	(
		cd replace-test &&
		set_connectivity_check incremental &&
		original=$(cat .git/test-oid) &&
		GIT_TRACE2_EVENT="$(pwd)/trace-fallback.txt" \
			test-tool check-connected "$original" &&
		test_region ! connectivity incremental trace-fallback.txt
	)
'

test_expect_success 'invalid transfer.connectivityCheck is rejected' '
	test_when_finished "rm -rf invalid-cfg" &&

	git init invalid-cfg &&
	(
		cd invalid-cfg &&
		test_commit --no-tag base file.txt &&
		git config transfer.connectivityCheck bogus &&
		oid=$(git rev-parse HEAD) &&
		test_must_fail test-tool check-connected "$oid" 2>err &&
		test_grep "unknown transfer.connectivityCheck" err
	)
'

# Integration: verify incremental runs during a real push.

test_expect_success 'push uses incremental when configured' '
	test_when_finished "rm -rf int-src int-dst.git" &&

	git init int-src &&
	test_commit -C int-src --no-tag base file.txt &&
	git clone --bare int-src int-dst.git &&
	test_commit -C int-src --no-tag update file.txt updated &&

	set_connectivity_check int-dst.git incremental &&
	GIT_TRACE2_EVENT="$(pwd)/trace-push.txt" \
		git -C int-src push ../int-dst.git main &&
	test_region connectivity incremental trace-push.txt
'

test_expect_success 'fetch uses incremental when configured' '
	test_when_finished "rm -rf fetch-src fetch-dst" &&

	git init fetch-src &&
	test_commit -C fetch-src --no-tag base file.txt &&
	git clone fetch-src fetch-dst &&
	test_commit -C fetch-src --no-tag update file.txt updated &&

	set_connectivity_check fetch-dst incremental &&
	GIT_TRACE2_EVENT="$(pwd)/trace-fetch.txt" \
		git -C fetch-dst fetch origin main &&
	test_region connectivity incremental trace-fetch.txt
'

test_expect_success 'clone respects transfer.connectivityCheck' '
	test_when_finished "rm -rf clone-src clone-dst" &&

	git init clone-src &&
	test_commit -C clone-src --no-tag base file.txt &&

	GIT_TRACE2_EVENT="$(pwd)/trace-clone.txt" \
		git -c transfer.connectivityCheck=incremental \
		clone --no-local clone-src clone-dst &&
	test_region connectivity incremental trace-clone.txt
'

# Error routing: verify --err-file and --quiet behavior.

test_expect_success 'incremental: errors written to err-file' '
	(
		cd main-repo &&
		set_connectivity_check incremental &&
		fake_oid=$(cat .git/fake-oid) &&
		bad_tree=$(printf "100644 blob %s\tfile.txt\n" "$fake_oid" |
			git mktree --missing) &&
		bad_commit=$(git commit-tree "$bad_tree" -p HEAD -m "bad") &&

		test_must_fail test-tool check-connected \
			--err-file err.out "$bad_commit" 2>stderr.out &&
		test_grep "missing blob object" err.out &&
		test_must_be_empty stderr.out
	)
'

test_expect_success 'incremental: --quiet suppresses errors without err-file' '
	(
		cd main-repo &&
		set_connectivity_check incremental &&
		fake_oid=$(cat .git/fake-oid) &&
		bad_tree=$(printf "100644 blob %s\tfile.txt\n" "$fake_oid" |
			git mktree --missing) &&
		bad_commit=$(git commit-tree "$bad_tree" -p HEAD -m "bad") &&

		test_must_fail test-tool check-connected \
			--quiet "$bad_commit" 2>stderr.out &&
		test_must_be_empty stderr.out
	)
'

test_expect_success 'incremental: --quiet with err-file still writes to fd' '
	(
		cd main-repo &&
		set_connectivity_check incremental &&
		fake_oid=$(cat .git/fake-oid) &&
		bad_tree=$(printf "100644 blob %s\tfile.txt\n" "$fake_oid" |
			git mktree --missing) &&
		bad_commit=$(git commit-tree "$bad_tree" -p HEAD -m "bad") &&

		test_must_fail test-tool check-connected \
			--quiet --err-file err-quiet.out "$bad_commit" 2>stderr.out &&
		test_grep "missing blob object" err-quiet.out &&
		test_must_be_empty stderr.out
	)
'

test_expect_success 'incremental: boundary search errors routed to err-file' '
	test_when_finished "rm -rf broken-parent" &&

	git init broken-parent &&
	(
		cd broken-parent &&
		set_connectivity_check incremental &&

		test_commit --no-tag first file.txt &&
		parent=$(git rev-parse HEAD) &&
		test_commit --no-tag second file.txt update &&
		child=$(git rev-parse HEAD) &&

		rm -rf .git/objects/info/commit-graph* &&
		rm .git/objects/$(test_oid_to_path "$parent") &&

		git update-ref -d refs/heads/main &&
		git update-ref -d HEAD &&

		test_must_fail test-tool check-connected \
			--err-file boundary-err.out "$child" 2>stderr.out &&
		test_file_not_empty boundary-err.out &&
		test_must_be_empty stderr.out
	)
'

test_done
