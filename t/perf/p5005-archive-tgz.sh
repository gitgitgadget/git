#!/bin/sh

test_description='Test archive --format=tgz performance'

. ./perf-lib.sh

test_perf_default_repo

test_perf 'archive --format=tgz' '
	git archive --format=tgz HEAD >/dev/null
'

test_done

