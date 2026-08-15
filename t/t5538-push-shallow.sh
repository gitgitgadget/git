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

test_expect_success 'shallow push only pushes what is necessary' '
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
	GIT_PROGRESS_DELAY=0 git -C adv-client push --progress origin topic 2>err &&

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

# A rejected ref must not over-exclude objects that another, accepted ref
# legitimately needs in the pack.  Set up a testcase using two independent
# shallow roots.
#
#   origin: two unrelated histories; only branch A carries blob O (sh=shared)
#       A:  A0---A1     (A0, A1 trees contain sh=O)
#       B:  B0---B1     (no "shared" blob)
#
#   receiver: seeded from branch B only, under both ref names; lacks blob O
#       refs/heads/B -> B1
#       refs/heads/A -> B1     (makes our A push a non-fast-forward)
#
#   client: "clone --depth=1 --no-single-branch" gives a graft at each tip
#           and a copy of blob O under A1   (x = cut parents = shallow graft)
#           x        x
#           |        |
#          A1       B1
#           |        |
#          cX     topic=cY     (cY re-adds sh=O, which the receiver lacks)
#
#   push "A topic" (non-atomic):
#     A     -> a non-fast-forward vs receiver A=B1, so its ref update is
#              rejected locally and never applied.  It still takes part in
#              the shared pack computation, and the buggy code also walked
#              back from it to graft A1 (which owns O).
#     topic -> accepted; cY grafts onto B1 and needs blob O.
#
#   Using the shallow graft A1 (an ancestor of A) to trim the pack, even
#   though our push of A is rejected locally, would omit blob O from topic's
#   pack -- yet topic needs O.  We want to ensure that when topic is pushed,
#   O is sent along with it despite A being rejected.
test_expect_success 'shallow push does not over-exclude for an accepted ref via a rejected one' '
	# origin
	git init tworoot-origin &&
	git -C tworoot-origin checkout -b A &&
	test_commit -C tworoot-origin --no-tag has-shared sh shared &&
	test_commit -C tworoot-origin --no-tag A1 &&
	git -C tworoot-origin switch --orphan B &&
	test_commit -C tworoot-origin --no-tag B0 &&
	test_commit -C tworoot-origin --no-tag B1 &&

	# receiver: branch B only, exposed as both B and A
	git init --bare tworoot-receiver.git &&
	git -C tworoot-origin push "file://$(pwd)/tworoot-receiver.git" \
		B:refs/heads/B B:refs/heads/A &&

	# client: a shallow graft at each branch tip
	git clone --depth=1 --no-single-branch \
		"file://$(pwd)/tworoot-origin" tworoot-client &&

	# branch A gets commit cX; including A in the push gives us a
	# locally-rejected ref whose graft A1 the buggy code walked to.  The A
	# ref update is a non-fast-forward, so it is rejected and never applied.
	git -C tworoot-client checkout A &&
	test_commit -C tworoot-client --no-tag cX &&

	# branch topic is what we actually send, reintroducing blob O on B1
	git -C tworoot-client checkout -b topic B &&
	test_commit -C tworoot-client --no-tag reintroduce sh shared &&

	# push both in one command: they share a single pack computation, so a
	# graft reached from the rejected A can strip objects that topic needs.
	# The A ref update is rejected locally (non-fast-forward); the shared
	# pack must still contain blob O for topic to land on the receiver.
	test_must_fail git -C tworoot-client push \
		"file://$(pwd)/tworoot-receiver.git" A topic &&
	git --git-dir=tworoot-receiver.git rev-parse --verify topic
'

# push.shallowExcludeBoundary (default true) omits the shallow boundary
# snapshot from the pack, since an ordinary receiver already has it.  The
# exception is a receiver willing to adopt a *new* shallow root
# (receive.shallowUpdate): it genuinely needs that snapshot, so the default
# optimization leaves it unable to graft the new root.  Verify the receiver
# rejects such a push (rather than corrupting itself), and that setting the
# config to false restores the full snapshot and lets the push succeed.  This
# is the tradeoff that motivates the config knob.
test_expect_success 'default push to a shallowUpdate receiver rejects a rootless snapshot' '
	git init seed-origin &&
	test_commit -C seed-origin s1 &&
	test_commit -C seed-origin s2 &&
	test_commit -C seed-origin s3 &&

	# depth-2: a shallow graft at s2, pushing s3 on top of it
	git clone --depth=2 "file://$(pwd)/seed-origin" seed-client &&

	git init --bare seed-receiver.git &&
	git --git-dir=seed-receiver.git config receive.shallowUpdate true &&

	# Default (optimization on): the s2 boundary snapshot is withheld, so
	# the receiver cannot graft the new root and rejects the push, leaving
	# the ref uncreated.
	test_must_fail git -C seed-client push \
		"file://$(pwd)/seed-receiver.git" HEAD:refs/heads/seeded 2>err &&
	test_grep "remote rejected" err &&
	test_must_fail git --git-dir=seed-receiver.git rev-parse --verify seeded &&

	# Opt-out: the full snapshot is sent, so the same push now succeeds and
	# the new shallow root is grafted.
	git -C seed-client -c push.shallowExcludeBoundary=false push \
		"file://$(pwd)/seed-receiver.git" HEAD:refs/heads/seeded &&
	git --git-dir=seed-receiver.git rev-parse --verify seeded
'

test_done
