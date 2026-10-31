#!/bin/sh

test_description='push from/to a shallow clone'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

commit() {
	echo "$1" >tracked &&
	git add tracked &&
	git commit -m "$1"
}

test_expect_success 'setup' '
	git config --global transfer.fsckObjects true &&
	commit 1 &&
	commit 2 &&
	commit 3 &&
	commit 4 &&
	git clone . full &&
	(
	git init full-abc &&
	cd full-abc &&
	commit a &&
	commit b &&
	commit c
	) &&
	git clone --no-local --depth=2 .git shallow &&
	git --git-dir=shallow/.git log --format=%s >actual &&
	cat <<EOF >expect &&
4
3
EOF
	test_cmp expect actual &&
	git clone --no-local --depth=2 full-abc/.git shallow2 &&
	git --git-dir=shallow2/.git log --format=%s >actual &&
	cat <<EOF >expect &&
c
b
EOF
	test_cmp expect actual
'

test_expect_success 'push from shallow clone' '
	(
	cd shallow &&
	commit 5 &&
	git push ../.git +main:refs/remotes/shallow/main
	) &&
	git log --format=%s shallow/main >actual &&
	git fsck &&
	cat <<EOF >expect &&
5
4
3
2
1
EOF
	test_cmp expect actual
'

test_expect_success 'push from shallow clone, with grafted roots' '
	(
	cd shallow2 &&
	test_must_fail git -c push.shallowExcludeBoundary=false \
		push ../.git +main:refs/remotes/shallow2/main 2>err &&
	test_grep "shallow2/main.*shallow update not allowed" err
	) &&
	test_must_fail git rev-parse shallow2/main &&
	git fsck
'

test_expect_success 'add new shallow root with receive.updateshallow on' '
	test_config receive.shallowupdate true &&
	(
	cd shallow2 &&
	git -c push.shallowExcludeBoundary=false \
		push ../.git +main:refs/remotes/shallow2/main
	) &&
	git log --format=%s shallow2/main >actual &&
	git fsck &&
	cat <<EOF >expect &&
c
b
EOF
	test_cmp expect actual
'

test_expect_success 'push from shallow to shallow' '
	(
	cd shallow &&
	git --git-dir=../shallow2/.git config receive.shallowupdate true &&
	git -c push.shallowExcludeBoundary=false \
		push ../shallow2/.git +main:refs/remotes/shallow/main &&
	git --git-dir=../shallow2/.git config receive.shallowupdate false
	) &&
	(
	cd shallow2 &&
	git log --format=%s shallow/main >actual &&
	git fsck &&
	cat <<EOF >expect &&
5
4
3
EOF
	test_cmp expect actual
	)
'

test_expect_success 'push from full to shallow' '
	! git --git-dir=shallow2/.git cat-file blob $(echo 1|git hash-object --stdin) &&
	commit 1 &&
	git push shallow2/.git +main:refs/remotes/top/main &&
	(
	cd shallow2 &&
	git log --format=%s top/main >actual &&
	git fsck &&
	cat <<EOF >expect &&
1
4
3
EOF
	test_cmp expect actual &&
	git cat-file blob $(echo 1|git hash-object --stdin) >/dev/null
	)
'

test_expect_success 'push new commit from shallow clone has correct object count' '
	git init origin &&
	test_commit -C origin a &&
	test_commit -C origin b &&

	git clone --depth=1 "file://$(pwd)/origin" client &&
	git -C client checkout -b topic &&
	git -C client commit --allow-empty -m "empty" &&
	GIT_PROGRESS_DELAY=0 git -C client push --progress origin topic 2>err &&
	test_grep "Enumerating objects: 1, done." err
'

test_expect_success 'push new commit from shallow clone has good deltas' '
	git init base &&
	test_seq 1 999 >base/a &&
	test_commit -C base initial &&
	git -C base add a &&
	git -C base commit -m "big a" &&

	git clone --depth=1 "file://$(pwd)/base" deltas &&
	git -C deltas checkout -b deltas &&
	test_seq 1 1000 >deltas/a &&
	git -C deltas commit -a -m "bigger a" &&
	GIT_PROGRESS_DELAY=0 git -C deltas push --progress origin deltas 2>err &&

	test_grep "Enumerating objects: 5, done" err &&

	# If the delta base is found, then this message uses "bytes".
	# If the delta base is not found, then this message uses "KiB".
	test_grep "Writing objects: .* bytes" err &&

	git -C deltas commit --amend -m "changed message" &&
	GIT_TRACE2_EVENT="$(pwd)/config-push.txt" \
	GIT_PROGRESS_DELAY=0 git -C deltas -c pack.usePathWalk=true \
		push --progress -f origin deltas 2>err &&

	test_grep "Enumerating objects: 1, done" err &&
	test_region pack-objects path-walk config-push.txt
