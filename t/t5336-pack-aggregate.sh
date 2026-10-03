#!/bin/sh

test_description='`git pack-aggregate` rolls up small packs and loose objects'

. ./test-lib.sh
. "$TEST_DIRECTORY/lib-terminal.sh"

# Build N tiny packs in objects/pack/, each containing one distinct
# blob.  Echoes the basenames (without .pack) one per line.
# An optional salt (default: $$) distinguishes separate batches.
build_n_packs () {
	n=$1 &&
	salt=${2-$$} &&
	mkdir -p .git/objects/pack &&
	i=0 &&
	while test $i -lt "$n"
	do
		blob=$(echo "content-$i-$salt" | git hash-object -w --stdin) &&
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
	# Ensure these predate the later cutoff marker even without nanoseconds.
	while test $i -lt "$n"
	do
		oid=$(echo "loose-$i-$$" | git hash-object -w --stdin) &&
		test-tool chmtime -1 \
			".git/objects/$(test_oid_to_path "$oid")" &&
		echo "$oid" &&
		i=$((i + 1)) || return 1
	done
}

# Build a single pack containing N distinct blobs.  Echoes the pack
# basename (without .pack).
build_big_pack () {
	n=$1 &&
	mkdir -p .git/objects/pack &&
	i=0 &&
	while test $i -lt "$n"
	do
		echo "big-$i-$$" | git hash-object -w --stdin &&
		i=$((i + 1)) || return 1
	done >big_oids &&
	git pack-objects --window=0 .git/objects/pack/pack \
		<big_oids >big_hash &&
	hash=$(cat big_hash) &&
	test -n "$hash" &&
	test_path_is_file .git/objects/pack/pack-${hash}.pack &&
	git prune-packed &&
	echo "pack-$hash"
}

count_packs () {
	ls .git/objects/pack/pack-*.pack 2>/dev/null | wc -l
}

count_unoptimized () {
	ls .git/objects/pack/pack-*.unoptimized 2>/dev/null | wc -l
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

test_expect_success 'an aggregation mode is required' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		test_must_fail git pack-aggregate 2>err &&
		test_grep "exactly one of --once or --loop" err
	)
'

test_expect_success 'aggregation modes and intervals are validated' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		test_must_fail git pack-aggregate --once --loop 2>err &&
		test_grep "exactly one of --once or --loop" err &&
		test_must_fail git pack-aggregate --loop --interval=0 2>err &&
		test_grep -- "--interval must be at least 1" err &&
		test_must_fail git pack-aggregate --once --min-loose=0 2>err &&
		test_grep -- "--min-loose must be at least 1" err &&
		test_must_fail git pack-aggregate --once --min-packs=0 2>err &&
		test_grep -- "--min-packs must be at least 1" err
	)
'

test_expect_success !MINGW 'loop exits when the parent pipe closes' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		: | git pack-aggregate --loop --interval=60 \
			--parent-pipe-fd=0
	)
'

test_expect_success !MINGW 'loop rejects a closed parent descriptor' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		test_must_fail git pack-aggregate --loop \
			--parent-pipe-fd=9 9<&- 2>err &&
		test_grep "invalid parent pipe descriptor" err
	)
'

test_expect_success 'explicit progress works without a terminal' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		git pack-aggregate --once --progress 2>err &&
		test_grep "Enumerating objects" err
	)
'

test_expect_success TTY 'one-shot progress defaults and override on a terminal' '
	test_when_finished "rm -fr work" &&
	for option in default --no-progress
	do
		rm -fr work &&
		cp -R repo work &&
		(
			cd work &&
			build_n_packs 5 >/dev/null &&
			if test "$option" = default
			then
				test_terminal git pack-aggregate --once \
					>out 2>err &&
				test_grep "Enumerating objects" err
			else
				test_terminal git pack-aggregate --once \
					"$option" >out 2>err &&
				test_must_be_empty err
			fi
		) || return 1
	done
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
		test 0 -eq "$(count_unoptimized)"
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
		test 1 -eq "$(count_unoptimized)" &&
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

test_expect_success '--keep-pack excludes repeated names before applying --min-packs' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 6 >packs.txt &&
		sort -r packs.txt >sorted &&
		first=$(head -n 1 sorted) &&
		second=$(sed -n 2p sorted) &&
		git pack-aggregate --once --min-loose=1000 \
			--min-packs=5 \
			--keep-pack="$first.pack" \
			--keep-pack="$second.pack" &&
		test 6 -eq "$(count_packs)" &&
		git pack-aggregate --once --min-loose=1000 \
			--min-packs=4 \
			--keep-pack="$first.pack" \
			--keep-pack="$second.pack" &&
		test 3 -eq "$(count_packs)" &&
		test 1 -eq "$(count_unoptimized)" &&
		for name in "$first" "$second"
		do
			test_path_is_file ".git/objects/pack/$name.pack" &&
			test_path_is_file ".git/objects/pack/$name.idx" ||
			return 1
		done &&
		git fsck
	)
