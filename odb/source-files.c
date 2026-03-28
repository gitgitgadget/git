#include "git-compat-util.h"
#include "abspath.h"
#include "chdir-notify.h"
#include "config.h"
#include "gettext.h"
#include "lockfile.h"
#include "loose.h"
#include "midx.h"
#include "object-file.h"
#include "odb.h"
#include "odb/source.h"
#include "odb/source-files.h"
#include "pack-objects.h"
#include "packfile.h"
#include "run-command.h"
#include "strbuf.h"
#include "strvec.h"
#include "oidtree.h"
#include "write-or-die.h"

static void odb_source_files_reparent(const char *name UNUSED,
				      const char *old_cwd,
				      const char *new_cwd,
				      void *cb_data)
{
	struct odb_source_files *files = cb_data;
	char *path = reparent_relative_path(old_cwd, new_cwd,
					    files->base.path);
	free(files->base.path);
	files->base.path = path;
}

static void odb_source_files_free(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	chdir_notify_unregister(NULL, odb_source_files_reparent, files);
	odb_source_loose_free(files->loose);
	packfile_store_free(files->packed);
	odb_source_release(&files->base);
	free(files);
}

static void odb_source_files_close(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	packfile_store_close(files->packed);
}

static void odb_source_files_reprepare(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	odb_source_loose_reprepare(&files->base);
	packfile_store_reprepare(files->packed);
}

static int odb_source_files_read_object_info(struct odb_source *source,
					     const struct object_id *oid,
					     struct object_info *oi,
					     enum object_info_flags flags)
{
	struct odb_source_files *files = odb_source_files_downcast(source);

	if (!packfile_store_read_object_info(files->packed, oid, oi, flags) ||
	    !odb_source_loose_read_object_info(source, oid, oi, flags))
		return 0;

	return -1;
}

static int odb_source_files_read_object_stream(struct odb_read_stream **out,
					       struct odb_source *source,
					       const struct object_id *oid)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	if (!packfile_store_read_object_stream(out, files->packed, oid) ||
	    !odb_source_loose_read_object_stream(out, source, oid))
		return 0;
	return -1;
}

static int odb_source_files_for_each_object(struct odb_source *source,
					    const struct object_info *request,
					    odb_for_each_object_cb cb,
					    void *cb_data,
					    unsigned flags)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	int ret;

	if (!(flags & ODB_FOR_EACH_OBJECT_PROMISOR_ONLY)) {
		ret = odb_source_loose_for_each_object(source, request, cb, cb_data, flags);
		if (ret)
			return ret;
	}

	ret = packfile_store_for_each_object(files->packed, request, cb, cb_data, flags);
	if (ret)
		return ret;

	return 0;
}

static int odb_source_files_count_objects(struct odb_source *source,
					  enum odb_count_objects_flags flags,
					  unsigned long *out)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	unsigned long count;
	int ret;

	ret = packfile_store_count_objects(files->packed, flags, &count);
	if (ret < 0)
		goto out;

	if (!(flags & ODB_COUNT_OBJECTS_APPROXIMATE)) {
		unsigned long loose_count;

		ret = odb_source_loose_count_objects(source, flags, &loose_count);
		if (ret < 0)
			goto out;

		count += loose_count;
	}

	*out = count;
	ret = 0;

out:
	return ret;
}

static int odb_source_files_freshen_object(struct odb_source *source,
					   const struct object_id *oid)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	if (packfile_store_freshen_object(files->packed, oid) ||
	    odb_source_loose_freshen_object(source, oid))
		return 1;
	return 0;
}

static int odb_source_files_write_object(struct odb_source *source,
					 const void *buf, unsigned long len,
					 enum object_type type,
					 struct object_id *oid,
					 struct object_id *compat_oid,
					 unsigned flags)
{
	return odb_source_loose_write_object(source, buf, len, type,
					     oid, compat_oid, flags);
}

