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

test_done