'

for input_mode in loose packed
do
	test_expect_success "pack.packSizeLimit splits $input_mode aggregation outputs" '
		test_when_finished "rm -fr work" &&
		cp -R repo work &&
		(
			cd work &&
			blob_size=716800 &&
			output_packs=3 &&
			if test "$input_mode" = packed
			then
				# Each input must fit below the half-limit gate.
				blob_size=409600 &&
				output_packs=2
			fi &&
			for i in 1 2 3
			do
				test-tool genrandom "split-$i" "$blob_size" >blob &&
				oid=$(git hash-object -w blob) &&
				test-tool chmtime -1 \
					".git/objects/$(test_oid_to_path "$oid")" &&
				if test "$input_mode" = packed
				then
					echo "$oid" |
					git pack-objects --window=0 \
						.git/objects/pack/pack >input.hash
				fi &&
				echo "$oid" || return 1
			done >oids &&
			if test "$input_mode" = packed
			then
				git prune-packed
			fi &&
			sort oids >expect &&

			# A one-blob output can have the same name as an input.
			for pass in 1 2
			do
				git -c pack.packSizeLimit=1m pack-aggregate --once \
					--min-loose=1 --min-packs=1 &&
				test "$output_packs" -eq "$(count_packs)" &&
				test "$output_packs" -eq "$(count_unoptimized)" &&
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

test_expect_success '--exclude-pack-file protects listed packs' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 6 >packs.txt &&
		first=$(sed -n 1p packs.txt) &&
		second=$(sed -n 2p packs.txt) &&
		cat >exclude.txt <<-EOF &&
		# pack exclusions may use either recognized suffix

		  $first.pack
		$second.idx
		pack-does-not-exist
		EOF
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=4 \
			--exclude-pack-file=exclude.txt &&
		test_path_is_file .git/objects/pack/${first}.pack &&
		test_path_is_file .git/objects/pack/${second}.pack &&
		# 2 excluded + 1 aggregate = 3 packs total.
		test 3 -eq "$(count_packs)" &&
		test 1 -eq "$(count_unoptimized)" &&
		git fsck
	)
'

test_expect_success '--exclude-loose-file protects listed loose objects' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_loose 5 >loose.txt &&
		first=$(sed -n 1p loose.txt) &&
		second=$(sed -n 2p loose.txt) &&
		cat >exclude.txt <<-EOF &&
		# loose exclusions are full object IDs

		  $first
		$second
		not-an-object-id
		EOF
		git pack-aggregate --once \
			--min-loose=1 --min-packs=1000 \
			--exclude-loose-file=exclude.txt &&
		test_path_is_file \
			".git/objects/$(test_oid_to_path "$first")" &&
		test_path_is_file \
			".git/objects/$(test_oid_to_path "$second")" &&
		test 2 -eq "$(count_loose)" &&
		test 1 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success 'MIDX packs are protected even when lookup is disabled' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >packs.txt &&
		sed -n "1,2s%$%.idx%p" packs.txt >midx-packs &&
		git multi-pack-index write --stdin-packs <midx-packs &&
		git -c core.multiPackIndex=false pack-aggregate --once \
			--min-loose=1000 --min-packs=3 &&
		git multi-pack-index verify &&
		# Two indexed packs remain alongside one aggregate.
		test 3 -eq "$(count_packs)" &&
		test 1 -eq "$(count_unoptimized)"
	)
'

test_expect_success 'packs with protective sidecars are skipped' '
	test_when_finished "rm -fr work" &&
	for sidecar in keep promisor mtimes bitmap
	do
		rm -fr work &&
		cp -R repo work &&
		(
			cd work &&
			build_n_packs 6 >packs.txt &&
			first=$(head -n 1 packs.txt) &&
			>.git/objects/pack/${first}.$sidecar &&
			git pack-aggregate --once \
				--min-loose=1000 --min-packs=4 &&
			test_path_is_file .git/objects/pack/${first}.pack &&
			test_path_is_file .git/objects/pack/${first}.$sidecar &&
			# 1 protected + 1 aggregate = 2 packs total.
			test 2 -eq "$(count_packs)" &&
			rm .git/objects/pack/${first}.$sidecar &&
			git fsck
		) || return 1
	done
'