static int odb_source_files_write_object_stream(struct odb_source *source,
						struct odb_write_stream *stream,
						size_t len,
						struct object_id *oid)
{
	return odb_source_loose_write_stream(source, stream, len, oid);
}

static int odb_source_files_begin_transaction(struct odb_source *source,
					      struct odb_transaction **out)
{
	struct odb_transaction *tx = odb_transaction_files_begin(source);
	if (!tx)
		return -1;
	*out = tx;
	return 0;
}

static int odb_source_files_read_alternates(struct odb_source *source,
					    struct strvec *out)
{
	struct strbuf buf = STRBUF_INIT;
	char *path;

	path = xstrfmt("%s/info/alternates", source->path);
	if (strbuf_read_file(&buf, path, 1024) < 0) {
		warn_on_fopen_errors(path);
		free(path);
		return 0;
	}
	parse_alternates(buf.buf, '\n', source->path, out);

	strbuf_release(&buf);
	free(path);
	return 0;
}

static int odb_source_files_write_alternate(struct odb_source *source,
					    const char *alternate)
{
	struct lock_file lock = LOCK_INIT;
	char *path = xstrfmt("%s/%s", source->path, "info/alternates");
	FILE *in, *out;
	int found = 0;
	int ret;

	hold_lock_file_for_update(&lock, path, LOCK_DIE_ON_ERROR);
	out = fdopen_lock_file(&lock, "w");
	if (!out) {
		ret = error_errno(_("unable to fdopen alternates lockfile"));
		goto out;
	}

	in = fopen(path, "r");
	if (in) {
		struct strbuf line = STRBUF_INIT;

		while (strbuf_getline(&line, in) != EOF) {
			if (!strcmp(alternate, line.buf)) {
				found = 1;
				break;
			}
			fprintf_or_die(out, "%s\n", line.buf);
		}

		strbuf_release(&line);
		fclose(in);
	} else if (errno != ENOENT) {
		ret = error_errno(_("unable to read alternates file"));
		goto out;
	}

	if (found) {
		rollback_lock_file(&lock);
	} else {
		fprintf_or_die(out, "%s\n", alternate);
		if (commit_lock_file(&lock)) {
			ret = error_errno(_("unable to move new alternates file into place"));
			goto out;
		}
	}

	ret = 0;

out:
	free(path);
	return ret;
}

