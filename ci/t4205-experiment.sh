#!/bin/sh
#
# CI EXPERIMENT (not for submission): find out why the EXPENSIVE tests of
# t4205 are ~10x slower on macOS than on Linux.  Run A runs t4205 alone,
# run B runs it in parallel with other tests that need a lot of memory.

machine_info () {
	echo "== $1"
	uname -a
	if test "$(uname -s)" = Darwin
	then
		sysctl hw.memsize hw.logicalcpu vm.swapusage
		vm_stat
		memory_pressure 2>/dev/null | tail -n 1
	else
		nproc
		free -m
	fi
	df -h .
}

cd t || exit 1
export GIT_TEST_LONG=true

machine_info "before"

echo "== run A: t4205 alone"
start=$(date +%s)
./t4205-log-pretty-formats.sh -v 2>&1 |
	grep -E "^== time_step|huge commit|^[[:space:]]+(Elapsed|User time|System time|Maximum resident|Major|Minor|Involuntary)|[0-9.]+ real +[0-9.]+ user|maximum resident set size|page reclaims|page faults|swaps|involuntary context switches|peak memory footprint"
echo "== run A took $(($(date +%s) - start))s"

machine_info "after run A"

echo "== run B: t4205 in parallel with t1007 and t5608"
start=$(date +%s)
prove --timer --jobs 3 \
	t4205-log-pretty-formats.sh t1007-hash-object.sh t5608-clone-2gb.sh
echo "== run B took $(($(date +%s) - start))s"

machine_info "after run B"
