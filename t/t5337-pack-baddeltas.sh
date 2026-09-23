#!/bin/sh

test_description='`.baddeltas` sidecar disables the same-pack try_delta() skip'

. ./test-lib.sh

# Two similar but distinct blobs.  The contents are deliberately large
# enough and similar enough that a real delta search will find a useful
# delta between them.
generate_blobs () {
	{
		printf "header\n" &&
		i=0 &&
		while test $i -lt 200
		do
			printf "line %d padding-padding-padding-padding\n" $i &&
			i=$((i + 1)) || return 1
		done
	} >a &&
	cp a b &&
	printf "tail-line\n" >>b
}

# Build a pack from the two blobs without doing any delta search, and
# echo the basename of the resulting .pack file.
build_input_pack () {
	A=$(git hash-object -w a) &&
	B=$(git hash-object -w b) &&
	pack_hash=$(printf "%s\n%s\n" "$A" "$B" |
		git pack-objects --window=0 .git/objects/pack/pack) &&
	git prune-packed &&
	echo "pack-$pack_hash.pack"
}

# Count delta entries in a pack idx.  "git verify-pack -v" prints one
# line per object with 5 fields for non-delta entries (oid, type, size,
# size-in-pack, offset) and 7 fields for delta entries (... depth
# base-oid).
count_deltas () {
	git verify-pack -v "$1" |
	awk 'NF == 7 { n++ } END { print n + 0 }'
}

# Repack just the two blobs from the existing pack into a fresh pack
# with prefix "out".  Echo the basename of the resulting .pack file.
repack_blobs () {
	A=$(git hash-object a) &&
	B=$(git hash-object b) &&
	pack_hash=$(printf "%s\n%s\n" "$A" "$B" |
		git pack-objects .git/objects/pack/out) &&
	echo "out-$pack_hash.pack"
}

test_expect_success 'set up two similar blobs' '
	git init repo &&
	(
		cd repo &&
		generate_blobs
	)
'

test_expect_success 'without .baddeltas, same-pack pair is skipped' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		input_pack=$(build_input_pack) &&
		test 0 -eq "$(count_deltas .git/objects/pack/${input_pack%.pack}.idx)" &&
		out_pack=$(repack_blobs) &&
		test 0 -eq "$(count_deltas .git/objects/pack/${out_pack%.pack}.idx)"
	)
'

test_expect_success 'with .baddeltas, same-pack pair gets reconsidered' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		input_pack=$(build_input_pack) &&
		test 0 -eq "$(count_deltas .git/objects/pack/${input_pack%.pack}.idx)" &&
		>.git/objects/pack/${input_pack%.pack}.baddeltas &&
		out_pack=$(repack_blobs) &&
		test 1 -le "$(count_deltas .git/objects/pack/${out_pack%.pack}.idx)"
	)
'

test_expect_success '.baddeltas does not trigger garbage warnings' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		input_pack=$(build_input_pack) &&
		>.git/objects/pack/${input_pack%.pack}.baddeltas &&
		git count-objects -v 2>warnings &&
		test_grep ! -i garbage warnings
	)
'

test_expect_success 'repacking an unchanged pack removes .baddeltas' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		# Tiny blobs leave no useful deltas to find.
		for i in 1 2 3 4
		do
			oid=$(echo "marker-$i" | git hash-object -w --stdin) &&
			git update-ref "refs/tags/blob-$i" "$oid" || return 1
		done &&
		git repack -ad &&
		ls .git/objects/pack/pack-*.pack >before &&
		test_line_count = 1 before &&
		read pack <before &&
		>"${pack%.pack}.baddeltas" &&
		git repack -ad &&
		ls .git/objects/pack/pack-*.pack >after &&
		test_cmp before after &&
		test_path_is_missing "${pack%.pack}.baddeltas" &&
		git fsck
	)
'

test_expect_success '.baddeltas is removed by git repack -d' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		input_pack=$(build_input_pack) &&
		>.git/objects/pack/${input_pack%.pack}.baddeltas &&
		git repack -ad &&
		test_path_is_missing .git/objects/pack/${input_pack%.pack}.baddeltas &&
		test_path_is_missing .git/objects/pack/$input_pack
	)
'

test_expect_success 'pack-objects --mark-bad-deltas writes .baddeltas' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		A=$(git hash-object -w a) &&
		B=$(git hash-object -w b) &&
		printf "%s\n%s\n" "$A" "$B" |
			git pack-objects --window=0 --mark-bad-deltas \
				.git/objects/pack/pack >pack.hash &&
		pack="pack-$(cat pack.hash).pack" &&
		test_path_is_file .git/objects/pack/${pack%.pack}.baddeltas &&
		test_must_be_empty .git/objects/pack/${pack%.pack}.baddeltas &&
		test 0 -eq "$(count_deltas .git/objects/pack/${pack%.pack}.idx)" &&
		git prune-packed &&

		out_pack=$(repack_blobs) &&
		test 1 -le "$(count_deltas .git/objects/pack/${out_pack%.pack}.idx)"
	)
'

test_expect_success 'pack-objects --mark-bad-deltas is incompatible with --stdout' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		A=$(git hash-object -w a) &&
		printf "%s\n" "$A" >in &&
		test_must_fail git pack-objects --mark-bad-deltas --stdout \
			<in >/dev/null 2>err &&
		test_grep "cannot be used together" err
	)
'

test_done