test_expect_success '--max-objects skips packs above the object-count limit' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		big=$(build_big_pack 20) &&
		build_n_packs 5 >/dev/null &&
		test 6 -eq "$(count_packs)" &&
		# The big pack (20 objects) is above the cap; the five
		# single-object packs are below it and get rolled up.
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=1 --max-objects=10 &&
		test_path_is_file .git/objects/pack/${big}.pack &&
		# 1 preserved big pack + 1 aggregate = 2 packs total.
		test 2 -eq "$(count_packs)" &&
		test 1 -eq "$(count_unoptimized)" &&
		git fsck
	)
'

test_expect_success '--max-objects=0 disables the object-count limit' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		big=$(build_big_pack 20) &&
		build_n_packs 5 >/dev/null &&
		test 6 -eq "$(count_packs)" &&
		# With no cap, the big pack is aggregated along with the
		# small ones into a single pack.
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=1 --max-objects=0 &&
		test_path_is_missing .git/objects/pack/${big}.pack &&
		test 1 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success 'pack.aggregateMaxObjects supplies the default limit' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		big=$(build_big_pack 20) &&
		build_n_packs 5 >/dev/null &&
		test 6 -eq "$(count_packs)" &&
		git -c pack.aggregateMaxObjects=10 pack-aggregate --once \
			--min-loose=1000 --min-packs=1 &&
		test_path_is_file .git/objects/pack/${big}.pack &&
		test 2 -eq "$(count_packs)" &&
		# An explicit --max-objects still overrides the config.
		big2=$(build_big_pack 20) &&
		git -c pack.aggregateMaxObjects=10 pack-aggregate --once \
			--min-loose=1000 --min-packs=1 --max-objects=0 &&
		test_path_is_missing .git/objects/pack/${big2}.pack &&
		test 1 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success 'pack.aggregateMaxObjects rejects negative values' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		test_must_fail git -c pack.aggregateMaxObjects=-1 \
			pack-aggregate --once --min-packs=5 2>err &&
		test_grep "cannot be negative" err
	)
'

test_expect_success 'the automatic byte gate excludes nearly full packs' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		test-tool genrandom large 716800 >blob &&
		oid=$(git hash-object -w blob) &&
		echo "$oid" |
		git pack-objects --window=0 .git/objects/pack/pack >large.hash &&
		large=.git/objects/pack/pack-$(cat large.hash) &&
		git prune-packed &&
		build_n_packs 5 >/dev/null &&

		# The excluded pack does not count toward --min-packs.
		git -c pack.packSizeLimit=1m pack-aggregate --once \
			--min-packs=6 --max-objects=0 &&
		test 6 -eq "$(count_packs)" &&
		test 0 -eq "$(count_unoptimized)" &&
		git -c pack.packSizeLimit=1m -c pack.aggregateMaxInputPackSize=1 \
			pack-aggregate --once --min-packs=5 \
			--max-input-pack-size=0 --max-objects=0 &&
		test_path_is_file "$large.pack" &&
		test_path_is_missing "$large.unoptimized" &&
		test 2 -eq "$(count_packs)" &&
		test 1 -eq "$(count_unoptimized)" &&
		git fsck
	)
'

test_expect_success 'input byte cap uses config, with an inclusive CLI override' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		big=$(build_big_pack 20) &&
		size=$(wc -c <.git/objects/pack/$big.pack) &&
		build_n_packs 5 >/dev/null &&
		git -c pack.aggregateMaxInputPackSize=$((size - 1)) \
			pack-aggregate --once --min-packs=1 &&
		test_path_is_file .git/objects/pack/$big.pack &&
		test_path_is_missing .git/objects/pack/$big.unoptimized &&
		test 2 -eq "$(count_packs)" &&
		git -c pack.aggregateMaxInputPackSize=1 pack-aggregate --once \
			--min-packs=1 --max-input-pack-size="$size" &&
		test_path_is_missing .git/objects/pack/$big.pack &&
		test 1 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success 'zero input cap restores unlimited automatic selection' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		git -c pack.packSizeLimit=0 -c pack.aggregateMaxInputPackSize=1 \
			pack-aggregate --once --max-input-pack-size=0 &&
		test 1 -eq "$(count_packs)" &&
		git fsck
	)
'

for output_limit in 1m 1k
do
	test_expect_success "input byte cap respects the effective $output_limit output limit" '
		git -C repo -c pack.packSizeLimit=$output_limit pack-aggregate \
			--once --max-input-pack-size=512k &&
		test_must_fail git -C repo -c pack.packSizeLimit=$output_limit \
			pack-aggregate --once --max-input-pack-size=524289 2>err &&
		test_grep -F "half the effective pack.packSizeLimit" err &&
		test_must_fail git -C repo -c pack.packSizeLimit=$output_limit \
			-c pack.aggregateMaxInputPackSize=524289 \
			pack-aggregate --once 2>err &&
		test_grep -F "half the effective pack.packSizeLimit" err
	'
