#ifndef FETCH_OBJECT_H
#define FETCH_OBJECT_H

#include "sha1-array.h"

extern int fetch_object(const char *remote_name, const unsigned char *sha1);

extern int fetch_objects(const char *remote_name,
			 const struct oid_array *to_fetch);

#endif
