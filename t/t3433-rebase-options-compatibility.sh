#!/bin/sh
#
# Copyright (c) 2019 Rohit Ashiwal
#

test_description='tests to ensure compatibility between am and interactive backends'

. ./test-lib.sh

. "$TEST_DIRECTORY"/lib-rebase.sh

GIT_AUTHOR_DATE="1999-04-02T08:03:20+05:30"
export GIT_AUTHOR_DATE

# This is a special case in which both am and interactive backends
# provide the same output. It was done intentionally because
# both the backends fall short of optimal behaviour.
test_expect_success 'setup' '
	git checkout -b topic &&
	q_to_tab >file <<-\EOF &&
	line 1
	Qline 2
	line 3
	EOF
	git add file &&
	git commit -m "add file" &&
	cat >file <<-\EOF &&
	line 1
	new line 2
	line 3
	EOF
	git commit -am "update file" &&
	git tag side &&
	test_commit commit1 foo foo1 &&
	test_commit commit2 foo foo2 &&
	test_commit commit3 foo foo3 &&

	git checkout --orphan master &&
	git rm --cached foo &&
	rm foo &&
	sed -e "s/^|//" >file <<-\EOF &&
	|line 1
	|        line 2
	|line 3
	EOF
	git add file &&
	git commit -m "add file" &&
	git tag main
'

test_expect_success '--ignore-whitespace works with am backend' '
	cat >expect <<-\EOF &&
	line 1
	new line 2
	line 3
	EOF
	test_must_fail git rebase main side &&
	git rebase --abort &&
	git rebase --ignore-whitespace main side &&
	test_cmp expect file
'

test_expect_success '--ignore-whitespace works with interactive backend' '
	cat >expect <<-\EOF &&
	line 1
	new line 2
	line 3
	EOF
	test_must_fail git rebase --merge main side &&
	git rebase --abort &&
	git rebase --merge --ignore-whitespace main side &&
	test_cmp expect file
'

test_expect_success '--ignore-whitespace is remembered when continuing' '
	cat >expect <<-\EOF &&
	line 1
	new line 2
	line 3
	EOF
	(
		set_fake_editor &&
		FAKE_LINES="break 1" &&
		export FAKE_LINES &&
		git rebase -i --ignore-whitespace main side
	) &&
	git rebase --continue &&
	test_cmp expect file
'

test_expect_success '--committer-date-is-author-date works with am backend' '
	GIT_AUTHOR_DATE="@1234 +0300" git commit --amend --reset-author &&
	git rebase --committer-date-is-author-date HEAD^ &&
	git log -1 --pretty="format:%ai" >authortime &&
	git log -1 --pretty="format:%ci" >committertime &&
	test_cmp authortime committertime
'

test_expect_success '--committer-date-is-author-date works with interactive backend' '
	GIT_AUTHOR_DATE="@1234 +0300" git commit --amend --reset-author &&
	git rebase -i --committer-date-is-author-date HEAD^ &&
	git log -1 --pretty="format:%ai" >authortime &&
	git log -1 --pretty="format:%ci" >committertime &&
	test_cmp authortime committertime
'

test_expect_success '--committer-date-is-author-date works with rebase -r' '
	git checkout side &&
	GIT_AUTHOR_DATE="@1234 +0300" git merge --no-ff commit3 &&
	git rebase -r --root --committer-date-is-author-date &&
	git log --pretty="format:%ai" >authortime &&
	git log --pretty="format:%ci" >committertime &&
	test_cmp authortime committertime
'

test_expect_success '--committer-date-is-author-date works when forking merge' '
	git checkout side &&
	GIT_AUTHOR_DATE="@1234 +0300" git merge --no-ff commit3 &&
	git rebase -r --root --strategy=resolve --committer-date-is-author-date &&
	git log --pretty="format:%ai" >authortime &&
	git log --pretty="format:%ci" >committertime &&
	test_cmp authortime committertime

'

test_expect_success '--committer-date-is-author-date works when committing conflict resolution' '
	git checkout commit2 &&
	GIT_AUTHOR_DATE="@1980 +0000" git commit --amend --only --reset-author &&
	git log -1 --format=%at HEAD >expect &&
	test_must_fail git rebase -i --committer-date-is-author-date \
		--onto HEAD^^ HEAD^ &&
	echo resolved > foo &&
	git add foo &&
	git rebase --continue &&
	git log -1 --format=%ct HEAD >actual &&
	test_cmp expect actual
'

# Checking for +0000 in author time is enough since default
# timezone is UTC, but the timezone used while committing
# sets to +0530.
test_expect_success '--ignore-date works with am backend' '
	git commit --amend --date="$GIT_AUTHOR_DATE" &&
	git rebase --ignore-date HEAD^ &&
	git log -1 --pretty="format:%ai" >authortime &&
	grep "+0000" authortime
'

test_expect_success '--ignore-date works with interactive backend' '
	git commit --amend --date="$GIT_AUTHOR_DATE" &&
	git rebase --ignore-date -i HEAD^ &&
	git log -1 --pretty="format:%ai" >authortime &&
	grep "+0000" authortime
'

test_expect_success '--ignore-date works after conflict resolution' '
	test_must_fail git rebase --ignore-date -i \
		--onto commit2^^ commit2^ commit2 &&
	echo resolved >foo &&
	git add foo &&
	git rebase --continue &&
	git log --pretty=%ai >authortime &&
	grep +0000 authortime
'

test_expect_success '--ignore-date works with rebase -r' '
	git checkout side &&
	git merge --no-ff commit3 &&
	git rebase -r --root --ignore-date &&
	git log --pretty=%ai >authortime &&
	! grep -v "+0000" authortime
'

test_expect_success '--ignore-date with --committer-date-is-author-date works' '
	test_must_fail git rebase -i --committer-date-is-author-date \
		--ignore-date --onto commit2^^ commit2^ commit3 &&
	git checkout --theirs foo &&
	git add foo &&
	git rebase --continue &&
	git log -2 --pretty=%ai >authortime &&
	git log -2 --pretty=%ci >committertime &&
	test_cmp authortime committertime &&
	! grep -v "+0000" authortime
'

test_expect_success '--ignore-date --committer-date-is-author-date works when forking merge' '
	GIT_SEQUENCE_EDITOR="echo \"merge -C $(git rev-parse HEAD) commit3\">" \
		git rebase -i --strategy=resolve --ignore-date \
		--committer-date-is-author-date side side &&
	git log -1 --pretty=%ai >authortime &&
	git log -1 --pretty=%ci >committertime &&
	test_cmp authortime committertime &&
	grep "+0000" authortime
 '

# This must be the last test in this file
test_expect_success '$EDITOR and friends are unchanged' '
	test_editor_unchanged
'

test_done
