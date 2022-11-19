#ifndef METACOMMIT_PARSER_H
#define METACOMMIT_PARSER_H

#include "commit.h"
#include "hash.h"

enum metacommit_type {
	/* Indicates a normal commit (non-metacommit) */
	METACOMMIT_TYPE_NONE = 0,
	/* Indicates a metacommit with normal content (non-abandoned) */
	METACOMMIT_TYPE_NORMAL = 1,
	/* Indicates a metacommit with abandoned content */
	METACOMMIT_TYPE_ABANDONED = 2,
};

enum metacommit_type get_metacommit_content(struct commit *commit,
					    struct object_id *content);

#endif
