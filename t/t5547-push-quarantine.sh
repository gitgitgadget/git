#!/bin/sh

test_description='check quarantine of objects during push'

. ./test-lib.sh

test_expect_success 'create picky dest repo' '
	git init --bare dest.git &&
	test_hook --setup -C dest.git pre-receive <<-\EOF
	while read old new ref; do
		test "$(git log -1 --format=%s $new)" = reject && exit 1
	done
	exit 0
	EOF
'

test_expect_success 'accepted objects work' '
	test_commit ok &&
	git push dest.git HEAD &&
	commit=$(git rev-parse HEAD) &&
	git --git-dir=dest.git cat-file commit $commit
'

test_expect_success 'rejected objects are not installed' '
	test_commit reject &&
	commit=$(git rev-parse HEAD) &&
	test_must_fail git push dest.git reject &&
	test_must_fail git --git-dir=dest.git cat-file commit $commit
'

test_expect_success 'rejected objects are removed' '
	echo "incoming-*" >expect &&
	(cd dest.git/objects && echo incoming-*) >actual &&
	test_cmp expect actual
'

test_expect_success 'push to repo path with path separator (colon)' '
	# The interesting failure case here is when the
	# receiving end cannot access its original object directory,
	# so make it likely for us to generate a delta by having
	# a non-trivial file with multiple versions.

	test-tool genrandom foo 4096 >file.bin &&
	git add file.bin &&
	git commit -m bin &&

	if test_have_prereq MINGW
	then
		pathsep=";"
	else
		pathsep=":"
	fi &&
	git clone --bare . "xxx${pathsep}yyy.git" &&

	echo change >>file.bin &&
	git commit -am change &&
	# Note that we have to use the full path here, or it gets confused
	# with the ssh host:path syntax.
	git push "$(pwd)/xxx${pathsep}yyy.git" HEAD
'

test_expect_success 'updating a ref from quarantine is forbidden' '
	git init --bare update.git &&
	test_hook -C update.git pre-receive <<-\EOF &&
	read old new refname
	git update-ref refs/heads/unrelated $new
	exit 1
	EOF
	test_must_fail git push update.git HEAD &&
	git -C update.git fsck
'

test_expect_success '.keep file is removed after push' '
	test_when_finished rm -rf keep.git &&
	git init --bare keep.git &&

	git -C keep.git config set receive.unpackLimit 0 &&

	# While incoming objects are still quarantined, validate that the
	# ".keep" lockfile is present in the quarantine directory.
	test_hook -C keep.git pre-receive <<-\EOF &&
	keep="$(ls "$GIT_QUARANTINE_PATH"/pack/pack-*.keep)" &&
	test -f "$keep"
	EOF

	# After quarantined objects are migrated, validate that the ".keep"
	# lockfile is migrated and present in the main ODB.
	test_hook -C keep.git reference-transaction <<-\EOF &&
	keep="$(ls objects/pack/pack-*.keep)" &&
	test -f "$keep"
	EOF

	test_commit foo &&
	git push keep.git HEAD &&

	# Once the operation is complete, validate that the ".keep" lockfile has
	# been removed.
	pack="$(ls keep.git/objects/pack/pack-*.pack)" &&
	keep="${pack%.pack}.keep" &&
	test_path_is_file "$pack" &&
	test_path_is_missing "$keep"
'

test_expect_success 'a rejected push does not remove a foreign ".keep"' '
	test_when_finished rm -rf foreign.git &&
	git init --bare foreign.git &&
	git -C foreign.git config set receive.unpackLimit 0 &&

	# Get a packfile into the main object database without updating any
	# ref, so that pushing the same objects again reuses its name.
	test_hook -C foreign.git update <<-\EOF &&
	exit 1
	EOF
	test_commit foreign &&
	test_must_fail git push foreign.git HEAD:refs/heads/one &&

	pack="$(ls foreign.git/objects/pack/pack-*.pack)" &&
	keep="${pack%.pack}.keep" &&

	# Pretend somebody else holds the lock on that packfile, and let the
	# next push be rejected before its objects are ever migrated.
	>"$keep" &&
	test_hook -C foreign.git pre-receive <<-\EOF &&
	exit 1
	EOF
	test_must_fail git push foreign.git HEAD:refs/heads/two &&
	test_path_is_file "$keep"
'

test_expect_success 'a ".keep" installed by a failed migration is removed' '
	test_when_finished rm -rf partial.git &&
	git init --bare partial.git &&
	git -C partial.git config set receive.unpackLimit 0 &&
	git -C partial.git config set pack.indexVersion 1 &&

	# Leave the objects in the main object database without a ref, so
	# that pushing them again produces a pack with the same name.
	test_hook -C partial.git update <<-\EOF &&
	exit 1
	EOF
	test_commit partial &&
	test_must_fail git push partial.git HEAD:refs/heads/one &&

	# The same pack now arrives with a differently formatted index. The
	# ".keep" is migrated first and goes in fine; the index then collides
	# with the one already there, and the migration fails with our
	# ".keep" already installed.
	git -C partial.git config set pack.indexVersion 2 &&
	test_must_fail git push partial.git HEAD:refs/heads/two 2>err &&
	test_grep "unable to migrate" err &&

	pack="$(ls partial.git/objects/pack/pack-*.pack)" &&
	test_path_is_missing "${pack%.pack}.keep"
'

test_done