done

for value in -1 bogus 999999999999999999999999999999
do
	test_expect_success "input byte cap rejects $value" '
		test_must_fail git -C repo pack-aggregate --once \
			--max-input-pack-size=$value 2>err &&
		test_grep "max-input-pack-size" err &&
		test_must_fail git -C repo -c pack.aggregateMaxInputPackSize=$value \
			pack-aggregate --once 2>err &&
		test_grep "aggregatemaxinputpacksize" err
	'
done

test_expect_success '--max-loose-objects splits the cycle-start backlog' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_loose 10 >/dev/null &&
		git pack-aggregate --once --min-loose=1 --min-packs=1000 \
			--max-loose-objects=4 &&
		test 0 -eq "$(count_loose)" &&
		test 3 -eq "$(count_packs)" &&
		test 3 -eq "$(count_unoptimized)" &&
		git fsck
	)
'

test_expect_success '--min-loose can exceed --max-loose-objects' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_loose 6 >/dev/null &&
		git pack-aggregate --once --min-loose=5 --min-packs=1000 \
			--max-loose-objects=2 &&
		test 0 -eq "$(count_loose)" &&
		test 3 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success 'multiple loose batches avoid same-cycle reaggregation' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_loose 10 >/dev/null &&
		build_n_packs 5 >/dev/null &&
		git pack-aggregate --once --min-loose=1 --min-packs=1 \
			--max-loose-objects=4 &&
		# The five original packs are aggregated, while the three
		# loose-rollup packs remain separate until a later cycle.
		test 0 -eq "$(count_loose)" &&
		test 4 -eq "$(count_packs)" &&
		test 4 -eq "$(count_unoptimized)" &&
		git fsck
	)
'

test_expect_success 'loose objects newer than the cycle cutoff are deferred' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_loose 5 >oids &&
		deferred=$(tail -n 1 oids) &&
		deferred_path=.git/objects/$(test_oid_to_path "$deferred") &&
		test-tool chmtime +60 "$deferred_path" &&
		git pack-aggregate --once --min-loose=1 --min-packs=1000 \
			--max-loose-objects=2 &&
		test 1 -eq "$(count_loose)" &&
		test_path_is_file "$deferred_path" &&
		test-tool chmtime -60 "$deferred_path" &&
		git pack-aggregate --once --min-loose=1 --min-packs=1000 \
			--max-loose-objects=2 &&
		test 0 -eq "$(count_loose)" &&
		git fsck
	)
'

test_expect_success '--max-loose-objects=0 disables batch splitting' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_loose 10 >/dev/null &&
		git pack-aggregate --once --min-loose=1 --min-packs=1000 \
			--max-loose-objects=0 &&
		test 0 -eq "$(count_loose)" &&
		test 1 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success 'pack.aggregateMaxLooseObjects supplies the default limit' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_loose 10 >/dev/null &&
		git -c pack.aggregateMaxLooseObjects=4 pack-aggregate --once \
			--min-loose=1 --min-packs=1000 &&
		test 0 -eq "$(count_loose)" &&
		test 3 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success 'pack.aggregateMaxLooseObjects rejects negative values' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		test_must_fail git -c pack.aggregateMaxLooseObjects=-1 \
			pack-aggregate --once 2>err &&
		test_grep "cannot be negative" err
	)
'

test_expect_success '--max-packs splits candidates into several output packs' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 10 >/dev/null &&
		test 10 -eq "$(count_packs)" &&
		git cat-file --batch-all-objects --batch-check >before &&
		# 10 candidates, at most 4 per output pack: ceil(10/4)=3
		# batches of sizes 4, 4, 2 => 3 output packs, with no
		# objects lost across the batch boundaries.
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=1 --max-packs=4 &&
		test 3 -eq "$(count_packs)" &&
		test 3 -eq "$(count_unoptimized)" &&
		git cat-file --batch-all-objects --batch-check >after &&
		test_cmp before after &&
		git fsck
	)
'

test_expect_success '--max-packs above the candidate count yields one pack' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		test 5 -eq "$(count_packs)" &&
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=1 --max-packs=100 &&
		test 1 -eq "$(count_packs)" &&
		test 1 -eq "$(count_unoptimized)" &&
		git fsck
	)
'

