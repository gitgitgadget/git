#include "git-compat-util.h"
#include "repack.h"
#include "repository.h"
#include "run-command.h"
#include "string-list.h"
#include "hex.h"
#include "packfile.h"
#include "list-objects-filter-options.h"
#include "list-objects-filter.h"
#include "odb.h"
#include "promisor-remote.h"
#include "copy.h"
#include "object-file.h"
#include "odb/source-files.h"
#include "oidset.h"
#include "path.h"
#include "sigchain.h"

static void start_unpack_objects(struct child_process *unpack,
				 const char *objdir)
{
	strvec_push(&unpack->args, "unpack-objects");
	strvec_push(&unpack->args, "-q");
	strvec_pushf(&unpack->env, "GIT_OBJECT_DIRECTORY=%s", objdir);
	strvec_push(&unpack->env, "GIT_ALTERNATE_OBJECT_DIRECTORIES");
	unpack->git_cmd = 1;
	unpack->in = -1;
	if (start_command(unpack))
		die(_("could not start unpack-objects for '%s'"), objdir);
}

int write_filtered_pack(const struct write_pack_opts *opts,
			struct existing_packs *existing,
			struct string_list *names)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct child_process unpack = CHILD_PROCESS_INIT;
	struct string_list_item *item;
	FILE *in;
	int ret;
	const char *caret;
	const char *pack_prefix = write_pack_opts_pack_prefix(opts);

	if (opts->loose) {
		sigchain_push(SIGPIPE, SIG_IGN);
		start_unpack_objects(&unpack, opts->destination);
		prepare_pack_objects(&cmd, opts->po_args, "--stdout");
	} else {
		prepare_pack_objects(&cmd, opts->po_args, opts->destination);
	}

	strvec_push(&cmd.args, "--stdin-packs");

	for_each_string_list_item(item, &existing->kept_packs)
		strvec_pushf(&cmd.args, "--keep-pack=%s", item->string);

	cmd.in = -1;
	if (opts->loose)
		cmd.out = unpack.in;

	ret = start_command(&cmd);
	if (ret) {
		if (opts->loose) {
			close(unpack.in);
			finish_command(&unpack);
			sigchain_pop(SIGPIPE);
		}
		return ret;
	}

	/*
	 * Here 'names' contains only the pack(s) that were just
	 * written, which is exactly the packs we want to keep. Also
	 * 'existing_kept_packs' already contains the packs in
	 * 'keep_pack_list'.
	 */
	in = xfdopen(cmd.in, "w");
	for_each_string_list_item(item, names)
		fprintf(in, "^%s-%s.pack\n", pack_prefix, item->string);
	for_each_string_list_item(item, &existing->non_kept_packs)
		fprintf(in, "%s.pack\n", item->string);
	for_each_string_list_item(item, &existing->cruft_packs)
		fprintf(in, "%s.pack\n", item->string);
	caret = opts->po_args->pack_kept_objects ? "" : "^";
	for_each_string_list_item(item, &existing->kept_packs)
		fprintf(in, "%s%s.pack\n", caret, item->string);
	fclose(in);

	if (opts->loose) {
		ret = finish_command(&cmd);
		if (finish_command(&unpack))
			ret = -1;
		sigchain_pop(SIGPIPE);
		if (ret)
			ret = error(_("failed to write filtered out objects to '%s'"),
				    opts->destination);
		return ret;
	}

	return finish_pack_objects_cmd(existing->repo->hash_algo, opts, &cmd,
				       names);
}

struct collect_cb_data {
	struct repository *repo;
	struct oidset *set;
};

static int collect_promisor_blob(const struct object_id *oid,
				 struct object_info *oi UNUSED,
				 void *cb_data)
{
	struct collect_cb_data *data = cb_data;
	struct object_info info = OBJECT_INFO_INIT;
	enum object_type type;

	info.typep = &type;

	/*
	 * Use OBJECT_INFO_SKIP_FETCH_OBJECT to avoid triggering a
	 * lazy fetch while collecting promisor blobs.
	 */
	if (odb_read_object_info_extended(data->repo->objects, oid, &info,
			OBJECT_INFO_SKIP_FETCH_OBJECT) < 0)
		return 0;

	if (type == OBJ_BLOB)
		oidset_insert(data->set, oid);

	return 0;
}

int enumerate_promisor_blobs(struct repository *repo,
			     const struct list_objects_filter_options *filter,
			     struct oidset *to_drop)
{
	struct oidset all_promisor_blobs = OIDSET_INIT;
	struct collect_cb_data cb = {
		.repo = repo,
		.set = &all_promisor_blobs
	};
	int ret = 0;

