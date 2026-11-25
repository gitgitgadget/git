#!/bin/sh

test_description='pack-objects object selection using sparse algorithm'
GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_success 'setup repo' '
	test_commit initial &&
	for i in $(test_seq 1 3)
	do
		mkdir f$i &&
		for j in $(test_seq 1 3)
		do
			mkdir f$i/f$j &&
			echo $j >f$i/f$j/data.txt || return 1
		done
	done &&
	git add . &&
	git commit -m "Initialized trees" &&
	for i in $(test_seq 1 3)
	do
		git checkout -b topic$i main &&
		echo change-$i >f$i/f$i/data.txt &&
		git commit -a -m "Changed f$i/f$i/data.txt" || return 1
	done &&
	cat >packinput.txt <<-EOF &&
	topic1
	^topic2
	^topic3
	EOF
	git rev-parse			\
		topic1			\
		topic1^{tree}		\
		topic1:f1		\
		topic1:f1/f1		\
		topic1:f1/f1/data.txt | sort >expect_objects.txt
'

test_expect_success 'non-sparse pack-objects' '
	git pack-objects --stdout --revs --no-sparse <packinput.txt >nonsparse.pack &&
	git index-pack -o nonsparse.idx nonsparse.pack &&
	git show-index <nonsparse.idx | awk "{print \$2}" >nonsparse_objects.txt &&
	test_cmp expect_objects.txt nonsparse_objects.txt
'

test_expect_success 'sparse pack-objects' '
	git pack-objects --stdout --revs --sparse <packinput.txt >sparse.pack &&
	git index-pack -o sparse.idx sparse.pack &&
	git show-index <sparse.idx | awk "{print \$2}" >sparse_objects.txt &&
	test_cmp expect_objects.txt sparse_objects.txt
'

test_expect_success 'duplicate a folder from f3 and commit to topic1' '
	git checkout topic1 &&
	echo change-3 >f3/f3/data.txt &&
	git commit -a -m "Changed f3/f3/data.txt" &&
	git rev-parse			\
		topic1~1		\
		topic1~1^{tree}		\
		topic1^{tree}		\
		topic1			\
		topic1:f1		\
		topic1:f1/f1		\
		topic1:f1/f1/data.txt | sort >required_objects.txt
'

test_expect_success 'non-sparse pack-objects' '
	git pack-objects --stdout --revs --no-sparse <packinput.txt >nonsparse.pack &&
	git index-pack -o nonsparse.idx nonsparse.pack &&
	git show-index <nonsparse.idx | awk "{print \$2}" >nonsparse_objects.txt &&
	comm -1 -2 required_objects.txt nonsparse_objects.txt >nonsparse_required_objects.txt &&
	test_cmp required_objects.txt nonsparse_required_objects.txt
'

test_expect_success 'sparse pack-objects' '
	git pack-objects --stdout --revs --sparse <packinput.txt >sparse.pack &&
	git index-pack -o sparse.idx sparse.pack &&
	git show-index <sparse.idx | awk "{print \$2}" >sparse_objects.txt &&
	comm -1 -2 required_objects.txt sparse_objects.txt >sparse_required_objects.txt &&
	test_cmp required_objects.txt sparse_required_objects.txt
'

# Demonstrate that the algorithms differ when we copy a tree wholesale
# from one folder to another.

