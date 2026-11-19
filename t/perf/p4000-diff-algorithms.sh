#!/bin/sh

test_description="Tests diff generation performance"

. ./perf-lib.sh

test_perf_default_repo

test_perf 'log -3000 (baseline)' '
	git log -3000 >/dev/null
'

test_perf 'log --raw -3000 (tree-only)' '
	git log --raw -3000 >/dev/null
'

test_perf 'log -p -3000 (Myers)' '
	git log -p -3000 >/dev/null
'

test_expect_success 'prepare revisions for Myers shortstat' '
	git rev-list --no-merges --max-count=8192 HEAD >commits
'

test_perf 'diff-tree --shortstat (Myers)' '
	git diff-tree --stdin -r --root --no-renames --no-ext-diff \
		--no-textconv --no-color --diff-algorithm=myers --shortstat \
		<commits >/dev/null
'

test_perf 'log -p -3000 --histogram' '
	git log -p -3000 --histogram >/dev/null
'

test_perf 'log -p -3000 --patience' '
	git log -p -3000 --patience >/dev/null
'

test_done
