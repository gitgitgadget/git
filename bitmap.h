#ifndef __BITMAP_H__
#define __BITMAP_H__


#include "git-compat-util.h"
#include "ewah/ewok.h"
#include "roaring/roaring.h"

enum bitmap_type {
	INIT_BITMAP_TYPE = 0,
	EWAH,
	ROARING
};

enum bitmap_type get_bitmap_type(void);
void set_bitmap_type(enum bitmap_type type);
void *roaring_or_ewah_bitmap_init(void);
void *roaring_or_raw_bitmap_new(void);
void *roaring_or_raw_bitmap_copy(void *bitmap);
int roaring_or_ewah_bitmap_set(void *bitmap, uint32_t i);
void roaring_or_raw_bitmap_set(void *bitmap, uint32_t i);
int roaring_or_raw_bitmap_get(void *bitmap, uint32_t pos);
int roaring_or_raw_bitmap_is_subset(void *a, void *b);
void roaring_or_raw_bitmap_or(void *self, void *other);
void roaring_or_raw_bitmap_free(void *bitmap);
void roaring_or_raw_bitmap_free_safe(void **bitmap);
void roaring_or_raw_bitmap_unset(void *bitmap, uint32_t i);
void roaring_or_raw_bitmap_printf(void *a);
int roaring_or_raw_bitmap_equals(void *a, void *b);
void roaring_or_raw_bitmap_and_not(void *self, void *other);
size_t roaring_or_raw_bitmap_cardinality(void *bitmap);

#endif
