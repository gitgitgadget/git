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

test_expect_success '--once requires either --once or --loop' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		test_must_fail git pack-aggregate 2>err &&
		grep "exactly one of --once or --loop" err
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

test_expect_success '--exclude-pack-file protects listed packs' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 6 >packs.txt &&
		head -n 2 packs.txt >exclude.txt &&
		git pack-aggregate --once \
			--min-loose=1000 --min-packs=4 \
			--exclude-pack-file=exclude.txt &&
		while read name
		do
			test_path_is_file \
				.git/objects/pack/${name}.pack || return 1
		done <exclude.txt &&
		# 2 excluded + 1 aggregate = 3 packs total.
		test 3 -eq "$(count_packs)" &&
		test 1 -eq "$(count_baddeltas)" &&
		git fsck
	)
'

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

test_expect_success '--exclude-loose-file protects listed loose objects' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_loose 5 >loose.txt &&
		head -n 2 loose.txt >exclude.txt &&
		git pack-aggregate --once \
			--min-loose=1 --min-packs=1000 \
			--exclude-loose-file=exclude.txt &&
		while read oid
		do
			dir=$(echo "$oid" | cut -c1-2) &&
			rest=$(echo "$oid" | cut -c3-) &&
			test_path_is_file \
				.git/objects/${dir}/${rest} || return 1
		done <exclude.txt &&
		test 2 -eq "$(count_loose)" &&
		test 1 -eq "$(count_packs)" &&
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


# ---- repack integration ----

test_expect_success 'repack without --aggregate does not spawn aggregator' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 3 >/dev/null &&
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git repack -d --geometric=2 &&
		! grep "\"pack-aggregate\"" trace.txt
	)
'

test_expect_success 'repack --aggregate spawns and reaps pack-aggregate' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		GIT_TEST_PACK_AGGREGATE_INTERVAL=1 \
		GIT_TEST_PACK_AGGREGATE_MIN_PACKS=10 \
		GIT_TEST_PACK_AGGREGATE_MIN_LOOSE=10 \
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git repack -d --geometric=2 --aggregate &&
		grep "\"pack-aggregate\"" trace.txt &&
		# Tempdir should be cleaned up.
		test -z "$(ls .git/objects | grep pack-aggregate)" &&
		git fsck
	)
'

test_expect_success 'repack.aggregate config enables aggregator' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 3 >/dev/null &&
		GIT_TEST_PACK_AGGREGATE_INTERVAL=1 \
		GIT_TEST_PACK_AGGREGATE_MIN_PACKS=10 \
		GIT_TEST_PACK_AGGREGATE_MIN_LOOSE=10 \
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git -c repack.aggregate=true \
			repack -d --geometric=2 &&
		grep "\"pack-aggregate\"" trace.txt
	)
'

test_expect_success 'CLI --no-aggregate overrides repack.aggregate=true' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 3 >/dev/null &&
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git -c repack.aggregate=true \
			repack -d --geometric=2 --no-aggregate &&
		! grep "\"pack-aggregate\"" trace.txt
	)
'

# ---- sidecar lifecycle / .keep marker coverage ----

test_expect_success PERL 'sidecar survives through write_midx_included_packs' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		GIT_TEST_PACK_AGGREGATE_INTERVAL=1 \
		GIT_TEST_PACK_AGGREGATE_MIN_PACKS=10 \
		GIT_TEST_PACK_AGGREGATE_MIN_LOOSE=10 \
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git repack -d --geometric=2 --aggregate \
				--write-midx &&
		# Each child subprocess writes its own trace2 "start"
		# (with argv) and "exit" (with timestamp) events under
		# its own session id.  Find the exit timestamp of the
		# multi-pack-index child and the exit timestamp of the
		# pack-aggregate child by joining via session id.
		# We cannot rely on the parents '\''child_exit'\'' event
		# for pack-aggregate because the sidecar teardown uses
		# kill+waitpid directly rather than finish_command().
		perl -ne '\''
			my ($sid)  = /"sid":"([^"]+)"/ or next;
			my ($time) = /"time":"([^"]+)"/;
			if (/"event":"start"/) {
				# Look only at the second argv element (the
				# subcommand), not any longer argument that
				# happens to contain "pack-aggregate" as a
				# substring (e.g. paths under our tempdir).
				my ($argv) = /"argv":\["[^"]*","([^"]+)"/;
				if (defined($argv) && $argv eq "multi-pack-index") {
					$kind{$sid} = "midx";
				} elsif (defined($argv) && $argv eq "pack-aggregate") {
					$kind{$sid} = "agg";
				}
			} elsif (/"event":"exit"/ && $kind{$sid}) {
				$exit{$kind{$sid}} //= $time;
			}
			END {
				die "missing midx exit\n" unless $exit{midx};
				die "missing agg exit\n"  unless $exit{agg};
				die "ordering wrong: agg=$exit{agg} midx=$exit{midx}\n"
					unless $exit{agg} gt $exit{midx};
			}
		'\'' trace.txt
	)
'

test_expect_success '--aggregate preserves pre-existing user .keep files' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >packs.txt &&
		first=$(head -n 1 packs.txt) &&
		# A user-created .keep with no marker content must
		# survive a repack --aggregate untouched.
		echo "I am a user keep file" \
			>.git/objects/pack/${first}.keep &&
		GIT_TEST_PACK_AGGREGATE_INTERVAL=1 \
		GIT_TEST_PACK_AGGREGATE_MIN_PACKS=10 \
		GIT_TEST_PACK_AGGREGATE_MIN_LOOSE=10 \
			git repack -d --geometric=2 --aggregate &&
		test_path_is_file .git/objects/pack/${first}.keep &&
		test_path_is_file .git/objects/pack/${first}.pack &&
		# Content untouched.
		echo "I am a user keep file" >expect &&
		test_cmp expect .git/objects/pack/${first}.keep
	)