test_expect_success '--max-packs=0 folds everything into one pack' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 10 >/dev/null &&
		test 10 -eq "$(count_packs)" &&
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=1 --max-packs=0 &&
		test 1 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success 'pack.aggregateMaxPacks supplies the default limit' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 10 >/dev/null &&
		test 10 -eq "$(count_packs)" &&
		git -c pack.aggregateMaxPacks=4 pack-aggregate --once \
			--min-loose=1000 --min-packs=1 &&
		test 3 -eq "$(count_packs)" &&
		# An explicit --max-packs=0 still overrides the config,
		# folding the current packs back into a single one.
		build_n_packs 6 >/dev/null &&
		test 9 -eq "$(count_packs)" &&
		git -c pack.aggregateMaxPacks=4 pack-aggregate --once \
			--min-loose=1000 --min-packs=1 --max-packs=0 &&
		test 1 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success 'pack.aggregateMaxPacks rejects negative values' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		test_must_fail git -c pack.aggregateMaxPacks=-1 \
			pack-aggregate --once --min-packs=5 2>err &&
		test_grep "cannot be negative" err
	)
'

test_expect_success 'aggregate re-rolls up .unoptimized packs' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=5 &&
		test 1 -eq "$(count_packs)" &&
		test 1 -eq "$(count_unoptimized)" &&
		# Now add more small packs and aggregate again.  The
		# previous .unoptimized pack should NOT be protected (we
		# want to keep rolling them up).
		build_n_packs 4 >/dev/null &&
		test 5 -eq "$(count_packs)" &&
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=5 &&
		test 1 -eq "$(count_packs)" &&
		test 1 -eq "$(count_unoptimized)" &&
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
		test 1 -eq "$(count_unoptimized)" &&
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
		test 1 -eq "$(count_unoptimized)" &&
		git fsck
	)
'

# ---- geometric .unoptimized demotion ----

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
					>"$pack_base.unoptimized"
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
			test 0 -eq "$(count_unoptimized)" &&
			git cat-file --batch-all-objects \
				--batch-check="%(objectname)" >after &&
			test_cmp before after &&
			git fsck
		)
	'
done

test_expect_success 'geometric repack demotes .unoptimized packs into rollup' '
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
		# Drop an .unoptimized marker on it (simulating an
		# aggregator output).
		>"${big%.pack}.unoptimized" &&
		test 1 -eq "$(count_unoptimized)" &&
		# Add small packs that are individually far smaller
		# than the big one; ordinarily the big pack would be
		# kept above the geometric split.
		make_unique_pack a &&
		make_unique_pack b &&
		make_unique_pack c &&
		test 4 -eq "$(count_packs)" &&
		# With the .unoptimized demotion, the big pack is rolled
		# up despite being above the natural split, and the
		# resulting pack carries no .unoptimized marker.
		git repack -d --geometric=2 &&
		test 1 -eq "$(count_packs)" &&
		test 0 -eq "$(count_unoptimized)" &&
		git fsck
	)
'

test_expect_success 'geometric repack leaves unmarked packs above the split alone' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 8 >/dev/null &&
		git repack -d --geometric=2 &&
		test 1 -eq "$(count_packs)" &&
		# Add small packs; without any .unoptimized marker the
		# big pack should be preserved above the split.
		make_unique_pack a &&
		make_unique_pack b &&
		make_unique_pack c &&
		test 4 -eq "$(count_packs)" &&
		git repack -d --geometric=2 &&
		# 1 kept big pack + 1 newly-aggregated rollup = 2.
		test 2 -eq "$(count_packs)" &&
		test 0 -eq "$(count_unoptimized)" &&
		git fsck
	)
'

# ---- repack integration ----

test_expect_success 'repack does not aggregate by default' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 3 >/dev/null &&
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git repack -d --geometric=2 &&
		test_grep ! "\"pack-aggregate\"" trace.txt
	)
'

test_expect_success 'repack --aggregate-once runs pack-aggregate once' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 3 >/dev/null &&
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git repack -d --geometric=2 --aggregate-once &&
		test_grep "\"argv\":.*\"pack-aggregate\",\"--once\"" trace.txt &&
		test_grep ! "\"argv\":.*\"pack-aggregate\",\"--loop\"" trace.txt &&
		git fsck
	)
'

