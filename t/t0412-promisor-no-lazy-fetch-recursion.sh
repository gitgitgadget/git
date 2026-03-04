#!/bin/sh

test_description='promisor-remote: no recursive lazy-fetch

Verify that fetch_objects() sets GIT_NO_LAZY_FETCH=1 in the child
fetch environment, so that index-pack cannot recurse back into
fetch_objects() when resolving REF_DELTA bases.
'

. ./test-lib.sh

test_expect_success 'setup' '
	test_create_repo server &&
	test_commit -C server foo &&
	git -C server repack -a -d --write-bitmap-index &&

	git clone "file://$(pwd)/server" client &&
	HASH=$(git -C client rev-parse foo) &&
	rm -rf client/.git/objects/* &&

	git -C client config core.repositoryformatversion 1 &&
	git -C client config extensions.partialclone "origin"
'

test_expect_success 'lazy-fetch spawns only one fetch subprocess' '
	GIT_TRACE="$(pwd)/trace" git -C client cat-file -p "$HASH" &&

	grep "git fetch" trace >fetches &&
	test_line_count = 1 fetches
'

test_expect_success 'child of lazy-fetch has GIT_NO_LAZY_FETCH=1' '
	rm -rf client/.git/objects/* &&

	# Install a reference-transaction hook to record the env var
	# as seen by processes inside the child fetch.
	test_hook -C client reference-transaction <<-\EOF &&
	echo "$GIT_NO_LAZY_FETCH" >>../env-in-child
	EOF

	rm -f env-in-child &&
	git -C client cat-file -p "$HASH" &&

	# The hook runs inside the child fetch, which should have
	# GIT_NO_LAZY_FETCH=1 in its environment.
	grep "^1$" env-in-child
'

test_done