'

test_expect_success 'incomplete shallow push rejects without disconnecting' '
	git init raw-origin &&
	git -C raw-origin checkout -b A &&
	test_commit -C raw-origin --no-tag has-shared sh shared &&
	test_commit -C raw-origin --no-tag A1 &&
	A1=$(git -C raw-origin rev-parse HEAD) &&
	git -C raw-origin switch --orphan B &&
	test_commit -C raw-origin --no-tag B0 &&
	test_commit -C raw-origin --no-tag B1 &&
	B1=$(git -C raw-origin rev-parse HEAD) &&

	git init --bare raw-receiver.git &&
	git -C raw-receiver.git config receive.fsckObjects false &&
	git -C raw-origin push ../raw-receiver.git \
		B:refs/heads/B B:refs/heads/A &&

	git -C raw-origin checkout A &&
	test_commit -C raw-origin --no-tag cX &&
	cX=$(git -C raw-origin rev-parse HEAD) &&
	git -C raw-origin checkout -b topic B &&
	test_commit -C raw-origin --no-tag reintroduce sh shared &&
	topic=$(git -C raw-origin rev-parse HEAD) &&

	# Declare A1 and B1 as shallow, but omit them and their objects from
	# the pack. This mimics an incomplete shallow push without relying on
	# send-pack to create one.
	{
		printf "shallow %s\nshallow %s\n" "$A1" "$B1" |
		packetize &&
		printf "%s %s refs/heads/A\0report-status object-format=%s\n" \
			"$B1" "$cX" "$(test_oid algo)" |
		packetize_raw &&
		printf "%s %s refs/heads/topic\n" "$ZERO_OID" "$topic" |
		packetize &&
		printf 0000 &&
		printf "%s\n%s\n^%s\n^%s\n" "$cX" "$topic" "$A1" "$B1" |
		git -C raw-origin pack-objects --stdout --revs
	} >input &&

	git receive-pack raw-receiver.git <input >out 2>err &&
	depacketize <out >out.raw &&
	test_grep "ng refs/heads/A missing necessary objects" out.raw &&
	test_grep "ng refs/heads/topic missing necessary objects" out.raw &&
	test_grep ! "unable to parse commit" err
'

test_expect_success 'shallow boundary exclusion avoids sending the full tree' '
	git init adv-origin &&
	# The shallow grafts are intentionally untagged so that no
	# advertised ref points at them.
	test_commit --no-tag -C adv-origin a &&
	test_commit --no-tag -C adv-origin b &&

	git clone --depth=1 "file://$(pwd)/adv-origin" adv-client &&

	# The remote branch advances past the history we have, so its
	# advertised tip is something we cannot use as a negative tip;
	# only the shallow graft lets us exclude the full tree.
	test_commit --no-tag -C adv-origin c &&

	git -C adv-client checkout -b topic &&
	test_commit --no-tag -C adv-client new &&
	GIT_PROGRESS_DELAY=0 git -C adv-client \
		push --progress origin topic 2>err &&

	# Only the new commit, its tree, and the new blob are sent; sending
	# the full tree is avoided by excluding the shallow graft.
	test_grep "Enumerating objects: 4, done." err
'

test_expect_success 'push.shallowExcludeBoundary=false sends full tree' '
	git init adv-origin2 &&
	test_commit --no-tag -C adv-origin2 a &&
	test_commit --no-tag -C adv-origin2 b &&

	git clone --depth=1 "file://$(pwd)/adv-origin2" adv-client2 &&
	test_commit --no-tag -C adv-origin2 c &&

	git -C adv-client2 checkout -b topic &&
	test_commit --no-tag -C adv-client2 new &&
	GIT_PROGRESS_DELAY=0 git -C adv-client2 \
		-c push.shallowExcludeBoundary=false \
		push --progress origin topic 2>err &&

	# With the optimization disabled and no advertised ref pointing at
	# the shallow graft, the full snapshot down to the shallow graft is
	# resent, including its full tree.
	test_grep "Enumerating objects: 7, done." err
'

test_expect_success 'push.shallowExcludeBoundary=abort refuses when a graft is reached' '
	git init adv-origin3 &&
	test_commit --no-tag -C adv-origin3 a &&
	test_commit --no-tag -C adv-origin3 b &&

	git clone --depth=1 "file://$(pwd)/adv-origin3" adv-client3 &&

	# The remote branch advances past the history we have, so its
	# advertised tip cannot bound the walk; only the shallow graft could,
	# which is exactly what "abort" refuses to rely on.
	test_commit --no-tag -C adv-origin3 c &&

	git -C adv-client3 checkout -b topic &&
	test_commit --no-tag -C adv-client3 new &&

	test_must_fail git -C adv-client3 \
		-c push.shallowExcludeBoundary=abort push origin topic 2>err &&
	test_grep "push.shallowExcludeBoundary" err &&

	# The receiver must be left untouched: no ref was created.
	test_must_fail git -C adv-origin3 rev-parse --verify refs/heads/topic
