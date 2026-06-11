#!/bin/sh

test_description='`git pack-aggregate` rolls up small packs and loose objects'

. ./test-lib.sh

# Build N tiny packs in objects/pack/, each containing one distinct
# blob.  Echoes the basenames (without .pack) one per line.
build_n_packs () {
	n=$1 &&
	mkdir -p .git/objects/pack &&
	i=0 &&
	while test $i -lt "$n"
	do
		blob=$(echo "content-$i-$$" | git hash-object -w --stdin) &&
		echo "$blob" |
		git pack-objects --window=0 .git/objects/pack/pack \
			>pack_hash &&
		hash=$(cat pack_hash) &&
		test -n "$hash" &&
		test_path_is_file .git/objects/pack/pack-${hash}.pack &&
		git prune-packed &&
		echo "pack-$hash" &&
		i=$((i + 1)) || return 1
	done
}

# Create N loose objects in the repository.  Echoes the OIDs one per
# line.
build_n_loose () {
	n=$1 &&
	i=0 &&
	while test $i -lt "$n"
	do
		echo "loose-$i-$$" | git hash-object -w --stdin &&
		i=$((i + 1)) || return 1
	done
}

count_packs () {
	ls .git/objects/pack/pack-*.pack 2>/dev/null | wc -l
}

count_baddeltas () {
	ls .git/objects/pack/pack-*.baddeltas 2>/dev/null | wc -l
}

count_loose () {
	find .git/objects \
		-type f \
		-name '[0-9a-f]*' \
		-not -path '.git/objects/pack/*' \
		-not -path '.git/objects/info/*' |
	wc -l
}

test_expect_success 'setup an empty repo' '
	git init repo
'

test_expect_success '--once is required' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		test_must_fail git pack-aggregate 2>err &&
		test_grep -- "--once is required" err
	)
'

test_expect_success '--once below --min-packs is a no-op for packs' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 3 >/dev/null &&
		test 3 -eq "$(count_packs)" &&
		git pack-aggregate --once --min-loose=1000 &&
		test 3 -eq "$(count_packs)" &&
		test 0 -eq "$(count_baddeltas)"
	)
'

test_expect_success '--once aggregates above --min-packs' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >packs.txt &&
		test 5 -eq "$(count_packs)" &&
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=5 &&
		test 1 -eq "$(count_packs)" &&
		test 1 -eq "$(count_baddeltas)" &&
		git fsck
	)
'

test_expect_success '--once prefers an existing delta over a duplicate base copy' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		# The shared seed makes o.bin a prefix of x.bin, so O can be
		# stored as a small delta against X.
		test-tool genrandom foo 8192 >x.bin &&
		test-tool genrandom foo 4096 >o.bin &&
		x=$(git hash-object -w x.bin) &&
		o=$(git hash-object -w o.bin) &&

		echo "$o" |
		git pack-objects --window=0 .git/objects/pack/pack >base.hash &&
		printf "%s\n%s\n" "$x" "$o" |
		git pack-objects .git/objects/pack/pack >delta.hash &&
		git prune-packed &&

		delta=".git/objects/pack/pack-$(cat delta.hash)" &&
		git verify-pack -v "$delta.idx" >before &&
		# Delta entries end with their base OID; full-object entries do not.
		test_grep "^$o .* $x$" before &&

		# The base copy must be encountered before the delta copy.
		test-tool chmtime -200 "$delta.pack" &&
		git pack-aggregate --once --min-packs=2 --min-loose=1000 &&
		test 1 -eq "$(count_packs)" &&
		git verify-pack -v .git/objects/pack/pack-*.idx >after &&
		test_grep "^$o .* $x$" after
	)
'

