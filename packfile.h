#ifndef PACKFILE_H
#define PACKFILE_H

#include "list.h"
#include "object.h"
#include "odb.h"
#include "oidset.h"

/* in odb.h */
struct object_info;

struct packed_git {
	struct hashmap_entry packmap_ent;
	struct packed_git *next;
	struct list_head mru;
	struct pack_window *windows;
	off_t pack_size;
	const void *index_data;
	size_t index_size;
	uint32_t num_objects;
	size_t crc_offset;
	struct oidset bad_objects;
	int index_version;
	time_t mtime;
	int pack_fd;
	int index;              /* for builtin/pack-objects.c */
	unsigned pack_local:1,
		 pack_keep:1,
		 pack_keep_in_core:1,
		 freshened:1,
		 do_not_close:1,
		 pack_promisor:1,
		 multi_pack_index:1,
		 is_cruft:1;
	unsigned char hash[GIT_MAX_RAWSZ];
	struct revindex_entry *revindex;
	const uint32_t *revindex_data;
	const uint32_t *revindex_map;
	size_t revindex_size;
	/*
	 * mtimes_map points at the beginning of the memory mapped region of
	 * this pack's corresponding .mtimes file, and mtimes_size is the size
	 * of that .mtimes file
	 */
	const uint32_t *mtimes_map;
	size_t mtimes_size;

	/* repo denotes the repository this packfile belongs to */
	struct repository *repo;

	/* something like ".git/objects/pack/xxxxx.pack" */
	char pack_name[FLEX_ARRAY]; /* more */
};

static inline int pack_map_entry_cmp(const void *cmp_data UNUSED,
				     const struct hashmap_entry *entry,
				     const struct hashmap_entry *entry2,
				     const void *keydata)
{
	const char *key = keydata;
	const struct packed_git *pg1, *pg2;

	pg1 = container_of(entry, const struct packed_git, packmap_ent);
	pg2 = container_of(entry2, const struct packed_git, packmap_ent);

	return strcmp(pg1->pack_name, key ? key : pg2->pack_name);
}

struct pack_window {
	struct pack_window *next;
	unsigned char *base;
	off_t offset;
	size_t len;
	unsigned int last_used;
	unsigned int inuse_cnt;
};

struct pack_entry {
	off_t offset;
	struct packed_git *p;
};

/*
 * Generate the filename to be used for a pack file with checksum "sha1" and
 * extension "ext". The result is written into the strbuf "buf", overwriting
 * any existing contents. A pointer to buf->buf is returned as a convenience.
 *
 * Example: odb_pack_name(out, sha1, "idx") => ".git/objects/pack/pack-1234..idx"
 */
char *odb_pack_name(struct repository *r, struct strbuf *buf,
		    const unsigned char *hash, const char *ext);

/*
 * Return the basename of the packfile, omitting any containing directory
 * (e.g., "pack-1234abcd[...].pack").
 */
const char *pack_basename(struct packed_git *p);

/*
 * Parse the pack idx file found at idx_path and create a packed_git struct
 * which can be used with find_pack_entry_one().
 *
 * You probably don't want to use this function! It skips most of the normal
 * sanity checks (including whether we even have the matching .pack file),
 * and does not add the resulting packed_git struct to the internal list of
 * packs. You probably want add_packed_git() instead.
 */
struct packed_git *parse_pack_index(struct repository *r, unsigned char *sha1,
				    const char *idx_path);

typedef void each_file_in_pack_dir_fn(const char *full_path, size_t full_path_len,
				      const char *file_name, void *data);
void for_each_file_in_pack_subdir(const char *objdir,
				  const char *subdir,
				  each_file_in_pack_dir_fn fn,
				  void *data);
void for_each_file_in_pack_dir(const char *objdir,
			       each_file_in_pack_dir_fn fn,
			       void *data);

/*
 * Iterate over all accessible packed objects without respect to reachability.
 * By default, this includes both local and alternate packs.
 *
 * Note that some objects may appear twice if they are found in multiple packs.
 * Each pack is visited in an unspecified order. By default, objects within a
 * pack are visited in pack-idx order (i.e., sorted by oid).
 */
typedef int each_packed_object_fn(const struct object_id *oid,
				  struct packed_git *pack,
				  uint32_t pos,
				  void *data);
int for_each_object_in_pack(struct packed_git *p,
			    each_packed_object_fn, void *data,
			    enum for_each_object_flags flags);
int for_each_packed_object(struct repository *repo, each_packed_object_fn cb,
			   void *data, enum for_each_object_flags flags);

/* A hook to report invalid files in pack directory */
#define PACKDIR_FILE_PACK 1
#define PACKDIR_FILE_IDX 2
#define PACKDIR_FILE_GARBAGE 4
extern void (*report_garbage)(unsigned seen_bits, const char *path);

void reprepare_packed_git(struct repository *r);
void install_packed_git(struct repository *r, struct packed_git *pack);

struct packed_git *get_packed_git(struct repository *r);
struct list_head *get_packed_git_mru(struct repository *r);
struct multi_pack_index *get_multi_pack_index(struct repository *r);
struct multi_pack_index *get_local_multi_pack_index(struct repository *r);
struct packed_git *get_all_packs(struct repository *r);

/*
 * Give a rough count of objects in the repository. This sacrifices accuracy
 * for speed.
 */
