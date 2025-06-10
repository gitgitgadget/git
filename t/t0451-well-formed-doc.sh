#!/bin/sh

test_description='basic doc format sanity check'

. ./test-lib.sh

test_expect_success 'linkgit macros are well formed' '
	(cd $GIT_BUILD_DIR && git grep -r '\''git[-a-z]*\[[0-9]'\'' -- '\''Documentation/**.adoc'\'') | sed -e '\''/linkgit:git/ d'\'' > out &&
        test_must_be_empty out
'

test_expect_success 'linkgit macros point to existing files' '
	for f in $(cd $GIT_BUILD_DIR && git grep -Ero '\''(linkgit:)[-a-z0-9{}]*'\'' -- '\''Documentation/**.adoc'\'' | cut -f3 -d:| sed -e '\''s/{litdd}/--/g'\'')
        do
		test_path_is_file $GIT_BUILD_DIR/Documentation/${f}.adoc || return 1
	done
'

test_done
