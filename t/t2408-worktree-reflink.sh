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

test_expect_success REFLINK 'git worktree add reflinks from the primary checkout' '
	test-tool chmtime =-60 file1.t &&
	GIT_TRACE2_EVENT="$PWD/trace-e2e.json" git worktree add wt-auto &&
	git -C wt-auto status --porcelain >out &&
	test_must_be_empty out &&
	test_cmp file1.t wt-auto/file1.t &&
	grep "\"category\":\"reflink\".*\"name\":\"hits\"" trace-e2e.json
'

test_expect_success REFLINK 'worktree.useReflink=false disables reflinking' '
	GIT_TRACE2_EVENT="$PWD/trace-off.json" \
		git -c worktree.usereflink=false worktree add wt-off &&
	git -C wt-off status --porcelain >out &&
	test_must_be_empty out &&
	! grep "\"category\":\"reflink\"" trace-off.json
'

test_expect_success REFLINK 'worktree add from a linked worktree uses the primary donor' '
	test-tool chmtime =-60 file1.t &&
	GIT_TRACE2_EVENT="$PWD/trace-linked.json" \
		git -C wt-auto worktree add "$PWD/wt-from-linked" &&
	test_cmp file1.t wt-from-linked/file1.t &&
	grep "\"category\":\"reflink\".*\"name\":\"hits\"" trace-linked.json
'

test_expect_success 'worktree add succeeds regardless of filesystem support' '
	git worktree add wt-plain &&
	git -C wt-plain status --porcelain >out &&
	test_must_be_empty out
'

test_expect_success '--no-checkout skips donor refresh and reflink env' '
	GIT_TRACE2_EVENT="$PWD/trace-nc.json" \
		git worktree add --no-checkout wt-nc &&
	! grep "update-index" trace-nc.json &&
	! grep "\"category\":\"reflink\"" trace-nc.json
'

# "eol=lf" resolves to an input-style CRLF action (CRLF_TEXT_INPUT) whose
# checkout-direction (smudge) conversion is the identity, so an LF blob
# checks out byte-for-byte as its own contents.  Such files are eligible
# for reflink.  Here both .gitattributes (no attribute matches its own
# name -> CRLF_BINARY) and file.txt (eol=lf -> CRLF_TEXT_INPUT) are
# reflinked, so the donor yields two hits.
test_expect_success REFLINK 'input-style CRLF (eol=lf) files are reflinked' '
	git init eol-lf-repo &&
	(
		cd eol-lf-repo &&
		echo "*.txt text eol=lf" >.gitattributes &&
		printf "one\ntwo\n" >file.txt &&
		git add . &&
		git commit -m one &&
		test-tool chmtime =-60 .gitattributes file.txt &&
		git update-index -q --refresh &&
		git worktree add --no-checkout wt &&
		GIT_TRACE2_EVENT="$PWD/trace.json" \
		GIT_WORKTREE_REFLINK_SOURCE="$PWD" \
			git -C wt reset --hard &&
		test_cmp file.txt wt/file.txt &&
		grep "\"category\":\"reflink\".*\"name\":\"hits\".*\"count\":2" trace.json
	)
'

# Guard the belt-and-braces size gate: the working file holds CRLF bytes,
# but "eol=lf" cleans them to an LF blob, so git considers the file
# content-clean.  The index records the on-disk size (10 bytes, CRLF)
# while the blob is 8 bytes (LF), so the donor entry is stat-clean yet its
# on-disk bytes diverge from the blob.  The size==blob backstop must
# reject it, and the new worktree must receive the LF blob, never the
# donor's CRLF bytes.  Only .gitattributes is reflinked -> exactly one hit.
test_expect_success REFLINK 'eol=lf donor with divergent worktree bytes is not reflinked' '
	git init eol-lf-crlf-repo &&
	(
		cd eol-lf-crlf-repo &&
		echo "*.txt text eol=lf" >.gitattributes &&
		printf "one\r\ntwo\r\n" >file.txt &&
		git add . &&
		git commit -m one &&
		test-tool chmtime =-60 .gitattributes file.txt &&
		git update-index -q --refresh &&
		git worktree add --no-checkout wt &&
		GIT_TRACE2_EVENT="$PWD/trace.json" \
		GIT_WORKTREE_REFLINK_SOURCE="$PWD" \
			git -C wt reset --hard &&
		printf "one\ntwo\n" >expect &&
		test_cmp expect wt/file.txt &&
		grep "\"category\":\"reflink\".*\"name\":\"hits\".*\"count\":1" trace.json
	)
'