test_expect_success 'repack --aggregate-once honors repeated --keep-pack' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 7 >packs.txt &&
		first=$(head -n 1 packs.txt) &&
		second=$(sed -n 2p packs.txt) &&
		git cat-file --batch-all-objects \
			--batch-check="create refs/tags/%(objectname) %(objectname)" \
			>refs &&
		git update-ref --stdin <refs &&
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git repack -ad --aggregate-once \
				--keep-pack="$first.pack" \
				--keep-pack="$second.pack" &&
		test_grep "\"argv\":.*\"pack-objects\".*\"--mark-unoptimized\"" \
			trace.txt &&
		for name in "$first" "$second"
		do
			test_path_is_file ".git/objects/pack/$name.pack" &&
			test_path_is_file ".git/objects/pack/$name.idx" ||
			return 1
		done &&
		test 3 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success 'repack stops when --aggregate-once fails' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 3 >/dev/null &&
		test_env GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			test_must_fail git -c pack.aggregateMaxObjects=-1 \
				repack -d --geometric=2 \
				--aggregate-once 2>err &&
		test_grep "pack.aggregateMaxObjects cannot be negative" err &&
		test_grep "git pack-aggregate --once failed" err &&
		test_grep ! "\"argv\":.*\"pack-objects\"" trace.txt
	)
'

test_expect_success TTY 'quiet repack suppresses one-shot progress on a terminal' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		test_terminal git repack -q --aggregate-once >out 2>err &&
		test_must_be_empty err
	)
'

test_expect_success 'repack --aggregate-once handles an existing MIDX' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		git multi-pack-index write &&
		git repack -d --geometric=2 --aggregate-once --write-midx &&
		git fsck
	)
'

test_expect_success 'repack --aggregate-loop spawns and reaps pack-aggregate' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git repack -d --geometric=2 --aggregate-loop &&
		test_grep "\"argv\":.*\"pack-aggregate\",\"--loop\"" trace.txt &&
		test_grep ! "\"argv\":.*\"pack-aggregate\",\"--once\"" trace.txt &&
		# Tempdir should be cleaned up.
		test -z "$(ls .git/objects | grep pack-aggregate)" &&
		git fsck
	)
'

wait_for_file () {
	local attempts=0 &&
	while test ! -f "$1" && test "$attempts" -lt 100
	do
		sleep 0.1 &&
		attempts=$((attempts + 1)) || return 1
	done &&
	test_path_is_file "$1"
}

test_aggregate_arrivals () {
	local mode="$1" &&
	repack_pid= &&
	test_when_finished '
		if test -n "$repack_pid"
		then
			>work/main-gate
			>work/aggregate-gate
			wait "$repack_pid" 2>/dev/null || :
		fi
		rm -fr work
	' &&
	cp -R repo work &&
	(cd work && build_n_packs 5 initial >/dev/null) &&
	{
		GIT_TEST_PACK_OBJECTS_WAIT_AFTER_INPUT="$PWD/work/main-gate" \
		GIT_TEST_PACK_OBJECTS_WAIT_BEFORE_WRITE="$PWD/work/aggregate-gate" \
		GIT_TEST_PACK_AGGREGATE_INTERVAL=1 \
			git -C work repack -d --geometric=2 \
				--aggregate-loop 2>work/err &
		repack_pid=$!
	} &&
	wait_for_file work/main-gate.waiting &&
	(cd work && build_n_packs 5 concurrent >/dev/null) &&
	wait_for_file work/aggregate-gate.waiting &&
	if test "$mode" = complete
	then
		>work/aggregate-gate &&
		i=0 &&
		while test "$(cd work && count_unoptimized)" -eq 0 &&
			test $i -lt 100
		do
			sleep 0.1 &&
			i=$((i + 1)) || return 1
		done &&
		test 1 -eq "$(cd work && count_unoptimized)"
	fi &&
	>work/main-gate &&
	wait "$repack_pid" &&
	repack_pid= &&
	test_grep ! "error:" work/err &&
	test -z "$(ls work/.git/objects | grep pack-aggregate)" &&
	git -C work fsck
}

test_expect_success 'repack --aggregate-loop aggregates new packs' '
	test_aggregate_arrivals complete
'

test_expect_success !MINGW 'repack can stop active aggregation without failing' '
	test_aggregate_arrivals cancel
'

test_expect_success 'repack warns about a failed background aggregation' '
	repack_pid= &&
	test_when_finished "
		if test -n \"\$repack_pid\"
		then
			>work/main-gate
			if test -d work/failure
			then
				>work/failure/gate
			fi
			wait \"\$repack_pid\" 2>/dev/null || :
		fi
		rm -fr work
	" &&
	cp -R repo work &&
	mkdir work/failure &&
	(cd work && build_n_packs 5 initial >/dev/null) &&
	{
		test_env \
			GIT_TEST_PACK_OBJECTS_WAIT_AFTER_INPUT="$PWD/work/main-gate" \
			GIT_TEST_PACK_OBJECTS_WAIT_BEFORE_WRITE="$PWD/work/failure/gate" \
			GIT_TEST_PACK_AGGREGATE_INTERVAL=1 \
			git -C work repack -d --geometric=2 \
				--aggregate-loop 2>work/err &
		repack_pid=$!
	} &&
	wait_for_file work/main-gate.waiting &&
	(cd work && build_n_packs 5 concurrent >/dev/null) &&
	wait_for_file work/failure/gate.waiting &&
	mv work/failure work/failure-saved &&
	>work/failure &&
	i=0 &&
	while ! grep "pack-objects failed during pack aggregation" work/err &&
		test $i -lt 100
	do
		sleep 0.1 &&
		i=$((i + 1)) || return 1
	done &&
	test_grep "pack-objects failed during pack aggregation" work/err &&
	test_grep "could not access.*failure/gate" work/err &&
	>work/main-gate &&
	wait "$repack_pid" &&
	repack_pid= &&
	test_grep "warning: git pack-aggregate --loop failed" work/err
