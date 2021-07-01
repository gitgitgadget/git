#!/bin/sh

test_description='diff performance with many pathspecs'
. ./perf-lib.sh

test_perf_default_repo

sizes='1 2 4 8 16 32 64 128 256 512 1024'

test_expect_success 'create pathspec lists' '
	git ls-tree --name-only -r HEAD >all &&
	for i in $sizes; do
		{
			printf "%s\n" -- &&
			head -$i all
		} >ps-$i
	done
'

for i in $sizes; do
	test_perf "size=$i" "
		git rev-list HEAD --stdin <ps-$i >/dev/null
	"
done

test_done
