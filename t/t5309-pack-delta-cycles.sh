#!/bin/sh

test_description='test handling of delta cycles in packfiles'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-pack.sh

# Two similar-ish objects that we have computed deltas between.
A=$(test_oid packlib_7_0)
B=$(test_oid packlib_7_76)

# double-check our hand-constucted packs
test_expect_success 'index-pack works with a single delta (A->B)' '
	clear_packs &&
	{
		pack_header 2 &&
		pack_obj $A $B &&
		pack_obj $B
	} >ab.pack &&
	pack_trailer ab.pack &&
	git index-pack --stdin <ab.pack &&
	git cat-file -t $A &&
	git cat-file -t $B
'

test_expect_success 'index-pack works with a single delta (B->A)' '
	clear_packs &&
	{
		pack_header 2 &&
		pack_obj $A &&
		pack_obj $B $A
	} >ba.pack &&
	pack_trailer ba.pack &&
	git index-pack --stdin <ba.pack &&
	git cat-file -t $A &&
	git cat-file -t $B
'

pack_delta_bases () {
	git verify-pack -v "$1" >verify &&
	awk '$2 == "blob" { print $1, (NF == 7 ? $7 : "base") }' verify
}

test_expect_success '--prefer-reused-deltas retains the selected delta instead of forming a cycle' '
	clear_packs &&
	git index-pack --stdin <ab.pack >ab.hash &&
	git index-pack --stdin <ba.pack >ba.hash &&
	ab="pack-$(cut -f2 ab.hash)" &&
	ba="pack-$(cut -f2 ba.hash)" &&

	cat >expect-ab <<-EOF &&
	$A $B
	$B base
	EOF
	cat >expect-ba <<-EOF &&
	$A base
	$B $A
	EOF
	pack_delta_bases ".git/objects/pack/$ab.idx" >actual &&
	test_cmp expect-ab actual &&
	pack_delta_bases ".git/objects/pack/$ba.idx" >actual &&
	test_cmp expect-ba actual &&

	# Visit ba first: A is a base, then B is a delta against A.
	# Switching to the A-against-B copy in ab would form a cycle.
	test-tool chmtime =1000000000 ".git/objects/pack/$ab.pack" &&
	test-tool chmtime =1000000200 ".git/objects/pack/$ba.pack" &&
	printf "%s.pack\n%s.pack\n" "$ba" "$ab" >packs &&
	git pack-objects --stdin-packs --window=0 --prefer-reused-deltas \
		out <packs >out.hash &&

	# Keep B against A. Forming and then breaking the cycle would
	# instead keep A against B, since A was enumerated first.
	pack_delta_bases "out-$(cat out.hash).idx" >actual &&
	test_cmp expect-ba actual
'

test_expect_success 'index-pack detects missing base objects' '
	clear_packs &&
	{
		pack_header 1 &&
		pack_obj $A $B
	} >missing.pack &&
	pack_trailer missing.pack &&
	test_must_fail git index-pack --fix-thin --stdin <missing.pack
'

test_expect_success 'index-pack detects REF_DELTA cycles' '
	clear_packs &&
	{
		pack_header 2 &&
		pack_obj $A $B &&
		pack_obj $B $A
	} >cycle.pack &&
	pack_trailer cycle.pack &&
	test_must_fail git index-pack --fix-thin --stdin <cycle.pack
'

test_expect_success 'failover to an object in another pack' '
	clear_packs &&
	git index-pack --stdin <ab.pack &&

	# This cycle does not fail since the existence of A & B in
	# the repo allows us to resolve the cycle.
	git index-pack --stdin --fix-thin <cycle.pack
'

test_expect_success 'failover to a duplicate object in the same pack' '
	clear_packs &&
	{
		pack_header 3 &&
		pack_obj $A $B &&
		pack_obj $B $A &&
		pack_obj $A
	} >recoverable.pack &&
	pack_trailer recoverable.pack &&

	# This cycle does not fail since the existence of a full copy
	# of A in the pack allows us to resolve the cycle.
	git index-pack --fix-thin --stdin <recoverable.pack
'

test_expect_success 'index-pack works with thin pack A->B->C with B on disk' '
	git init server &&
	(
		cd server &&
		test_commit_bulk 4
	) &&

	A=$(git -C server rev-parse HEAD^{tree}) &&
	B=$(git -C server rev-parse HEAD~1^{tree}) &&
	C=$(git -C server rev-parse HEAD~2^{tree}) &&
	git -C server reset --hard HEAD~1 &&

	test-tool -C server pack-deltas --num-objects=2 >thin.pack <<-EOF &&
	REF_DELTA $A $B
	REF_DELTA $B $C
	EOF

	git clone "file://$(pwd)/server" client &&
	(
		cd client &&
		git index-pack --fix-thin --stdin <../thin.pack
	)
'

test_done