static int odb_source_files_write_packfile(struct odb_source *source,
					   int pack_fd,
					   struct odb_write_packfile_options *opts)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct child_process cmd = CHILD_PROCESS_INIT;
	int fsck_objects = 0;
	int use_index_pack = 1;
	int ret;

	if (opts && opts->nr_objects) {
		int transfer_unpack_limit = -1;
		int fetch_unpack_limit = -1;
		int unpack_limit = 100;

		repo_config_get_int(source->odb->repo, "fetch.unpacklimit",
				    &fetch_unpack_limit);
		repo_config_get_int(source->odb->repo, "transfer.unpacklimit",
				    &transfer_unpack_limit);
		if (0 <= fetch_unpack_limit)
			unpack_limit = fetch_unpack_limit;
		else if (0 <= transfer_unpack_limit)
			unpack_limit = transfer_unpack_limit;

		if (opts->nr_objects < (unsigned int)unpack_limit &&
		    !opts->from_promisor && !opts->lockfile_out)
			use_index_pack = 0;
	}

	cmd.in = pack_fd;
	cmd.git_cmd = 1;

	if (!use_index_pack) {
		strvec_push(&cmd.args, "unpack-objects");
		if (opts && opts->quiet)
			strvec_push(&cmd.args, "-q");
		if (opts && opts->pack_header_version)
			strvec_pushf(&cmd.args, "--pack_header=%"PRIu32",%"PRIu32,
				     opts->pack_header_version,
				     opts->pack_header_entries);
		repo_config_get_bool(source->odb->repo, "transfer.fsckobjects",
				     &fsck_objects);
		repo_config_get_bool(source->odb->repo, "receive.fsckobjects",
				     &fsck_objects);
		if (fsck_objects)
			strvec_push(&cmd.args, "--strict");
		if (opts && opts->max_input_size)
			strvec_pushf(&cmd.args, "--max-input-size=%lu",
				     opts->max_input_size);
		ret = run_command(&cmd);
		if (ret)
			return error(_("unpack-objects failed"));
		return 0;
	}

	strvec_push(&cmd.args, "index-pack");
	strvec_push(&cmd.args, "--stdin");
	strvec_push(&cmd.args, "--keep=write_packfile");

	if (opts && opts->pack_header_version)
		strvec_pushf(&cmd.args, "--pack_header=%"PRIu32",%"PRIu32,
			     opts->pack_header_version,
			     opts->pack_header_entries);

	if (opts) {
		if (opts->use_thin_pack)
			strvec_push(&cmd.args, "--fix-thin");
		if (opts->from_promisor)
			strvec_push(&cmd.args, "--promisor");
		if (opts->check_self_contained)
			strvec_push(&cmd.args, "--check-self-contained-and-connected");
		if (opts->max_input_size)
			strvec_pushf(&cmd.args, "--max-input-size=%lu",
				     opts->max_input_size);
		if (opts->shallow_file)
			strvec_pushf(&cmd.env, "GIT_SHALLOW_FILE=%s",
				     opts->shallow_file);
		if (opts->report_end_of_input)
			strvec_push(&cmd.args, "--report-end-of-input");
		if (opts->fsck_objects)
			fsck_objects = 1;
	}

	if (!fsck_objects) {
		repo_config_get_bool(source->odb->repo, "transfer.fsckobjects",
				     &fsck_objects);
		repo_config_get_bool(source->odb->repo, "fetch.fsckobjects",
				     &fsck_objects);
	}
	if (fsck_objects)
		strvec_push(&cmd.args, "--strict");

	if (opts && opts->lockfile_out) {
		cmd.out = -1;
		ret = start_command(&cmd);
		if (ret)
			return error(_("index-pack failed to start"));
		*opts->lockfile_out = index_pack_lockfile(source->odb->repo,
							  cmd.out, NULL);
		close(cmd.out);
		ret = finish_command(&cmd);
	} else {
		ret = run_command(&cmd);
	}

	if (ret)
		return error(_("index-pack failed"));

	if (opts && opts->check_self_contained)
		opts->self_contained_out = 1;

	packfile_store_reprepare(files->packed);
	return 0;
}

static int match_hash_prefix(unsigned len, const unsigned char *a,
			     const unsigned char *b)
{
	while (len > 1) {
		if (*a != *b)
			return 0;
		a++; b++; len -= 2;
	}
	if (len)
		if ((*a ^ *b) & 0xf0)
			return 0;
	return 1;
}

struct abbrev_cb_data {
	odb_for_each_object_cb cb;
	void *cb_data;
	int ret;
};

static enum cb_next abbrev_loose_cb(const struct object_id *oid, void *data)
{
	struct abbrev_cb_data *d = data;
	d->ret = d->cb(oid, NULL, d->cb_data);
	return d->ret ? CB_BREAK : CB_CONTINUE;
}