for input_mode in loose packed
do
	test_expect_success "pack.packSizeLimit splits $input_mode aggregation outputs" '
		test_when_finished "rm -fr work" &&
		cp -R repo work &&
		(
			cd work &&
			# Each blob fits in 1 MiB, but no pair does.
			for i in 1 2 3
			do
				test-tool genrandom "split-$i" 716800 >blob &&
				oid=$(git hash-object -w blob) &&
				test-tool chmtime -1 \
					".git/objects/$(test_oid_to_path "$oid")" &&
				echo "$oid" || return 1
			done >oids &&
			if test "$input_mode" = packed
			then
				git pack-objects --window=0 .git/objects/pack/pack \
					<oids >input.hash &&
				git prune-packed
			fi &&
			sort oids >expect &&

			# On the second pass, all three output names match inputs.
			for pass in 1 2
			do
				git -c pack.packSizeLimit=1m pack-aggregate --once \
					--min-loose=1 --min-packs=1 &&
				test 3 -eq "$(count_packs)" &&
				test 3 -eq "$(count_baddeltas)" &&
				test 0 -eq "$(count_loose)" &&
				find .git/objects/pack -name ".tmp-*" >temporary &&
				test_must_be_empty temporary &&
				git verify-pack .git/objects/pack/pack-*.idx &&
				git cat-file --batch-all-objects \
					--batch-check="%(objectname)" >actual.raw &&
				sort actual.raw >actual &&
				test_cmp expect actual || return 1
			done
		)
	'
done

test_expect_success 'sidecar-marked packs are skipped' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 6 >packs.txt &&
		first=$(head -n 1 packs.txt) &&
		>.git/objects/pack/${first}.keep &&
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=4 &&
		test_path_is_file .git/objects/pack/${first}.pack &&
		test_path_is_file .git/objects/pack/${first}.keep &&
		# 1 kept + 1 aggregate = 2 packs total.
		test 2 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success 'aggregate re-rolls up .baddeltas packs' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=5 &&
		test 1 -eq "$(count_packs)" &&
		test 1 -eq "$(count_baddeltas)" &&
		# Now add more small packs and aggregate again.  The
		# previous .baddeltas pack should NOT be protected (we
		# want to keep rolling them up).
		build_n_packs 4 >/dev/null &&
		test 5 -eq "$(count_packs)" &&
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=5 &&
		test 1 -eq "$(count_packs)" &&
		test 1 -eq "$(count_baddeltas)" &&
		git fsck
	)
'

test_expect_success 'aggregates loose objects above --min-loose' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_loose 5 >/dev/null &&
		test 5 -eq "$(count_loose)" &&
		git pack-aggregate --once \
			--min-loose=1 --min-packs=1000 &&
		test 0 -eq "$(count_loose)" &&
		test 1 -eq "$(count_packs)" &&
		test 1 -eq "$(count_baddeltas)" &&
		git fsck
	)
'

test_expect_success 'loose then pack aggregation in one cycle' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_loose 5 >/dev/null &&
		build_n_packs 5 >/dev/null &&
		test 5 -eq "$(count_loose)" &&
		test 5 -eq "$(count_packs)" &&
		git pack-aggregate --once \
			--min-loose=1 --min-packs=5 &&
		# Step 1 creates one pack from the loose objects, then
		# step 2 sweeps that pack plus the five existing ones
		# into a single aggregate.
		test 0 -eq "$(count_loose)" &&
		test 1 -eq "$(count_packs)" &&
		test 1 -eq "$(count_baddeltas)" &&
		git fsck
	)
'

# ---- geometric .baddeltas demotion ----

# Helper for the geometric tests: pack N unique blobs (default 1) into
# one packfile.  The `tag` parameter must differ between callers so
# that the blob contents do not collide with one another (otherwise
# `git pack-objects --stdin-packs` would see the "rollup" objects
# already present in the kept pack and emit nothing).
make_unique_pack () {
	tag=$1 &&
	n=${2:-1} &&
	i=0 &&
	while test "$i" -lt "$n"
	do
		echo "unique-${tag}-${i}-$$" | git hash-object -w --stdin &&
		i=$((i + 1)) || return 1
	done >oids &&
	git pack-objects --window=0 .git/objects/pack/pack \
		<oids >pack_hash &&
	git prune-packed
}

for scenario in middle already-geometric largest boundary larger-candidate \
	multiple-markers factor-three factor-one
