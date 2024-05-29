/*
Copyright 2020 Google LLC

Use of this source code is governed by a BSD-style
license that can be found in the LICENSE file or at
https://developers.google.com/open-source/licenses/bsd
*/

#include "test-lib.h"
#include "reftable/tree.h"

static int test_compare(const void *a, const void *b)
{
	return (char *)a - (char *)b;
}

struct curry {
	void *last;
};

static void check_increasing(void *arg, void *key)
{
	struct curry *c = arg;
	if (c->last)
		check_int(test_compare(c->last, key), <, 0);
	c->last = key;
}

static void test_tree_search(void)
{
	struct tree_node *root = NULL;

	void *values[11] = { NULL };
	struct tree_node *nodes[11] = { NULL };
	int i = 1;
	do {
		nodes[i] = tree_search(values + i, &root, &test_compare, 1);
		i = (i * 7) % 11;
	} while (i != 1);

	for (i = 1; i < ARRAY_SIZE(nodes); i++) {
		check(values + i == nodes[i]->key);
		check(nodes[i] == tree_search(values + i, &root, &test_compare, 0));
	}

	tree_free(root);
}

static void test_infix_walk(void)
{
	struct tree_node *root = NULL;

	void *values[13] = { NULL };
	struct curry c = { NULL };
	int i = 1;
	do {
		tree_search(values + i, &root, &test_compare, 1);
		i = (i * 5) % 13;
	} while (i != 1);

	infix_walk(root, &check_increasing, &c);
	tree_free(root);
}

int cmd_main(int argc, const char *argv[])
{
	TEST(test_tree_search(), "tree_search works");
	TEST(test_infix_walk(), "infix_walk works");

	return test_done();
}