	/*
	 * The caller (cmd_repack) is responsible for validating that a
	 * blob:limit filter and a promisor remote are present before
	 * calling this function.
	 *
	 * Walk only promisor objects. Every object visited here is a
	 * promisor object, so it is recoverable from the promisor remote
	 * as long as the remote still has it, the same assumption the rest
	 * of partial clone relies on.
	 *
	 * We do not use write_filtered_pack() here because git repack
	 * routes promisor objects through repack_promisor_objects()
	 * before the filter machinery runs, so the filtered pack never
	 * contains promisor blobs. Direct enumeration via
	 * ODB_FOR_EACH_OBJECT_PROMISOR_ONLY is the correct approach.
	 */
	ret = odb_for_each_object(repo->objects, NULL,
			collect_promisor_blob, &cb,
			ODB_FOR_EACH_OBJECT_PROMISOR_ONLY);
	if (ret)
		goto cleanup;

	/*
	 * Apply the filter to find which blobs exceed the threshold.
	 * The caller has to_drop and is responsible for clearing it.
	 */
	ret = list_objects_filter__filter_oidset(repo,
		filter,
		&all_promisor_blobs,
		to_drop);

cleanup:
	oidset_clear(&all_promisor_blobs);
	return ret;
}

static int collect_loose_oid(const struct object_id *oid,
			     const char *path UNUSED, void *cb_data)
{
	oidset_insert(cb_data, oid);
	return 0;
}

static void dest_loose_path(struct strbuf *buf, const char *objdir,
			    const struct object_id *oid)
{
	const char *hex = oid_to_hex(oid);

	strbuf_reset(buf);
	strbuf_addf(buf, "%s/%.2s/%s", objdir, hex, hex + 2);
}

int move_loose_filtered_objects(struct repository *repo,
				const struct list_objects_filter_options *filter,
				const char *dest_objdir,
				struct oidset *moved)
{
	struct odb_source_files *files =
		odb_source_files_downcast(repo->objects->source);
	struct oidset loose = OIDSET_INIT;
	struct oidset omitted = OIDSET_INIT;
	struct oidset_iter iter;
	const struct object_id *oid;
	struct strbuf src = STRBUF_INIT;
	struct strbuf dst = STRBUF_INIT;
	struct strbuf tmp = STRBUF_INIT;
	int ret = 0;

	if (filter->choice != LOFC_BLOB_LIMIT)
		return 0;

	ret = for_each_loose_file_in_source(&files->dirs->loose->base,
					    collect_loose_oid, NULL, NULL,
					    &loose);
	if (ret)
		goto out;
	ret = list_objects_filter__filter_oidset(repo, filter, &loose,
						 &omitted);
	if (ret)
		goto out;

	oidset_iter_init(&omitted, &iter);
	while ((oid = oidset_iter_next(&iter))) {
		odb_loose_path(files->dirs->loose, &src, oid);
		dest_loose_path(&dst, dest_objdir, oid);

		/*
		 * Only the destination decides whether the copy is needed;
		 * the object being present in the source says nothing.
		 */
		if (!access(dst.buf, F_OK)) {
			oidset_insert(moved, oid);
			continue;
		}

		if (safe_create_leading_directories(repo, dst.buf) < 0) {
			ret = error_errno(_("unable to create leading directories of '%s'"),
					  dst.buf);
			goto out;
		}
		strbuf_reset(&tmp);
		strbuf_addf(&tmp, "%s.tmp-%"PRIuMAX, dst.buf,
			    (uintmax_t)getpid());
		if (copy_file(repo, tmp.buf, src.buf, 0444) ||
		    finalize_object_file(repo, tmp.buf, dst.buf)) {
			unlink(tmp.buf);
			ret = error(_("unable to copy loose object '%s' to '%s'"),
				    src.buf, dst.buf);
			goto out;
		}
		oidset_insert(moved, oid);
	}

out:
	oidset_clear(&loose);
	oidset_clear(&omitted);
	strbuf_release(&src);
	strbuf_release(&dst);
	strbuf_release(&tmp);
	return ret;
}

void remove_moved_loose_objects(struct repository *repo,
				const struct oidset *moved)
{
	struct odb_source_files *files =
		odb_source_files_downcast(repo->objects->source);
	struct strbuf path = STRBUF_INIT;
	struct oidset_iter iter;
	const struct object_id *oid;

	oidset_iter_init(moved, &iter);
	while ((oid = oidset_iter_next(&iter))) {
		odb_loose_path(files->dirs->loose, &path, oid);
		unlink(path.buf);
	}
	strbuf_release(&path);
}
