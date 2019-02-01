#!/bin/sh

test_description='test large file handling on windows'
. ./test-lib.sh

test_expect_success EXPENSIVE,SIZE_T_IS_64BIT 'blah blubb' '

	dd if=/dev/zero of=file bs=1M count=4100 &&
	git config core.compression 0 &&
	git config core.looseCompression 0 &&
	git add file &&
	git commit -m msg file &&
	git log --stat &&
	git fsck --verbose --strict --full &&
	git verify-pack .git/objects/pack/*.pack &&
	git gc
'

test_done
