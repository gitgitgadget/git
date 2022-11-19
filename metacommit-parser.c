#include "cache.h"
#include "metacommit-parser.h"
#include "commit.h"

/*
 * Search the commit buffer for a line starting with the given key. Unlike
 * find_commit_header, this also searches the commit message body.
 */
static const char *find_key(const char *msg, const char *key, size_t *out_len)
{
	int key_len = strlen(key);
	const char *line = msg;

	while (line) {
		const char *eol = strchrnul(line, '\n');

		if (eol - line > key_len && !memcmp(line, key, key_len) &&
		    line[key_len] == ' ') {
			*out_len = eol - line - key_len - 1;
			return line + key_len + 1;
		}
		line = *eol ? eol + 1 : NULL;
	}
	return NULL;
}

/*
 * Writes the index of the content parent to "result". Returns the metacommit
 * type. See the METACOMMIT_TYPE_* constants.
 */
static enum metacommit_type index_of_content_commit(const char *buffer,
						    int *result)
{
	int index = 0;
	int ret = METACOMMIT_TYPE_NONE;
	size_t parent_types_size;
	const char *parent_types =
		find_key(buffer, "parent-type", &parent_types_size);
	const char *end;
	const char *enum_start = parent_types;
	int enum_length = 0;

	if (!parent_types)
		return METACOMMIT_TYPE_NONE;

	end = &parent_types[parent_types_size];

	while (1) {
		char next = *parent_types;
		if (next == ' ' || parent_types >= end) {
			if (enum_length == 1) {
				char type = *enum_start;
				if (type == 'c') {
					ret = METACOMMIT_TYPE_NORMAL;
					break;
				}
				if (type == 'a') {
					ret = METACOMMIT_TYPE_ABANDONED;
					break;
				}
			}
			if (parent_types >= end)
				return METACOMMIT_TYPE_NONE;
			enum_start = parent_types + 1;
			enum_length = 0;
			index++;
		} else {
			enum_length++;
		}
		parent_types++;
	}

	*result = index;
	return ret;
}

/*
 * Writes the content parent's object id to "content".
 * Returns the metacommit type. See the METACOMMIT_TYPE_* constants.
 */
enum metacommit_type get_metacommit_content(struct commit *commit,
					    struct object_id *content)
{
	const char *buffer = get_commit_buffer(commit, NULL);
	int index = 0;
	enum metacommit_type ret = index_of_content_commit(buffer, &index);
	struct commit *content_parent;

	if (ret == METACOMMIT_TYPE_NONE)
		return ret;

	content_parent = get_commit_by_index(commit->parents, index);

	if (!content_parent)
		return METACOMMIT_TYPE_NONE;

	oidcpy(content, &(content_parent->object.oid));
	return ret;
}