'

test_expect_success 'repack can enable both aggregation modes' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 3 >/dev/null &&
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git repack -d --geometric=2 \
				--aggregate-once --aggregate-loop &&
		test_grep "\"argv\":.*\"pack-aggregate\",\"--once\"" trace.txt &&
		test_grep "\"argv\":.*\"pack-aggregate\",\"--loop\"" trace.txt
	)
'

test_expect_success 'repack aggregation config enables both modes' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 3 >/dev/null &&
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git -c repack.aggregateOnce=true \
			-c repack.aggregateLoop=true \
			repack -d --geometric=2 &&
		test_grep "\"argv\":.*\"pack-aggregate\",\"--once\"" trace.txt &&
		test_grep "\"argv\":.*\"pack-aggregate\",\"--loop\"" trace.txt
	)
'

test_expect_success 'CLI can disable configured aggregation modes' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 3 >/dev/null &&
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git -c repack.aggregateOnce=true \
			-c repack.aggregateLoop=true \
			repack -d --geometric=2 \
				--no-aggregate-once --no-aggregate-loop &&
		test_grep ! "\"pack-aggregate\"" trace.txt
	)
'

# ---- pack-aggregate lifecycle / .keep marker coverage ----

test_expect_success PERL 'pack-aggregate survives through MIDX bitmap write' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		test_commit base &&
		build_n_packs 5 >/dev/null &&
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git repack -d --geometric=2 --aggregate-loop \
				--write-midx --write-bitmap-index &&
		test_path_is_file \
			.git/objects/pack/multi-pack-index-*.bitmap &&
		git rev-list --test-bitmap HEAD &&
		# Match child start and exit events by session ID.
		# Observe the children exiting, not the parent reaping them.
		perl -ne '\''
			my ($sid)  = /"sid":"([^"]+)"/ or next;
			my ($time) = /"time":"([^"]+)"/;
			if (/"event":"(?:start|child_start)"/) {
				# Match the subcommand, not a path containing
				# pack-aggregate.
				my ($argv) = /"argv":\["[^"]*","([^"]+)"/;
				my $kind = defined($argv) &&
					$argv eq "multi-pack-index" ? "midx" :
					defined($argv) && $argv eq "pack-aggregate" ?
					"agg" : undef;
				next unless $kind;
				if (/"event":"child_start"/) {
					my ($id) = /"child_id":([0-9]+)/ or next;
					$child{"$sid:$id"} = $kind;
				} else {
					$kind{$sid} = $kind;
				}
			} elsif (/"event":"exit"/ && $kind{$sid}) {
				$exit{$kind{$sid}} = $time
					unless defined($exit{$kind{$sid}});
			} elsif (/"event":"child_exit"/) {
				my ($id) = /"child_id":([0-9]+)/ or next;
				my $kind = $child{"$sid:$id"} or next;
				$reaped{$kind} = $time
					unless defined($reaped{$kind});
			}
			END {
				die "missing midx exit\n" unless $exit{midx};
				die "missing agg exit\n"  unless $exit{agg};
				die "ordering wrong: agg=$exit{agg} midx=$exit{midx}\n"
					unless $exit{agg} gt $exit{midx};
				for my $kind ("midx", "agg") {
					die "missing $kind reap\n" unless $reaped{$kind};
					die "reaped before exit: $kind\n"
						unless $reaped{$kind} ge $exit{$kind};
				}
			}
		'\'' trace.txt
	)
'

test_expect_success '--aggregate-loop preserves pre-existing user .keep files' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >packs.txt &&
		first=$(head -n 1 packs.txt) &&
		# A user-created .keep with no marker content must
		# survive a repack --aggregate-loop untouched.
		echo "I am a user keep file" >expect &&
		cp expect .git/objects/pack/${first}.keep &&
		git repack -d --geometric=2 --aggregate-loop &&
		test_path_is_file .git/objects/pack/${first}.keep &&
		test_path_is_file .git/objects/pack/${first}.pack &&
		test_cmp expect .git/objects/pack/${first}.keep
	)