unsigned long repo_approximate_object_count(struct repository *r);

/*
 * Find the pack within the "packs" list whose index contains the object "oid".
 * For general object lookups, you probably don't want this; use
 * find_pack_entry() instead.
 */
struct packed_git *find_oid_pack(const struct object_id *oid,
				 struct packed_git *packs);

void pack_report(struct repository *repo);

/*
 * mmap the index file for the specified packfile (if it is not
 * already mmapped).  Return 0 on success.
 */
int open_pack_index(struct packed_git *);

/*
 * munmap the index file for the specified packfile (if it is
 * currently mmapped).
 */
void close_pack_index(struct packed_git *);

int close_pack_fd(struct packed_git *p);

uint32_t get_pack_fanout(struct packed_git *p, uint32_t value);

struct object_database;

unsigned char *use_pack(struct packed_git *, struct pack_window **, off_t, unsigned long *);
void close_pack_windows(struct packed_git *);
void close_pack(struct packed_git *);
void close_object_store(struct object_database *o);
void unuse_pack(struct pack_window **);
void clear_delta_base_cache(void);
struct packed_git *add_packed_git(struct repository *r, const char *path,
				  size_t path_len, int local);

/*
 * Unlink the .pack and associated extension files.
 * Does not unlink if 'force_delete' is false and the pack-file is
 * marked as ".keep".
 */
void unlink_pack_path(const char *pack_name, int force_delete);

/*
 * Make sure that a pointer access into an mmap'd index file is within bounds,
 * and can provide at least 8 bytes of data.
 *
 * Note that this is only necessary for variable-length segments of the file
 * (like the 64-bit extended offset table), as we compare the size to the
 * fixed-length parts when we open the file.
 */
void check_pack_index_ptr(const struct packed_git *p, const void *ptr);

/*
 * Perform binary search on a pack-index for a given oid. Packfile is expected to
 * have a valid pack-index.
 *
 * See 'bsearch_hash' for more information.
 */
int bsearch_pack(const struct object_id *oid, const struct packed_git *p, uint32_t *result);

/*
 * Write the oid of the nth object within the specified packfile into the first
 * parameter. Open the index if it is not already open.  Returns 0 on success,
 * negative otherwise.
 */
int nth_packed_object_id(struct object_id *, struct packed_git *, uint32_t n);

/*
 * Return the offset of the nth object within the specified packfile.
 * The index must already be opened.
 */
off_t nth_packed_object_offset(const struct packed_git *, uint32_t n);

/*
 * If the object named by oid is present in the specified packfile,
 * return its offset within the packfile; otherwise, return 0.
 */
off_t find_pack_entry_one(const struct object_id *oid, struct packed_git *);

int is_pack_valid(struct packed_git *);
void *unpack_entry(struct repository *r, struct packed_git *, off_t, enum object_type *, unsigned long *);
unsigned long unpack_object_header_buffer(const unsigned char *buf, unsigned long len, enum object_type *type, unsigned long *sizep);
unsigned long get_size_from_delta(struct packed_git *, struct pack_window **, off_t);
int unpack_object_header(struct packed_git *, struct pack_window **, off_t *, unsigned long *);
off_t get_delta_base(struct packed_git *p, struct pack_window **w_curs,
		     off_t *curpos, enum object_type type,
		     off_t delta_obj_offset);

void release_pack_memory(size_t);

/* global flag to enable extra checks when accessing packed objects */
extern int do_check_packed_object_crc;

int packed_object_info(struct repository *r,
		       struct packed_git *pack,
		       off_t offset, struct object_info *);

void mark_bad_packed_object(struct packed_git *, const struct object_id *);
const struct packed_git *has_packed_and_bad(struct repository *, const struct object_id *);

#define ON_DISK_KEEP_PACKS 1
#define IN_CORE_KEEP_PACKS 2

/*
 * Iff a pack file in the given repository contains the object named by sha1,
 * return true and store its location to e.
 */
int find_pack_entry(struct repository *r, const struct object_id *oid, struct pack_entry *e);
int find_kept_pack_entry(struct repository *r, const struct object_id *oid, unsigned flags, struct pack_entry *e);

int has_object_pack(struct repository *r, const struct object_id *oid);
int has_object_kept_pack(struct repository *r, const struct object_id *oid,
			 unsigned flags);

struct packed_git **kept_pack_cache(struct repository *r, unsigned flags);

/*
 * Return 1 if an object in a promisor packfile is or refers to the given
 * object, 0 otherwise.
 */
int is_promisor_object(struct repository *r, const struct object_id *oid);

/*
 * Expose a function for fuzz testing.
 *
 * load_idx() parses a block of memory as a packfile index and puts the results
 * into a struct packed_git.
 *
 * This function should not be used directly. It is exposed here only so that we
 * have a convenient entry-point for fuzz testing. For real uses, you should
 * probably use open_pack_index() instead.
 */
int load_idx(const char *path, const unsigned int hashsz, void *idx_map,
	     size_t idx_size, struct packed_git *p);

/*
 * Parse a --pack_header option as accepted by index-pack and unpack-objects,
 * turning it into the matching bytes we'd find in a pack.
 */
int parse_pack_header_option(const char *in, unsigned char *out, unsigned int *len);

#endif
