#include "bitmap.h"
#include "cache.h"

static enum bitmap_type bitmap_type = INIT_BITMAP_TYPE;

void set_bitmap_type(enum bitmap_type type)
{
	bitmap_type = type;
}

enum bitmap_type get_bitmap_type(void)
{
	return bitmap_type;
}

void *roaring_or_ewah_bitmap_init(void)
{
	switch (bitmap_type)
	{
	case EWAH:
		return ewah_new();
	case ROARING:
		return roaring_bitmap_create();
	default:
		error(_("bitmap type not initialized\n"));
		return NULL;
	}
}

void *roaring_or_raw_bitmap_new(void)
{
	switch (bitmap_type)
	{
	case EWAH:
		return raw_bitmap_new();
	case ROARING:
		return roaring_bitmap_create();
	default:
		error(_("bitmap type not initialized\n"));
			return NULL;
	}
}

void *roaring_or_raw_bitmap_copy(void *bitmap)
{
	switch (bitmap_type)
	{
	case EWAH:
		return raw_bitmap_dup(bitmap);
	case ROARING:
		return roaring_bitmap_copy(bitmap);
	default:
		error(_("bitmap type not initialized\n"));
			return NULL;
	}
}

int roaring_or_ewah_bitmap_set(void *bitmap, uint32_t i)
{
	switch (bitmap_type) {
	case EWAH:
		ewah_set(bitmap, i);
		break;
	case ROARING:
		roaring_bitmap_add(bitmap, i);
		break;
	default:
		return error(_("bitmap type not initialized\n"));
	}

	return 0;
}

void roaring_or_raw_bitmap_set(void *bitmap, uint32_t i)
{
	switch (bitmap_type)
	{
	case EWAH:
		raw_bitmap_set(bitmap, i);
		break;
	case ROARING:
		roaring_bitmap_add(bitmap, i);
		break;
	default:
		error(_("bitmap type not initialized\n"));
	}
}

void roaring_or_raw_bitmap_unset(void *bitmap, uint32_t i)
{
	switch (bitmap_type)
	{
	case EWAH:
		raw_bitmap_unset(bitmap, i);
		break;
	case ROARING:
		roaring_bitmap_remove(bitmap, i);
	default:
		break;
	}
}

int roaring_or_raw_bitmap_get(void *bitmap, uint32_t pos)
{
	switch (bitmap_type)
	{
	case EWAH:
		return raw_bitmap_get(bitmap, pos);
	case ROARING:
		return roaring_bitmap_contains(bitmap, pos);
	default:
		return error(_("bitmap type not initialized\n"));
	}
}

int roaring_or_raw_bitmap_equals(void *a, void *b)
{
	switch (bitmap_type)
	{
	case EWAH:
		return raw_bitmap_equals(a, b);
		break;
	case ROARING:
		return roaring_bitmap_equals(a, b);
	default:
		return error(_("bitmap type not initialized\n"));
	}
}

size_t roaring_or_raw_bitmap_cardinality(void *bitmap)
{
	switch (bitmap_type)
	{
	case EWAH:
		return raw_bitmap_popcount(bitmap);
	case ROARING:
		return roaring_bitmap_get_cardinality(bitmap);
	default:
		return error(_("bitmap type not initialized\n"));
	}
}

int roaring_or_raw_bitmap_is_subset(void *a, void *b)
{
	switch (bitmap_type) {
	case EWAH:
		return raw_bitmap_is_subset(a, b);
	case ROARING:
		return roaring_bitmap_andnot_cardinality(a, b) ? 1: 0;
	default:
		return error(_("bitmap type not initialized\n"));
	}
}

void roaring_or_raw_bitmap_printf(void *a)
{
	switch (bitmap_type) {
	case EWAH:
		ewah_bitmap_print(a);
		return;
	case ROARING:
		roaring_bitmap_printf(a);
		return;
	}
}

void roaring_or_raw_bitmap_or(void *self, void *other)
{
	switch (bitmap_type)
	{
	case EWAH:
		raw_bitmap_or(self, other);
		break;
	case ROARING:
		roaring_bitmap_or_inplace(self, other);
		break;
	default:
		error(_("bitmap type not initialized\n"));
	}
}

void roaring_or_raw_bitmap_and_not(void *self, void *other)
{
	switch (bitmap_type)
	{
	case EWAH:
		raw_bitmap_and_not(self, other);
		break;
	case ROARING:
		roaring_bitmap_andnot_inplace(self, other);
		break;
	default:
		error(_("bitmap type not initialized\n"));
	}
}

void roaring_or_raw_bitmap_free(void *bitmap)
{
	switch (bitmap_type)
	{
	case EWAH:
		raw_bitmap_free(bitmap);
		break;
	case ROARING:
		roaring_bitmap_free(bitmap);
		break;
	default:
		error(_("bitmap type not initialized\n"));
	}
}

void roaring_or_raw_bitmap_free_safe(void **bitmap)
{
	switch (bitmap_type)
	{
	case EWAH:
		raw_bitmap_free(*bitmap);
		break;
	case ROARING:
		roaring_bitmap_free_safe((roaring_bitmap_t **)bitmap);
		break;
	default:
		error(_("bitmap type not initialized\n"));
	}
}