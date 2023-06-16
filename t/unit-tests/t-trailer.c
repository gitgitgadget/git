#include "test-lib.h"
#include "trailer.h"

static void t_token_len_without_separator(const char *token, size_t expected)
{
	size_t result;
	result = token_len_without_separator(token, strlen(token));
	check_uint(result, ==, expected);
}

static void t_after_or_end(enum trailer_where where, int expected)
{
	size_t result;
	result = after_or_end(where);
	check_int(result, ==, expected);
}


static void t_same_token(char *a, char *b, int expected)
{
    struct trailer_item trailer_item = { .token = a };
	struct arg_item arg_item = { .token = b };
	size_t result;
	result = same_token(&trailer_item, &arg_item);
	check_int(result, ==, expected);
}

int cmd_main(int argc, const char **argv)
{
	TEST(t_same_token("", "", 1),
		 "empty trailer_item and arg_item");
	TEST(t_same_token("foo", "foo", 1),
		 "identical");
	TEST(t_same_token("foo", "FOO", 1),
		 "case should not matter");
	TEST(t_same_token("foo", "foobar", 1),
		 "arg_item is longer than trailer_item");
	TEST(t_same_token("foobar", "foo", 1),
		 "trailer_item is longer than arg_item");
	TEST(t_same_token("foo", "bar", 0),
		 "no similarity");

	TEST(t_after_or_end(WHERE_AFTER, 1), "accept WHERE_AFTER");
	TEST(t_after_or_end(WHERE_END, 1), "accept WHERE_END");
	TEST(t_after_or_end(WHERE_DEFAULT, 0), "reject WHERE_END");

	TEST(t_token_len_without_separator("Signed-off-by:", 13),
	     "token with trailing punctuation (colon)");
	TEST(t_token_len_without_separator("Signed-off-by", 13),
	     "token without trailing punctuation");
	TEST(t_token_len_without_separator("Foo bar:", 7),
	     "token with spaces with trailing punctuation (colon)");
	TEST(t_token_len_without_separator("Foo bar", 7),
	     "token with spaces without trailing punctuation");
	TEST(t_token_len_without_separator("-Foo bar:", 8),
	     "token with leading non-separator punctuation");
	TEST(t_token_len_without_separator("- Foo bar:", 9),
	     "token with leading non-separator punctuation and space");
	TEST(t_token_len_without_separator("F:", 1),
	     "one-letter token");
	TEST(t_token_len_without_separator("abc%de#f@", 8),
	     "token with punctuation in-between");
	TEST(t_token_len_without_separator(":;*%_.#f@", 8),
	     "token with multiple leading punctuation chars");

	/*
	 * These tests fail unexpectedly. They are probably bugs, although it may be
	 * the case that these bugs never bubble up to the user because of other
	 * checks we do elsewhere up the stack.
	 */
	TEST(t_same_token("", "foo", 0),
		"empty trailer_item");
	TEST(t_same_token("foo", "", 0),
		"empty arg_item");

	return test_done();
}