'

test_expect_success '--aggregate-loop cleans up its own .keep markers' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		git repack -d --geometric=2 --aggregate-loop &&
		# No marker .keep files should remain.  Any .keep that
		# survives must NOT carry our marker prefix.
		for f in .git/objects/pack/*.keep
		do
			test -e "$f" || continue
			test_grep ! "^git-repack-aggregate-temporary " "$f" \
				|| return 1
		done
	)
'

test_expect_success 'startup cleans up stale .keep markers from dead pids' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 3 >packs.txt &&
		# Spawn a short-lived subshell that writes its own pid
		# to a file, then exits.  That pid is guaranteed dead
		# by the time we read it.
		sh -c "echo \$\$" >dead_pid.txt &&
		dead_pid=$(cat dead_pid.txt) &&
		test -n "$dead_pid" &&
		first=$(head -n 1 packs.txt) &&
		stale=.git/objects/pack/${first}.keep &&
		printf "git-repack-aggregate-temporary pid=%d\n" \
			"$dead_pid" >"$stale" &&
		test_path_is_file "$stale" &&
		git repack -d --geometric=2 --aggregate-loop &&
		# The stale marker should have been cleaned at startup;
		# even if its pack was preserved by geometric repack,
		# the .keep should not still carry the dead-pid marker.
		if test -e "$stale"
		then
			test_grep ! "^git-repack-aggregate-temporary " \
				"$stale"
		fi
	)
'

test_expect_success 'startup leaves live-pid .keep markers alone' '
	test_when_finished "
		>live.stop
		wait \$pid_job 2>/dev/null || :
		rm -fr work live.stop live_pid.txt
	" &&
	cp -R repo work &&
	{
		test-tool run-command print-pid-and-wait live.stop \
			>live_pid.txt &
		pid_job=$!
	} &&
	i=0 &&
	while test ! -s live_pid.txt && test $i -lt 100
	do
		sleep 0.1 &&
		i=$((i + 1)) || return 1
	done &&
	test -s live_pid.txt &&
	live_pid=$(cat live_pid.txt) &&
	(
		cd work &&
		build_n_packs 3 >packs.txt &&
		first=$(head -n 1 packs.txt) &&
		live=.git/objects/pack/${first}.keep &&
		printf "git-repack-aggregate-temporary pid=%d\n" \
			"$live_pid" >"$live" &&
		test_path_is_file "$live" &&
		git repack -d --geometric=2 --aggregate-loop &&
		test_path_is_file "$live" &&
		test_grep "pid=${live_pid}" "$live"
	) &&
	>live.stop &&
	wait "$pid_job"
'

test_expect_success 'pack-aggregate ignores in-flight temporary packs' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >packs.txt &&
		# Completed temporary packs are visible to the object-store scan,
		# but must not be consumed before their owner installs them.
		for prefix in .tmp-1234- tmp_
		do
			read victim &&
			for e in pack idx rev
			do
				from=.git/objects/pack/${victim}.$e &&
				if test -f "$from"
				then
					mv "$from" \
						.git/objects/pack/${prefix}${victim}.$e ||
						return 1
				fi
			done || return 1
		done <packs.txt &&
		git pack-aggregate --once --min-loose=1000 --min-packs=3 &&
		for prefix in .tmp-1234- tmp_
		do
			test_path_is_file .git/objects/pack/${prefix}*.pack &&
			test_path_is_file .git/objects/pack/${prefix}*.idx ||
				return 1
		done &&
		# Only the three permanent packs were rolled into one.
		test 1 -eq "$(count_packs)" &&
		git fsck
	)
'

test_expect_success !MINGW 'loop exits when signaled' '
	aggregate_pid= &&
	test_when_finished "
		if test -n \"\$aggregate_pid\"
		then
			kill \"\$aggregate_pid\" 2>/dev/null || :
			wait \"\$aggregate_pid\" 2>/dev/null || :
		fi
		rm -fr work
	" &&
	cp -R repo work &&
	>work/trace.txt &&
	{
		GIT_TRACE2_EVENT="$PWD/work/trace.txt" \
			git -C work pack-aggregate --loop --interval=60 &
		aggregate_pid=$!
	} &&
	i=0 &&
	while ! grep "\"label\":\"cycle\"" work/trace.txt &&
		test $i -lt 100
	do
		sleep 0.1 &&
		i=$((i + 1)) || return 1
	done &&
	test_grep "\"label\":\"cycle\"" work/trace.txt &&
	kill "$aggregate_pid" &&
	wait "$aggregate_pid" &&
	aggregate_pid=
'

test_done