'

test_expect_success '--aggregate cleans up its own .keep markers' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	(
		cd work &&
		build_n_packs 5 >/dev/null &&
		GIT_TEST_PACK_AGGREGATE_INTERVAL=1 \
		GIT_TEST_PACK_AGGREGATE_MIN_PACKS=10 \
		GIT_TEST_PACK_AGGREGATE_MIN_LOOSE=10 \
			git repack -d --geometric=2 --aggregate &&
		# No marker .keep files should remain.  Any .keep that
		# survives must NOT carry our marker prefix.
		for f in .git/objects/pack/*.keep
		do
			test -e "$f" || continue
			! grep -q "^git-repack-aggregate-temporary " "$f" \
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
		GIT_TEST_PACK_AGGREGATE_INTERVAL=1 \
		GIT_TEST_PACK_AGGREGATE_MIN_PACKS=10 \
		GIT_TEST_PACK_AGGREGATE_MIN_LOOSE=10 \
			git repack -d --geometric=2 --aggregate &&
		# The stale marker should have been cleaned at startup;
		# even if its pack was preserved by geometric repack,
		# the .keep should not still carry the dead-pid marker.
		if test -e "$stale"
		then
			! grep -q "^git-repack-aggregate-temporary " \
				"$stale"
		fi
	)
'

test_expect_success !MINGW 'startup leaves live-pid .keep markers alone' '
	test_when_finished "rm -fr work" &&
	cp -R repo work &&
	# Use the test-runner shell pid, which is reliably alive
	# for the duration of this test.  PID 1 (init) would also
	# work since our code treats EPERM as "alive".
	#
	# !MINGW: on Windows, bash $$ is a virtualized MSYS pid that
	# does not correspond to a process kill(pid, 0) can see, so
	# our liveness check would treat it as dead and the marker
	# would be removed.  Skip this test there.
	live_pid=$$ &&
	(
		cd work &&
		build_n_packs 3 >packs.txt &&
		first=$(head -n 1 packs.txt) &&
		live=.git/objects/pack/${first}.keep &&
		printf "git-repack-aggregate-temporary pid=%d\n" \
			"$live_pid" >"$live" &&
		test_path_is_file "$live" &&
		GIT_TEST_PACK_AGGREGATE_INTERVAL=1 \
		GIT_TEST_PACK_AGGREGATE_MIN_PACKS=10 \
		GIT_TEST_PACK_AGGREGATE_MIN_LOOSE=10 \
			git repack -d --geometric=2 --aggregate &&
		test_path_is_file "$live" &&
		grep -q "pid=${live_pid}" "$live"
	)
'

# ---- geometric .baddeltas demotion ----

# Helper for the geometric tests: pack a single unique blob into its
# own packfile.  The `tag` parameter must differ between callers so
# that the blob contents do not collide with one another (otherwise
# `git pack-objects --stdin-packs` would see the "rollup" objects
# already present in the kept pack and emit nothing).
make_unique_pack () {
	tag=$1 &&
	blob=$(echo "unique-${tag}-$$" | git hash-object -w --stdin) &&
	echo "$blob" |
	git pack-objects --window=0 .git/objects/pack/pack >/dev/null &&
	git prune-packed
}

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
