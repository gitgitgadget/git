#!/bin/sh

test_description='copy-on-write (reflink) population of new worktrees'

. ./test-lib.sh

test_expect_success REFLINK 'test-tool reflink clones file contents' '
	echo content >src &&
	test-tool reflink src dst &&
	test_cmp src dst
'

test_expect_success 'test-tool reflink fails on missing source' '
	test_must_fail test-tool reflink does-not-exist dst2 &&
	test_path_is_missing dst2
'

# Racily-clean donor entries (index written in the same second as the
# file's mtime) are conservatively skipped by the reflink logic, so a
# donor committed "just now" would never be cloned.  Age the donor
# files into the past and refresh the index so that the recorded stat
# data is deterministically clean and non-racy.
test_expect_success REFLINK 'files are reflinked from donor during checkout' '
	test_commit file1 &&
	test-tool chmtime =-60 file1.t &&
	git update-index -q --refresh &&
	git worktree add --no-checkout wt1 &&
	GIT_TRACE2_EVENT="$PWD/trace1.json" \
	GIT_WORKTREE_REFLINK_SOURCE="$PWD" \
		git -C wt1 reset --hard &&
	test_cmp file1.t wt1/file1.t &&
	git -C wt1 status --porcelain >out &&
	test_must_be_empty out &&
	grep "\"category\":\"reflink\".*\"name\":\"hits\"" trace1.json
'

test_expect_success REFLINK 'dirty donor file is not reflinked' '
	echo original >file2 &&
	git add file2 &&
	git commit -m file2 &&
	test-tool chmtime =-60 file2 &&
	git update-index -q --refresh &&
	echo dirtied >file2 &&
	git worktree add --no-checkout wt2 &&
	GIT_WORKTREE_REFLINK_SOURCE="$PWD" git -C wt2 reset --hard &&
	echo original >expect &&
	test_cmp expect wt2/file2 &&
	git checkout -- file2
'

test_expect_success REFLINK 'paths with conversion attributes are not reflinked' '
	git init attr-repo &&
	(
		cd attr-repo &&
		echo "file.txt text eol=crlf" >.gitattributes &&
		printf "one\r\ntwo\r\n" >file.txt &&
		git add . &&
		git commit -m one &&
		test-tool chmtime =-60 .gitattributes file.txt &&
		git update-index -q --refresh &&
		git worktree add --no-checkout wt &&
		GIT_TRACE2_EVENT="$PWD/trace.json" \
		GIT_WORKTREE_REFLINK_SOURCE="$PWD" \
			git -C wt reset --hard &&
		test_cmp file.txt wt/file.txt &&
		grep "\"category\":\"reflink\".*\"name\":\"hits\".*\"count\":1" trace.json
	)
'

test_expect_success REFLINK,SYMLINKS 'symlinks are checked out correctly, not reflinked' '
	ln -s file1.t link1 &&
	git add link1 &&
	git commit -m link1 &&
	git worktree add --no-checkout wt3 &&
	GIT_WORKTREE_REFLINK_SOURCE="$PWD" git -C wt3 reset --hard &&
	test -h wt3/link1
'

test_expect_success REFLINK,POSIXPERM 'executable files keep their mode when reflinked' '
	test_write_lines "#!/bin/sh" "true" >exec.sh &&
	chmod +x exec.sh &&
	git add exec.sh &&
	git commit -m exec &&
	git worktree add --no-checkout wt4 &&
	GIT_WORKTREE_REFLINK_SOURCE="$PWD" git -C wt4 reset --hard &&
	test -x wt4/exec.sh
'

test_expect_success 'bogus reflink source is harmless' '
	test_commit bogus-base &&
	git worktree add --no-checkout wt5 &&
	GIT_WORKTREE_REFLINK_SOURCE=/nonexistent git -C wt5 reset --hard &&
	test_cmp bogus-base.t wt5/bogus-base.t
'

test_done
