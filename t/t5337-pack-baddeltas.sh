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
	{
		printf "header\n" &&
		i=0 &&
		while test $i -lt 200
		do
			printf "line %d padding-padding-padding-padding\n" $i &&
			i=$((i + 1)) || return 1
		done &&
		printf "tail-line\n"
	} >b
}

# Build a pack from the two blobs without doing any delta search, and
# echo the basename of the resulting .pack file (the last "pack-*.pack"
# in objects/pack).
build_input_pack () {
	A=$(git hash-object -w a) &&
	B=$(git hash-object -w b) &&
	printf "%s\n%s\n" "$A" "$B" |
	git pack-objects --window=0 .git/objects/pack/pack >/dev/null &&
	git prune-packed &&
	basename "$(ls -t .git/objects/pack/pack-*.pack | head -1)"
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
	rm -f .git/objects/pack/out-*.pack \
	      .git/objects/pack/out-*.idx \
	      .git/objects/pack/out-*.rev &&
	printf "%s\n%s\n" "$A" "$B" |
	git pack-objects .git/objects/pack/out >/dev/null &&
	basename "$(ls .git/objects/pack/out-*.pack)"
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
		! grep -i garbage warnings
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

test_done