test_expect_success 'duplicate a folder from f1 into f3' '
	mkdir f3/f4 &&
	cp -r f1/f1/* f3/f4 &&
	git add f3/f4 &&
	git commit -m "Copied f1/f1 to f3/f4" &&
	cat >packinput.txt <<-EOF &&
	topic1
	^topic1~1
	EOF
	git rev-parse		\
		topic1		\
		topic1^{tree}   \
		topic1:f3 | sort >required_objects.txt
'

test_expect_success 'non-sparse pack-objects' '
	git pack-objects --stdout --revs --no-sparse <packinput.txt >nonsparse.pack &&
	git index-pack -o nonsparse.idx nonsparse.pack &&
	git show-index <nonsparse.idx | awk "{print \$2}" >nonsparse_objects.txt &&
	comm -1 -2 required_objects.txt nonsparse_objects.txt >nonsparse_required_objects.txt &&
	test_cmp required_objects.txt nonsparse_required_objects.txt
'

# --sparse is enabled by default by pack.useSparse
test_expect_success 'sparse pack-objects' '
	GIT_TEST_PACK_SPARSE=-1 &&
	git rev-parse			\
		topic1			\
		topic1^{tree}		\
		topic1:f3		\
		topic1:f3/f4		\
		topic1:f3/f4/data.txt | sort >expect_sparse_objects.txt &&
	git pack-objects --stdout --revs <packinput.txt >sparse.pack &&
	git index-pack -o sparse.idx sparse.pack &&
	git show-index <sparse.idx | awk "{print \$2}" >sparse_objects.txt &&
	test_cmp expect_sparse_objects.txt sparse_objects.txt
'

test_expect_success 'pack.useSparse enables algorithm' '
	git config pack.useSparse true &&
	git pack-objects --stdout --revs <packinput.txt >sparse.pack &&
	git index-pack -o sparse.idx sparse.pack &&
	git show-index <sparse.idx | awk "{print \$2}" >sparse_objects.txt &&
	test_cmp expect_sparse_objects.txt sparse_objects.txt
'

test_expect_success 'pack.useSparse overridden' '
	git pack-objects --stdout --revs --no-sparse <packinput.txt >sparse.pack &&
	git index-pack -o sparse.idx sparse.pack &&
	git show-index <sparse.idx | awk "{print \$2}" >sparse_objects.txt &&
	test_cmp required_objects.txt sparse_objects.txt
'

test_expect_success 'setup inspect-have receiver and outgoing commits' '
	git checkout -b receiver main &&
	echo large >large.bin &&
	echo receiver >f2/f2/data.txt &&
	git add large.bin f2/f2/data.txt &&
	test_commit --no-tag receiver file.txt &&
	git checkout -b outgoing main &&
	git checkout receiver -- large.bin f2 &&
	git rm -r f3 &&
	test_commit --no-tag outgoing file.txt &&
	receiver_tip=$(git rev-parse receiver) &&
	cat >packinput.txt <<-EOF
	outgoing
	^main
	^receiver
	--inspect-have $receiver_tip
	EOF
'

test_expect_success 'inspect-have excludes an identical root tree' '
	tree=$(git rev-parse receiver^{tree}) &&
	same_tip=$(git commit-tree "$tree" -p main -m "Same tree") &&
	cat >same-tree-input.txt <<-EOF &&
	$same_tip
	^main
	^receiver
	--inspect-have $receiver_tip
	EOF
	echo "$same_tip" >expect_objects.txt &&
	git pack-objects --stdout --revs <same-tree-input.txt >inspected.pack &&
	git index-pack -o inspected.idx inspected.pack &&
	git show-index <inspected.idx >index &&
	awk "{print \$2}" index >inspected_objects.txt &&
	test_cmp_sorted expect_objects.txt inspected_objects.txt
'

test_expect_success 'inspect-have excludes shared large blob from different root trees' '
	test_cmp_rev ! receiver^{tree} outgoing^{tree} &&
	test_cmp_rev receiver:large.bin outgoing:large.bin &&
	blob=$(git rev-parse receiver:large.bin) &&
	sed "/^--inspect-have /d" packinput.txt >ordinary-input.txt &&
	git pack-objects --stdout --revs <ordinary-input.txt >ordinary.pack &&
	git index-pack -o ordinary.idx ordinary.pack &&
	git show-index <ordinary.idx >index &&
	awk "{print \$2}" index >ordinary_objects.txt &&
	test_grep "$blob" ordinary_objects.txt &&
	git rev-parse outgoing outgoing^{tree} outgoing:file.txt >expect_objects.txt &&
	git pack-objects --stdout --revs <packinput.txt >inspected.pack &&
	git index-pack -o inspected.idx inspected.pack &&
	git show-index <inspected.idx >index &&
	awk "{print \$2}" index >inspected_objects.txt &&
	test_cmp_sorted expect_objects.txt inspected_objects.txt
'

test_expect_success 'inspect-have keeps changed blobs and trees' '
	git checkout -b outgoing-changed outgoing &&
	test_commit changed f2/f2/data.txt &&
	sed "s/^outgoing$/outgoing-changed/" packinput.txt >changed-input.txt &&
	git rev-parse \
		outgoing \
		outgoing^{tree} \
		outgoing:file.txt \
		outgoing-changed \
		outgoing-changed^{tree} \
		outgoing-changed:f2 \
		outgoing-changed:f2/f2 \
		outgoing-changed:f2/f2/data.txt >changed_objects.txt &&
	git pack-objects --stdout --revs <changed-input.txt >inspected.pack &&
	git index-pack -o inspected.idx inspected.pack &&
	git show-index <inspected.idx >index &&
	awk "{print \$2}" index >inspected_objects.txt &&
	test_cmp_sorted changed_objects.txt inspected_objects.txt
'

test_expect_success 'inspect-have skips unchanged and receiver-only subtrees' '
	git rev-parse receiver:f2 receiver:f3 >trees &&
	git pack-objects --stdout --window=0 <trees >trees.pack &&
	git index-pack --stdin <trees.pack &&
	while read oid
	do
		path=$(test_oid_to_path "$oid") &&
		rm ".git/objects/$path" || return 1
	done <trees &&
	>pack-access &&
	GIT_TRACE_PACK_ACCESS="$(pwd)/pack-access" \
	git pack-objects --stdout --revs <packinput.txt >inspected.pack &&
	test_must_be_empty pack-access &&
	git rev-parse outgoing outgoing^{tree} outgoing:file.txt >expect_objects.txt &&
	git index-pack -o inspected.idx inspected.pack &&
	git show-index <inspected.idx >index &&
	awk "{print \$2}" index >inspected_objects.txt &&
	test_cmp_sorted expect_objects.txt inspected_objects.txt
'

test_expect_success 'inspect-have rejects non-commit objects' '
	blob=$(git rev-parse receiver:f2/f2/data.txt) &&
	echo "--inspect-have $blob" >invalid-input.txt &&
	test_must_fail git pack-objects --stdout --revs \
		<invalid-input.txt >invalid.pack 2>err &&
	test_grep "is not a commit" err
'

test_expect_success 'inspect-have is ignored in partial clones' '
	test_config remote.missing.url "$(pwd)/missing" &&
	test_config remote.missing.promisor true &&
	sed "/^--inspect-have /d" packinput.txt >partial-input.txt &&
	git pack-objects --stdout --revs --thin --window=0 \
		<partial-input.txt >ordinary.pack 2>err &&
	test_must_be_empty err &&
	git pack-objects --stdout --revs --thin --window=0 \
		<packinput.txt >inspected.pack 2>err &&
	test_must_be_empty err &&
	git index-pack -o ordinary.idx ordinary.pack &&
	git show-index <ordinary.idx >index &&
	awk "{print \$2}" index >ordinary_objects.txt &&
	git index-pack -o inspected.idx inspected.pack &&
	git show-index <inspected.idx >index &&
	awk "{print \$2}" index >inspected_objects.txt &&
	test_cmp ordinary_objects.txt inspected_objects.txt
'

test_expect_success 'inspect-have excludes shared blobs from earlier outgoing commits' '
	git checkout -b receiver-extra receiver &&
	test_commit --no-tag extra f2/f2/small &&
	git checkout -b outgoing-extra outgoing &&
	test_commit --no-tag extra f2/f2/small &&
	receiver_extra_tip=$(git rev-parse receiver-extra) &&
	cat >tip-input.txt <<-EOF &&
	outgoing-extra
	^main
	^receiver-extra
	--inspect-have $receiver_extra_tip
	EOF
	git rev-parse \
		outgoing \
		outgoing^{tree} \
		outgoing:f2 \
		outgoing:f2/f2 \
		outgoing:file.txt \
		outgoing-extra \
		outgoing-extra^{tree} >expect_objects.txt &&
	git pack-objects --stdout --revs <tip-input.txt >inspected.pack &&
	git index-pack -o inspected.idx inspected.pack &&
	git show-index <inspected.idx >index &&
	awk "{print \$2}" index >inspected_objects.txt &&
	test_cmp_sorted expect_objects.txt inspected_objects.txt
'

test_done
