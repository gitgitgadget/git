#!/bin/sh

test_description='Performance of merge-base with a deep side branch

Synthetic repository that triggers slow merge-base when a merge commit
introduces a side branch rooted far back in history.  The pathological
case forces paint_down_to_common() to chase STALE flags through many
intermediate commits after the merge-base is already found.

Topology (N = $NUM_COMMITS commits on main before the merge, F is
the BRANCH_POINT-th of them, S = $SIDE_COMMITS commits on the side):

#        F -- s1 -- ... -- sS             (side, dates set old)
#       /                    \
#  1 -- 2 -- ... -- N-1 -- N -- M         (main)
#                          |
#                          P              (pr branch)

merge-base(M, P) = N.  Without early termination, the algorithm
walks from N back through roughly N - BRANCH_POINT commits to F
to exhaust STALE processing.
'

. ./perf-lib.sh

NUM_COMMITS=500000
BRANCH_POINT=10000
SIDE_COMMITS=5

test_expect_success 'setup synthetic repo' '
	git init --bare repo.git &&
	awk -v N=$NUM_COMMITS -v BP=$BRANCH_POINT -v S=$SIDE_COMMITS '\''
		BEGIN {
			# Shared blob
			print "blob"
			print "mark :1"
			print "data 8"
			print "content"
			print ""

			# Main branch commits: mark :2 is the 1st main
			# commit, mark :(N+1) is the Nth.
			for (i = 2; i <= N + 1; i++) {
				print "commit refs/heads/main"
				print "mark :" i
				print "committer C <c@c> " (1000000 + i) " +0000"
				print "data 2"
				print "x"
				if (i > 2)
					print "from :" (i - 1)
				print "M 100644 :1 file"
				print ""
			}

			# Side branch forks from the BP-th main commit
			# (mark :(BP+1)), with old dates.
			side_start = BP + 1
			side_mark_base = N + 2
			for (j = 0; j < S; j++) {
				mark = side_mark_base + j
				if (j == 0)
					from_mark = side_start
				else
					from_mark = mark - 1
				print "commit refs/heads/side"
				print "mark :" mark
				print "committer C <c@c> " (500000 + j) " +0000"
				print "data 2"
				print "x"
				print "from :" from_mark
				print ""
			}

			# Merge side into main
			main_tip = N + 1
			side_tip = side_mark_base + S - 1
			merge_mark = side_tip + 1
			print "commit refs/heads/main"
			print "mark :" merge_mark
			print "committer C <c@c> " (1000000 + N + 2) " +0000"
			print "data 2"
			print "x"
			print "from :" main_tip
			print "merge :" side_tip
			print ""

			# PR branch forked from main tip (just before merge)
			pr_mark = merge_mark + 1
			print "commit refs/heads/pr"
			print "mark :" pr_mark
			print "committer C <c@c> " (1000000 + N + 3) " +0000"
			print "data 2"
			print "x"
			print "from :" main_tip
			print ""
		}
	'\'' </dev/null |
	git -C repo.git fast-import --quiet &&

	git -C repo.git commit-graph write --reachable &&

	# Compute expected merge-base: just below the merge on main.
	git -C repo.git rev-parse main~1 >expect
'

test_expect_success 'merge-base result is correct' '
	git -C repo.git merge-base --all main pr >actual &&
	test_cmp expect actual
'

test_perf 'merge-base: commit introducing old side branch' '
	git -C repo.git merge-base --all main pr
'

test_perf 'merge-base: no side branch (baseline)' '
	git -C repo.git merge-base --all main~1 pr
'

test_done
