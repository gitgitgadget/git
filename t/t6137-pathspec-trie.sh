#!/bin/sh

test_description='test optimized pathspec trie lookup'
. ./test-lib.sh

# This is a basic lookup test; the offsets are there to provide
# some variation in where we land in our binary search.
ps="faa fzz foo/bar foo/baa foo/bzz"
for offset in a "a b" "a b c"; do
	test_expect_success "lookups with ps=$offset $ps" "
		cat >expect <<-\\EOF &&
		no
		yes
		yes
		no
		EOF
		test-tool pathspec trie $offset $ps -- f faa foo/bar foo/baz >actual &&
		test_cmp expect actual
	"
done

test_expect_success 'pathspecs match by prefix' '
	cat >expect <<-\EOF &&
	yes
	yes
	yes
	EOF
	test-tool pathspec trie foo -- foo foo/bar foo/with/deep/subdirs >actual &&
	test_cmp expect actual
'

test_expect_success 'trailing slash sets must_be_dir' '
	cat >expect <<-\EOF &&
	no
	yes
	yes
	EOF
	test-tool pathspec trie dir/ -- dir dir/ dir/foo
'

test_expect_success 'overlapping pathspecs allow the "loose" side' '
	echo yes >expect &&
	test-tool pathspec trie foo foo/ -- foo >actual &&
	test_cmp expect actual &&
	test-tool pathspec trie foo/ foo -- foo >actual &&
	test_cmp expect actual
'

test_expect_success '"." at the root matches everything' '
	cat >expect <<-\EOF &&
	yes
	yes
	EOF
	test-tool pathspec trie . -- foo foo/bar
'

test_done
