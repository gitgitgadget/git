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

test_expect_success 'delimited sections have an empty line before when needed' '
	for f in $(find $GIT_BUILD_DIR/Documentation -name '\''*.adoc'\'')
	do
		awk "
BEGIN { line_length = 0; in_section = 0; section_header = 0;}
/^\+?$/ { line_length = 0; next; }
/^\[.*\]$/ { line_length = 0; next; }
/^ifdef::.*$/ { line_length = 0; next; }
/^[^-\.].*\$/ { line_length = length(\$0); next; }
/^(-{3,}|\.{3,})$/ {
	if (in_section) {
		if (\$0 == section_header) {
			in_section = 0;
		}
		next;
	}
	if (line_length == 0) {
		in_section = 1;
		section_header = \$0;
		next;
	}
	if ((line_length != 0) &&  (length(\$0) != line_length)) {
		print \"section delimiter not preceded by an empty line\";
		print \"File: \" FILENAME;
		print \"Line: \" NR;
	}
	line_length = 0;
	next;
}
END { if (in_section) {print \"section not finished in \" FILENAME;}}
" $f || (echo "awk failed for $f"; return 1) 
	done > out 2> err &&
	test_must_be_empty out
'

test_expect_success 'no multiple parameters in definition list items' '
(cd $GIT_BUILD_DIR && git grep -Er '\''^[ \t]*`?[-a-z0-9.]+`?(, `?[-a-z0-9.]+`?)+(::|;;)$'\'' -- '\''Documentation/**.adoc'\'' || true) > out &&
	test_must_be_empty out
'

test_expect_success 'no synopsis style option names in definition list items' '
(cd $GIT_BUILD_DIR && git grep -Er '\''^`?--\[no-\][a-z0-9-]+`?(\:\:|;;)$'\'' -- '\''Documentation/**.adoc'\'' || true) > out &&
	test_must_be_empty out
'

test_done