# On a filesystem where FICLONE is not supported at all (e.g. ext4), the
# first reflink attempt fails with EOPNOTSUPP/ENOTTY/ENOSYS/EXDEV and
# reflink_donor.state latches to "disabled" for the rest of the process,
# so no further attempts are made even though more eligible donor files
# remain.  This only makes sense on a filesystem where FICLONE truly
# fails, hence the !REFLINK gate: the REFLINK prereq itself proves
# FICLONE works here, which would defeat the point of this test.
test_expect_success !REFLINK 'first FICLONE failure disables further attempts' '
	git init reflink-disable-repo &&
	(
		cd reflink-disable-repo &&
		test_commit fileA &&
		test_commit fileB &&
		test_commit fileC &&
		test-tool chmtime =-60 fileA.t fileB.t fileC.t &&
		git update-index -q --refresh &&
		git worktree add --no-checkout wt &&
		GIT_TRACE2_EVENT="$PWD/trace.json" \
		GIT_WORKTREE_REFLINK_SOURCE="$PWD" \
			git -C wt reset --hard &&
		test_cmp fileA.t wt/fileA.t &&
		test_cmp fileB.t wt/fileB.t &&
		test_cmp fileC.t wt/fileC.t &&
		grep "\"category\":\"reflink\".*\"name\":\"attempts\".*\"count\":1" trace.json &&
		! grep "\"category\":\"reflink\".*\"name\":\"hits\"" trace.json
	)
'

test_expect_success REFLINK 'skip-worktree donor entries are not reflinked' '
	git init skip-worktree-repo &&
	(
		cd skip-worktree-repo &&
		test_commit fileA &&
		test_commit fileB &&
		test-tool chmtime =-60 fileA.t fileB.t &&
		git update-index -q --refresh &&
		git update-index --skip-worktree fileB.t &&
		git worktree add --no-checkout wt &&
		GIT_TRACE2_EVENT="$PWD/trace.json" \
		GIT_WORKTREE_REFLINK_SOURCE="$PWD" \
			git -C wt reset --hard &&
		test_cmp fileA.t wt/fileA.t &&
		test_cmp fileB.t wt/fileB.t &&
		grep "\"category\":\"reflink\".*\"name\":\"hits\".*\"count\":1" trace.json
	)
'

# The donor's entry carries CE_VALID (assume-unchanged), but its on-disk
# content was changed without telling the index.  The replacement text is
# deliberately the same size as the original blob: that neutralizes the
# independent size==blob backstop (which would reject a differently-sized
# replacement on its own and mask whatever ie_match_stat() decided), so
# the only thing standing between this stale donor and a bad reflink is
# passing CE_MATCH_IGNORE_VALID to force a real stat compare instead of
# trusting the assume-unchanged bit.
test_expect_success REFLINK 'assume-unchanged donor with stale content is not reflinked' '
	git init assume-unchanged-repo &&
	(
		cd assume-unchanged-repo &&
		printf "hello world\n" >file.t &&
		git add file.t &&
		git commit -m file &&
		test-tool chmtime =-60 file.t &&
		git update-index -q --refresh &&
		git update-index --assume-unchanged file.t &&
		printf "HELLO WORLD\n" >file.t &&
		git worktree add --no-checkout wt &&
		GIT_TRACE2_EVENT="$PWD/trace.json" \
		GIT_WORKTREE_REFLINK_SOURCE="$PWD" \
			git -C wt reset --hard &&
		printf "hello world\n" >expect &&
		test_cmp expect wt/file.t &&
		! grep "\"category\":\"reflink\"" trace.json
	)
'

# "update-index --chmod=+x" only flips the mode bit recorded in the
# index; it does not touch the file on disk.  That leaves the donor's
# index entry at 100755 while the committed tree (and the new worktree's
# own index, built fresh from that tree) still has 100644, so the
# ce_mode comparison in try_reflink_entry() must reject the donor before
# ever looking at its on-disk stat data.
test_expect_success REFLINK,POSIXPERM 'donor index mode differing from target tree is not reflinked' '
	git init mode-mismatch-repo &&
	(
		cd mode-mismatch-repo &&
		test_write_lines "#!/bin/sh" "true" >file.sh &&
		git add file.sh &&
		git commit -m file &&
		test-tool chmtime =-60 file.sh &&
		git update-index -q --refresh &&
		git update-index --chmod=+x file.sh &&
		git worktree add --no-checkout wt &&
		GIT_TRACE2_EVENT="$PWD/trace.json" \
		GIT_WORKTREE_REFLINK_SOURCE="$PWD" \
			git -C wt reset --hard &&
		test_cmp file.sh wt/file.sh &&
		! test -x wt/file.sh &&
		! grep "\"category\":\"reflink\"" trace.json
	)
'

# Gitlink entries never reach try_reflink_entry() (it is only consulted
# for S_IFREG blobs), so they are unaffected by a reflink donor; this
# guards against a regression that made checkout mishandle gitlinks
# whenever a donor is active, by comparing against a control worktree
# with reflinking turned off via worktree.usereflink=false.
test_expect_success REFLINK 'gitlink entries are checked out correctly with reflink donor' '
	git init gitlink-repo &&
	(
		cd gitlink-repo &&
		git init inner &&
		test_commit -C inner inner-file &&
		inner_head=$(git -C inner rev-parse HEAD) &&
		git update-index --add --cacheinfo 160000,$inner_head,inner &&
		test_commit regular &&
		test-tool chmtime =-60 regular.t &&
		git update-index -q --refresh &&
		GIT_TRACE2_EVENT="$PWD/trace.json" git worktree add wt &&
		git -c worktree.usereflink=false worktree add wt-control &&
		test_cmp regular.t wt/regular.t &&
		test_path_is_dir wt/inner &&
		test_path_is_dir wt-control/inner &&
		grep "\"category\":\"reflink\".*\"name\":\"hits\".*\"count\":1" trace.json
	)
'

test_done
