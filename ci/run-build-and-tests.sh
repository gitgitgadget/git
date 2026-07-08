#!/bin/sh
#
# Build and test Git
#

. ${0%/*}/lib.sh

export TEST_CONTRIB_TOO=yes

case "$jobname" in
linux-musl-meson)
	MESONFLAGS="$MESONFLAGS -Drust=disabled"
	export GIT_TEST_USE_SET_E=yes
	;;
almalinux-*|debian-*|fedora-*|linux-*)
	export GIT_TEST_USE_SET_E=yes
	;;
esac

case "$jobname" in
fedora-breaking-changes-musl|linux-breaking-changes)
	export WITH_BREAKING_CHANGES=YesPlease
	MESONFLAGS="$MESONFLAGS -Dbreaking_changes=true"
	;;
linux-TEST-vars)
	export OPENSSL_SHA1_UNSAFE=YesPlease
	export GIT_TEST_SPLIT_INDEX=yes
	export GIT_TEST_FULL_IN_PACK_ARRAY=true
	export GIT_TEST_OE_SIZE=10
	export GIT_TEST_OE_DELTA_SIZE=5
	export GIT_TEST_COMMIT_GRAPH=1
	export GIT_TEST_COMMIT_GRAPH_CHANGED_PATHS=1
	export GIT_TEST_MULTI_PACK_INDEX=1
	export GIT_TEST_MULTI_PACK_INDEX_WRITE_INCREMENTAL=1
	export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=master
	export GIT_TEST_NO_WRITE_REV_INDEX=1
	export GIT_TEST_CHECKOUT_WORKERS=2
	export GIT_TEST_PACK_USE_BITMAP_BOUNDARY_TRAVERSAL=1
	;;
linux-clang)
	export NO_RUST=UnfortunatelyYes
	export GIT_TEST_DEFAULT_HASH=sha1
	;;
linux-sha256)
	export GIT_TEST_DEFAULT_HASH=sha256
	;;
linux-reftable|linux-reftable-leaks|osx-reftable)
	export GIT_TEST_DEFAULT_REF_FORMAT=reftable
	;;

esac

# A hung test produces no output, so the job idles until the CI platform's
# hard timeout with no hint which test stalled. Start a watchdog that dumps
# diagnostics (see dump_stalled_test_state) if the run makes no progress for
# GIT_TEST_STALL_TIMEOUT seconds. Count down in short steps rather than one
# long "sleep" so that when we stop it on the happy path, no long-lived sleep
# is left holding our output open and delaying the job from ending.
stall_timeout="${GIT_TEST_STALL_TIMEOUT:-7200}"
{
	set +x
	remaining=$stall_timeout
	while test 0 -lt "$remaining"
	do
		sleep 5 || exit
		remaining=$((remaining - 5))
	done
	dump_stalled_test_state "$stall_timeout"
} &
stall_watchdog_pid=$!

case "$jobname" in
*-meson)
	group "Configure" meson setup build . \
		--fatal-meson-warnings \
		--warnlevel 2 --werror \
		--wrap-mode nofallback \
		-Dfuzzers=true \
		-Dtest_output_directory="${TEST_OUTPUT_DIRECTORY:-$(pwd)/t}" \
		$MESONFLAGS
	group "Build" meson compile -C build --
	group "Run tests" meson test -C build --print-errorlogs --test-args="$GIT_TEST_OPTS" || (
		./t/aggregate-results.sh "${TEST_OUTPUT_DIRECTORY:-t}/test-results"
		handle_failed_tests
	)
	;;
*)
	group Build make
	group "Run tests" make test ||
	handle_failed_tests
	;;
esac

kill "$stall_watchdog_pid" 2>/dev/null || :

check_unignored_build_artifacts
save_good_tree