static int odb_source_files_for_each_unique_abbrev(struct odb_source *source,
						   const struct object_id *oid_prefix,
						   unsigned int prefix_len,
						   odb_for_each_object_cb cb,
						   void *cb_data)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct multi_pack_index *m;
	struct packfile_list_entry *entry;
	unsigned int hexsz = source->odb->repo->hash_algo->hexsz;
	unsigned int len = prefix_len > hexsz ? hexsz : prefix_len;

	/* Search loose objects */
	{
		struct oidtree *tree = odb_source_loose_cache(source, oid_prefix);
		if (tree) {
			struct abbrev_cb_data d = { cb, cb_data, 0 };
			oidtree_each(tree, oid_prefix, prefix_len, abbrev_loose_cb, &d);
			if (d.ret)
				return d.ret;
		}
	}

	/* Search multi-pack indices */
	m = get_multi_pack_index(source);
	for (; m; m = m->base_midx) {
		uint32_t num, i, first = 0;

		if (!m->num_objects)
			continue;

		num = m->num_objects + m->num_objects_in_base;
		bsearch_one_midx(oid_prefix, m, &first);

		for (i = first; i < num; i++) {
			struct object_id oid;
			const struct object_id *current;
			int ret;

			current = nth_midxed_object_oid(&oid, m, i);
			if (!match_hash_prefix(len, oid_prefix->hash, current->hash))
				break;
			ret = cb(current, NULL, cb_data);
			if (ret)
				return ret;
		}
	}

	/* Search packs not covered by MIDX */
	for (entry = packfile_store_get_packs(files->packed); entry; entry = entry->next) {
		struct packed_git *p = entry->pack;
		uint32_t num, i, first = 0;

		if (p->multi_pack_index)
			continue;
		if (open_pack_index(p) || !p->num_objects)
			continue;

		num = p->num_objects;
		bsearch_pack(oid_prefix, p, &first);

		for (i = first; i < num; i++) {
			struct object_id oid;
			int ret;

			nth_packed_object_id(&oid, p, i);
			if (!match_hash_prefix(len, oid_prefix->hash, oid.hash))
				break;
			ret = cb(&oid, NULL, cb_data);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int odb_source_files_convert_object_id(struct odb_source *source,
					      const struct object_id *src,
					      const struct git_hash_algo *to,
					      struct object_id *dest)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct loose_object_map *map;
	kh_oid_map_t *hash_map;
	khiter_t pos;

	if (!files->loose || !files->loose->map)
		return -1;

	map = files->loose->map;

	if (to == source->odb->repo->compat_hash_algo)
		hash_map = map->to_compat;
	else if (to == source->odb->repo->hash_algo)
		hash_map = map->to_storage;
	else
		return -1;

	pos = kh_get_oid_map(hash_map, *src);
	if (pos == kh_end(hash_map))
		return -1;

	oidcpy(dest, kh_value(hash_map, pos));
	return 0;
}

struct odb_source_files *odb_source_files_new(struct object_database *odb,
					      const char *path,
					      bool local)
{
	struct odb_source_files *files;

	CALLOC_ARRAY(files, 1);
	odb_source_init(&files->base, odb, ODB_SOURCE_FILES, path, local);
	files->loose = odb_source_loose_new(&files->base);
	files->packed = packfile_store_new(&files->base);
	files->base.packed = files->packed;
	files->base.loose = files->loose;

	files->base.free = odb_source_files_free;
	files->base.close = odb_source_files_close;
	files->base.reprepare = odb_source_files_reprepare;
	files->base.read_object_info = odb_source_files_read_object_info;
	files->base.read_object_stream = odb_source_files_read_object_stream;
	files->base.for_each_object = odb_source_files_for_each_object;
	files->base.count_objects = odb_source_files_count_objects;
	files->base.freshen_object = odb_source_files_freshen_object;
	files->base.write_object = odb_source_files_write_object;
	files->base.write_object_stream = odb_source_files_write_object_stream;
	files->base.begin_transaction = odb_source_files_begin_transaction;
	files->base.read_alternates = odb_source_files_read_alternates;
	files->base.write_alternate = odb_source_files_write_alternate;
	files->base.write_packfile = odb_source_files_write_packfile;
	files->base.for_each_unique_abbrev = odb_source_files_for_each_unique_abbrev;
	files->base.convert_object_id = odb_source_files_convert_object_id;

	/*
	 * Ideally, we would only ever store absolute paths in the source. This
	 * is not (yet) possible though because we access and assume relative
	 * paths in the primary ODB source in some user-facing functionality.
	 */
	if (!is_absolute_path(path))
		chdir_notify_register(NULL, odb_source_files_reparent, files);

	return files;
}
