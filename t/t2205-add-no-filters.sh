#!/bin/sh

test_description='git add --no-filters

This test creates a file and a corresponding .gitattributes setup
to ensure the file undergoes a conversion when committed or checked
out.

It then verifies that the conversion happens by default, but does not
when --no-filters is used.'

. ./test-lib.sh

test_expect_success setup '
	echo "* eol=crlf" > .gitattributes &&
	git add .gitattributes &&
	git commit -m initial &&
	printf "test\r\ntest\r\n" > test
'

test_expect_success 'add without --no-filters' '
	original="$(git hash-object --stdin < test)" &&
	converted="$(git hash-object test)" &&
	git add test &&
	git ls-files -s > actual &&
	cat > expected <<-EOF &&
	100644 $(git hash-object .gitattributes) 0	.gitattributes
	100644 $converted 0	test
	EOF
	test_cmp expected actual
'

test_expect_success 'add with --no-filters' '
	git rm -f --cached test &&
	original="$(git hash-object --stdin < test)" &&
	converted="$(git hash-object test)" &&
	git add --no-filters test &&
	git ls-files -s > actual &&
	cat > expected <<-EOF &&
	100644 $(git hash-object .gitattributes) 0	.gitattributes
	100644 $original 0	test
	EOF
	test_cmp expected actual
'

test_done
