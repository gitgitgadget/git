#!/bin/sh

test_description='reader recovery when a concurrent repack retires a pack

"git repack" consolidates existing packs into a replacement pack and then
removes the redundant packs, deleting each pack.idx before its pack.pack (see
the ordering in unlink_pack_path()).  A reader that discovered one of those
packs -- most easily through a multi-pack-index -- can look the pack up in the
window where its .idx is gone but its .pack is not.

For an OBJECT_INFO_QUICK lookup this is not recovered automatically: QUICK
skips the reprepare-and-retry that a normal lookup performs, so a persistent
reader whose pack list predates the replacement pack reports the object as
missing even though it still lives in the replacement pack.  "git mktree
--batch" is such a persistent QUICK reader: it stays resident across multiple
trees and resolves each entry with OBJECT_INFO_QUICK, so before this fix it
produced wrong output in this window.

The removal can also be observed one step later, from the other side: a reader
that already mmapped a pack.idx (so open_pack_index() succeeds without touching
the filesystem) but has not yet opened its pack.pack.  If the pack.pack is gone
by the time the reader opens it, the same QUICK false-negative results unless we
notice the vanished .pack and reprepare.
'

. ./test-lib.sh

test_expect_success 'setup repo with a multi-pack-index over per-object packs' '
	test_commit seed &&
	a=$(echo A | git hash-object -w --stdin) &&
	b=$(echo B | git hash-object -w --stdin) &&
	echo "$a" | git pack-objects .git/objects/pack/pack >pack-a &&
	echo "$b" | git pack-objects .git/objects/pack/pack >pack-b &&

	# Drop the loose copies so the blobs resolve only through the packs the
	# multi-pack-index references; otherwise the loose object would satisfy
	# the lookup and the pack-removal race could never be observed.
	git prune-packed &&
	git multi-pack-index write &&

	printf "100644 blob %s\ta\n" "$a" >tree-a-input &&
	printf "100644 blob %s\tb\n" "$b" >tree-b-input
'

test_expect_success PIPE 'QUICK reader recovers an object whose pack was retired mid-lookup' '
	victim=".git/objects/pack/pack-$(cat pack-b)" &&
	mkfifo in out &&
	test_when_finished "rm -f in out" &&

	# "git mktree --batch" is a resident OBJECT_INFO_QUICK reader; start it
	# now so its in-memory pack list / midx predates the replacement pack.
	(git mktree --batch <in >out 2>err &) &&
	exec 9>in &&
	exec 8<out &&
	test_when_finished "exec 9>&- || :" &&
	test_when_finished "exec 8<&- || :" &&

	# The first tree forces the reader to prepare its (soon stale) pack view
	# and gives us a synchronization point.
	cat tree-a-input >&9 &&
	echo >&9 &&
	read tree_a <&8 &&

	# Reproduce the transient state a concurrent repack creates: a
	# replacement pack holding every object, plus the original pack for b
	# with its .idx removed but its .pack still present.
	git cat-file --batch-all-objects --batch-check="%(objectname)" >all-oids &&
	git pack-objects .git/objects/pack/pack <all-oids >/dev/null &&
	rm -f "$victim.idx" &&
	test_path_is_file "$victim.pack" &&

	# The reader (stale pack list) now resolves b.  Without the recovery its
	# QUICK lookup reports b missing and mktree dies; with it, b is found in
	# the replacement pack and the misleading "index unavailable" error is
	# not printed.
	cat tree-b-input >&9 &&
	echo >&9 &&
	read tree_b <&8 &&
	exec 9>&- &&

	test -n "$tree_b" &&
	test_grep ! "index unavailable" err
'

test_expect_success 'setup a second repo with plain (non-midx) packs' '
	git init nomidx &&
	(
		cd nomidx &&
		test_commit seed &&
		a=$(echo A | git hash-object -w --stdin) &&
		b=$(echo B | git hash-object -w --stdin) &&
		echo "$a" | git pack-objects .git/objects/pack/pack >pack-a &&
		echo "$b" | git pack-objects .git/objects/pack/pack >pack-b &&
		git prune-packed &&

		printf "100644 blob %s\ta\n" "$a" >tree-a-input &&
		printf "100644 blob %s\tb\n" "$b" >tree-b-input
	)
'

test_expect_success PIPE 'QUICK reader recovers when a mapped pack loses its .pack mid-lookup' '
	(
		cd nomidx &&
		victim=".git/objects/pack/pack-$(cat pack-b)" &&
		mkfifo in out &&

		# We run in a subshell, so leaving the fifos and the reader
		# descriptors open is harmless: they are cleaned up when the
		# subshell exits (which also lets "git mktree --batch" see EOF
		# and quit).
		(git mktree --batch <in >out 2>err &) &&
		exec 9>in &&
		exec 8<out &&

		# Resolving the first tree makes the reader prepare its pack
		# list.  With no multi-pack-index, that scan mmaps every
		# pack.idx -- including the one for b -- but only opens the
		# pack.pack it actually reads (the one for a).  b is now in the
		# exact state we want: its .idx is mapped while its .pack is
		# still unopened.
		cat tree-a-input >&9 &&
		echo >&9 &&
		read tree_a <&8 &&

		# A concurrent repack writes a replacement pack holding every
		# object and removes the now-redundant pack for b.  Delete only
		# its .pack: the reader keeps the mapped .idx for b, so
		# open_pack_index() still succeeds and the failure surfaces when
		# we open the vanished .pack.
		git cat-file --batch-all-objects --batch-check="%(objectname)" >all-oids &&
		git pack-objects .git/objects/pack/pack <all-oids >/dev/null &&
		rm -f "$victim.pack" &&
		test_path_is_file "$victim.idx" &&

		# The reader (stale pack list) now resolves b.  Without the
		# recovery its QUICK lookup opens the missing .pack, gives up,
		# and mktree dies; with it, the vanished .pack forces a reprepare
		# and b is found in the replacement pack.
		cat tree-b-input >&9 &&
		echo >&9 &&
		read tree_b <&8 &&
		exec 9>&- &&

		test -n "$tree_b"
	)
'

test_done