do
	test_expect_success "geometric demotion preserves spacing ($scenario)" '
		test_when_finished "rm -fr work" &&
		cp -R repo work &&
		(
			cd work &&
			factor=2 &&
			case "$scenario" in
			middle)
				sizes="1 1 4 8 16" &&
				marked="8" &&
				expected="4 26"
				;;
			already-geometric|factor-one)
				sizes="1 2 4 8 16" &&
				marked="4" &&
				expected="$sizes" &&
				if test "$scenario" = factor-one
				then
					factor=1
				fi
				;;
			largest)
				sizes="1 2 4 8 16" &&
				marked="16" &&
				expected="$sizes"
				;;
			boundary)
				sizes="1 1 4 8 20" &&
				marked="8" &&
				expected="4 10 20"
				;;
			larger-candidate)
				sizes="1 1 4 8 16 32 96" &&
				marked="8" &&
				expected="4 16 42 96"
				;;
			multiple-markers)
				sizes="1 2 4 8 16 32 64" &&
				marked="4 16" &&
				expected="1 2 8 32 84"
				;;
			factor-three)
				factor=3 &&
				sizes="1 1 6 18 54" &&
				marked="18" &&
				expected="6 74"
				;;
			esac &&
			batch=0 &&
			for size in $sizes
			do
				make_unique_pack "batch-$batch" "$size" &&
				hash=$(cat pack_hash) &&
				pack_base=".git/objects/pack/pack-$hash" &&
				case " $marked " in
				*" $size "*)
					>"$pack_base.baddeltas"
					;;
				esac &&
				batch=$((batch + 1)) || return 1
			done &&
			git cat-file --batch-all-objects \
				--batch-check="%(objectname)" >before &&
			git repack -d --geometric="$factor" &&
			for idx in .git/objects/pack/pack-*.idx
			do
				git show-index <"$idx" >index &&
				awk "END { print NR }" index || return 1
			done >counts &&
			sort -n counts >actual &&
			for size in $expected
			do
				echo "$size" || return 1
			done >expect &&
			test_cmp expect actual &&
			test 0 -eq "$(count_baddeltas)" &&
			git cat-file --batch-all-objects \
				--batch-check="%(objectname)" >after &&
			test_cmp before after &&
			git fsck
		)
	'
done

test_expect_success 'geometric repack demotes .baddeltas packs into rollup' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		# Build several small packs and consolidate them into
		# one larger pack that would normally sit above the
		# geometric split.
		build_n_packs 8 >/dev/null &&
		git repack -d --geometric=2 &&
		test 1 -eq "$(count_packs)" &&
		big=$(ls .git/objects/pack/pack-*.pack) &&
		# Drop a .baddeltas marker on it (simulating an
		# aggregator output).
		>"${big%.pack}.baddeltas" &&
		test 1 -eq "$(count_baddeltas)" &&
		# Add small packs that are individually far smaller
		# than the big one; ordinarily the big pack would be
		# kept above the geometric split.
		make_unique_pack a &&
		make_unique_pack b &&
		make_unique_pack c &&
		test 4 -eq "$(count_packs)" &&
		# With the .baddeltas demotion, the big pack is rolled
		# up despite being above the natural split, and the
		# resulting pack carries no .baddeltas marker.
		git repack -d --geometric=2 &&
		test 1 -eq "$(count_packs)" &&
		test 0 -eq "$(count_baddeltas)" &&
		git fsck
	)
'

test_expect_success 'geometric repack leaves non-baddeltas packs above the split alone' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 8 >/dev/null &&
		git repack -d --geometric=2 &&
		test 1 -eq "$(count_packs)" &&
		# Add small packs; without any .baddeltas marker the
		# big pack should be preserved above the split.
		make_unique_pack a &&
		make_unique_pack b &&
		make_unique_pack c &&
		test 4 -eq "$(count_packs)" &&
		git repack -d --geometric=2 &&
		# 1 kept big pack + 1 newly-aggregated rollup = 2.
		test 2 -eq "$(count_packs)" &&
		test 0 -eq "$(count_baddeltas)" &&
		git fsck
	)
'

test_done