'

# A and B are unrelated shallow histories. The receiver has B1 under both
# names, but lacks the "shared" blob from A1. The client adds cX atop A1 and
# reintroduces "shared" on a topic atop B1. Pushing A and topic together
# rejects A as a non-fast-forward, but A still participates in pack selection.
# Its A1 boundary must not exclude the blob needed by topic.
test_expect_success 'shallow push does not over-exclude for an accepted ref via a rejected one' '
	git init tworoot-origin &&
	git -C tworoot-origin checkout -b A &&
	test_commit -C tworoot-origin --no-tag has-shared sh shared &&
	test_commit -C tworoot-origin --no-tag A1 &&
	git -C tworoot-origin switch --orphan B &&
	test_commit -C tworoot-origin --no-tag B0 &&
	test_commit -C tworoot-origin --no-tag B1 &&

	git init --bare tworoot-receiver.git &&
	git -C tworoot-origin push "file://$(pwd)/tworoot-receiver.git" \
		B:refs/heads/B B:refs/heads/A &&

	git clone --depth=1 --no-single-branch \
		"file://$(pwd)/tworoot-origin" tworoot-client &&

	git -C tworoot-client checkout A &&
	test_commit -C tworoot-client --no-tag cX &&

	git -C tworoot-client checkout -b topic B &&
	test_commit -C tworoot-client --no-tag reintroduce sh shared &&

	test_must_fail git -C tworoot-client \
		-c push.shallowExcludeBoundary=true push \
		"file://$(pwd)/tworoot-receiver.git" A topic &&
	git --git-dir=tworoot-receiver.git rev-parse --verify topic
'

# A receive.shallowUpdate receiver needs the boundary snapshot to adopt a new
# shallow root, so omission must reject rather than create a broken ref.
test_expect_success 'push to a shallowUpdate receiver rejects a rootless snapshot' '
	git init seed-origin &&
	test_commit -C seed-origin s1 &&
	test_commit -C seed-origin s2 &&
	test_commit -C seed-origin s3 &&

	# depth-2: a shallow graft at s2, pushing s3 on top of it
	git clone --depth=2 "file://$(pwd)/seed-origin" seed-client &&

	git init --bare seed-receiver.git &&
	git --git-dir=seed-receiver.git config receive.shallowUpdate true &&

	# Optimization on: the s2 boundary snapshot is withheld, so the
	# receiver cannot graft the new root and rejects the push, leaving the
	# ref uncreated.
	test_must_fail git -C seed-client \
		-c push.shallowExcludeBoundary=true push \
		"file://$(pwd)/seed-receiver.git" HEAD:refs/heads/seeded 2>err &&
	test_grep "remote rejected" err &&
	test_must_fail git --git-dir=seed-receiver.git rev-parse --verify seeded &&

	# Opt-out: the full snapshot is sent, so the same push now succeeds and
	# the new shallow root is grafted.
	git -C seed-client -c push.shallowExcludeBoundary=false push \
		"file://$(pwd)/seed-receiver.git" HEAD:refs/heads/seeded &&
	git --git-dir=seed-receiver.git rev-parse --verify seeded
'

# Splitting a multi-ref push recomputes the pack and avoids exclusions from
# one ref stripping objects needed by another.
test_expect_success 'incomplete multi-ref shallow push advises pushing refs separately' '
	git init hint-origin &&
	git -C hint-origin checkout -b A &&
	test_commit -C hint-origin --no-tag has-shared sh shared &&
	test_commit -C hint-origin --no-tag A1 &&
	git -C hint-origin switch --orphan B &&
	test_commit -C hint-origin --no-tag B0 &&
	test_commit -C hint-origin --no-tag B1 &&

	# Strict checking rejects the incomplete pack before connectivity.
	git init --bare hint-receiver.git &&
	git --git-dir=hint-receiver.git config receive.fsckObjects true &&
	git -C hint-origin push "file://$(pwd)/hint-receiver.git" \
		B:refs/heads/B B:refs/heads/A &&

	git clone --depth=1 --no-single-branch \
		"file://$(pwd)/hint-origin" hint-client &&

	git -C hint-client checkout A &&
	test_commit -C hint-client --no-tag cX &&
	git -C hint-client checkout -b topic B &&
	test_commit -C hint-client --no-tag reintroduce sh shared &&

	test_must_fail git -C hint-client \
		-c push.shallowExcludeBoundary=true \
		push --force "file://$(pwd)/hint-receiver.git" A topic 2>err &&
	test_grep "shallow boundary may have excluded objects" err
'

test_done
