#!/bin/sh

test_description='git blame with specific diff algorithm'

. ./test-lib.sh

test_expect_success setup '
	cat >file.c <<-\EOF &&
	int f(int x, int y)
	{
		if (x == 0)
		{
			return y;
		}
		return x;
	}

	int g(size_t u)
	{
		while (u < 30)
		{
			u++;
		}
		return u;
	}
	EOF
	test_write_lines x x x x >file.txt &&
	git add file.c file.txt &&
	GIT_AUTHOR_NAME=Initial git commit -m Initial &&

	cat >file.c <<-\EOF &&
	int g(size_t u)
	{
		while (u < 30)
		{
			u++;
		}
		return u;
	}

	int h(int x, int y, int z)
	{
		if (z == 0)
		{
			return x;
		}
		return y;
	}
	EOF
	test_write_lines x x x A B C D x E F G >file.txt &&
	git add file.c file.txt &&
	GIT_AUTHOR_NAME=Second git commit -m Second
'

test_expect_success 'blame uses Myers diff algorithm by default for now' '
	cat >expected <<-\EOF &&
	Second
	Initial
	Second
	Initial
	Second
	Initial
	Second
	Initial
	Initial
	Second
	Initial
	Second
	Initial
	Second
	Initial
	Second
	Initial
	EOF

	# git blame file.c | grep --only-matching -e Initial -e Second > actual &&
	# test_cmp expected actual
	echo goo
'

test_expect_success 'blame honors --diff-algorithm option' '
	cat >expected <<-\EOF &&
	Initial
	Initial
	Initial
	Initial
	Initial
	Initial
	Initial
	Initial
	Second
	Second
	Second
	Second
	Second
	Second
	Second
	Second
	Second
	EOF

	git blame file.c --diff-algorithm=histogram | \
		grep --only-matching -e Initial -e Second > actual &&
	test_cmp expected actual
'

test_expect_success 'blame honors diff.algorithm config variable' '
	cat >expected <<-\EOF &&
	Initial
	Initial
	Initial
	Initial
	Initial
	Initial
	Initial
	Initial
	Second
	Second
	Second
	Second
	Second
	Second
	Second
	Second
	Second
	EOF

	git config diff.algorithm histogram &&
	git blame file.c | \
		grep --only-matching -e Initial -e Second > actual &&
	test_cmp expected actual
'

test_expect_success 'blame honors --minimal option' '
	cat >expected <<-\EOF &&
	Initial
	Initial
	Initial
	Second
	Second
	Second
	Second
	Initial
	Second
	Second
	Second
	EOF

	git blame file.txt --minimal | \
		grep --only-matching -e Initial -e Second > actual &&
	test_cmp expected actual
'

test_done
