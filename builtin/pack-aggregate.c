#define USE_THE_REPOSITORY_VARIABLE

#include "builtin.h"
#include "config.h"
#include "gettext.h"
#include "hash.h"
#include "hex.h"
#include "midx.h"
#include "object-file.h"
#include "odb.h"
#include "oid-array.h"
#include "packfile.h"
#include "parse-options.h"
#include "path.h"
#include "repository.h"
#include "run-command.h"
#include "strbuf.h"
#include "string-list.h"
#include "strmap.h"
#include "strvec.h"
#include "tempfile.h"
#include "wrapper.h"

static const char *const pack_aggregate_usage[] = {
	N_("git pack-aggregate --once [--min-loose=<n>] [--min-packs=<n>]\n"
	   "                         [--max-loose-objects=<n>]\n"
	   "                         [--max-objects=<n>]\n"
	   "                         [--max-input-pack-size=<bytes>]"),
	NULL
};

#define DEFAULT_MAX_LOOSE_OBJECTS 100000

#define DEFAULT_MAX_OBJECTS 100000

static int has_sidecar(const char *packdir, const char *basename,
		       const char *ext)
{
	struct strbuf buf = STRBUF_INIT;
	struct stat st;
	int ret;

	strbuf_addf(&buf, "%s/%s.%s", packdir, basename, ext);
	ret = !lstat(buf.buf, &st);
	strbuf_release(&buf);
	return ret;
}

static int has_protective_sidecar(const char *packdir, const char *basename)
{
	/*
	 * Ignore packs with sidecars that mean "don't touch me". .baddeltas
	 * is intentionally absent: rolling those up is the point.
	 */
	static const char *exts[] = {
		"keep", "promisor", "mtimes", "bitmap", NULL
	};
	int i;

	for (i = 0; exts[i]; i++)
		if (has_sidecar(packdir, basename, exts[i]))
			return 1;
	return 0;
}

static int idx_file_size(const char *packdir, const char *basename,
			 off_t *size)
{
	struct strbuf buf = STRBUF_INIT;
	struct stat st;
	int ret;

	strbuf_addf(&buf, "%s/%s.idx", packdir, basename);
	ret = !lstat(buf.buf, &st);
	if (ret)
		*size = st.st_size;
	strbuf_release(&buf);
	return ret;
}

/*
 * Convert the object-count cap to a v2 index size for comparison with
 * idx_file_size().  For N objects:
 *
 *   size = 8 (header) + 1024 (fanout) + N*(rawsz + 8) + 2*rawsz (trailer)
 *
 * Ignore the extra 8 bytes per offset >= 2GiB.  This overestimates N
 * for packs with large offsets, favoring skipping borderline packs.
 * A cap of 0 disables the limit and returns 0.
 */
static off_t max_objects_to_idx_size(const struct git_hash_algo *algo,
				     int max_objects)
{
	off_t rawsz = algo->rawsz;

	if (!max_objects)
		return 0;
	return 8 + 1024 + 2 * rawsz + (rawsz + 8) * (off_t)max_objects;
}

/*
 * Re-read the multi-pack-index (and any incremental layers) and
 * populate `set` with the basenames of every referenced pack.  The
 * strset is cleared first so this is safe to call once per cycle.
 */
static void refresh_midx_exclusions(struct repository *repo,
				    struct strset *set)
{
	struct odb_source *source;

	strset_clear(set);

	odb_reprepare(repo->objects);
	for (source = repo->objects->sources; source; source = source->next) {
		struct odb_source_files *files = odb_source_files_downcast(source);
		struct multi_pack_index *m = get_multi_pack_index(files->packed);
		for (; m; m = m->base_midx) {
			uint32_t i;
			for (i = 0; i < m->num_packs; i++) {
				struct strbuf base = STRBUF_INIT;
				strbuf_addstr(&base, m->pack_names[i]);
				strbuf_strip_suffix(&base, ".idx");
				strbuf_strip_suffix(&base, ".pack");
				strset_add(set, base.buf);
				strbuf_release(&base);
			}
		}
	}
}

/* ---------- loose-object pre-pass ---------- */

struct loose_scan {
	struct oid_array *oids;
	struct string_list *paths;
	size_t limit;
	size_t minimum;
	size_t eligible;
	time_t cutoff_sec;
	unsigned int cutoff_nsec;
};

static void set_loose_scan_cutoff(struct repository *repo,
				  struct loose_scan *data)
{
	struct strbuf template = STRBUF_INIT;
	struct tempfile *marker;
	struct stat st;

	strbuf_addf(&template, "%s/.tmp-pack-aggregate-cutoff-XXXXXX",
		    repo_get_object_directory(repo));
	marker = xmks_tempfile(template.buf);
	strbuf_release(&template);

	if (fstat(get_tempfile_fd(marker), &st))
		die_errno(_("could not stat loose-object cutoff marker"));
	data->cutoff_sec = st.st_mtime;
	data->cutoff_nsec = ST_MTIME_NSEC(st);
	delete_tempfile(&marker);
}

static int loose_scan_cb(const struct object_id *oid, const char *path,
			 void *cb_data)
{
	struct loose_scan *data = cb_data;
	struct stat st;

	if (lstat(path, &st)) {
		if (errno != ENOENT)
			warning_errno(_("could not stat loose object '%s'"),
				      path);
		return 0;
	}
	if (st.st_mtime > data->cutoff_sec ||
	    (st.st_mtime == data->cutoff_sec &&
	     ST_MTIME_NSEC(st) >= data->cutoff_nsec))
		return 0;

	data->eligible++;
	if (!data->limit || data->oids->nr < data->limit) {
		oid_array_append(data->oids, oid);
		string_list_append(data->paths, path);
	}

	if (data->limit && data->oids->nr >= data->limit &&
	    data->eligible >= data->minimum)
		return 1;
	return 0;
}

static int run_pack_objects(const char *packtmp, int stdin_packs,
			    const struct strbuf *input,
			    struct string_list *out_hashes)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct strbuf output = STRBUF_INIT;
	int ret;

	strvec_push(&cmd.args, "pack-objects");
	if (stdin_packs)
		strvec_pushl(&cmd.args, "--stdin-packs",
			     "--prefer-reused-deltas", NULL);
	strvec_pushl(&cmd.args,
		     "--window=0",
		     "--mark-bad-deltas",
		     "--delta-base-offset",
		     "--no-write-bitmap-index",
		     "--quiet",
		     packtmp,
		     NULL);
	cmd.git_cmd = 1;
	cmd.clean_on_exit = 1;

	/*
	 * pipe_command() pumps stdin and stdout concurrently.  Its
	 * clean-on-exit handling also terminates the child if we exit
	 * before it does.
	 */
	ret = pipe_command(&cmd, input->buf, input->len, &output, 0, NULL, 0);

	if (!ret && output.len) {
		strbuf_strip_suffix(&output, "\n");
		string_list_split(out_hashes, output.buf, "\n", -1);
	}

	strbuf_release(&output);
	return ret;
}

static int run_pack_objects_loose(const char *packtmp, struct oid_array *oids,
				  struct string_list *out_hashes)
{
	struct strbuf input = STRBUF_INIT;
	size_t i;
	int ret;

	for (i = 0; i < oids->nr; i++)
		strbuf_addf(&input, "%s\n", oid_to_hex(&oids->oid[i]));
	ret = run_pack_objects(packtmp, 0, &input, out_hashes);
	strbuf_release(&input);
	return ret;
}

static int unlink_loose_paths(const struct string_list *paths)
{
	size_t i;

	for (i = 0; i < paths->nr; i++)
		if (unlink_or_warn(paths->items[i].string))
			return -1;
	return 0;
}

/* ---------- pack aggregation ---------- */

static void collect_pack_candidates(struct repository *repo,
				    const char *packdir,
				    struct strset *cycle_exclude,
				    struct strset *midx_exclude,
				    struct string_list *candidates,
				    off_t max_idx_size,
				    unsigned long max_input_pack_size)
{
	struct packed_git *p;
	struct strbuf base = STRBUF_INIT;
	off_t idx_size = 0;

	repo_for_each_pack(repo, p) {
		if (!p->pack_local)
			continue;
		if (max_input_pack_size &&
		    (uintmax_t)p->pack_size > max_input_pack_size)
			continue;

		strbuf_reset(&base);
		strbuf_addstr(&base, pack_basename(p));
		if (!strbuf_strip_suffix(&base, ".pack"))
			continue;

		if (strset_contains(cycle_exclude, base.buf))
			continue;
		if (strset_contains(midx_exclude, base.buf))
			continue;
		if (has_protective_sidecar(packdir, base.buf))
			continue;
		if (!idx_file_size(packdir, base.buf, &idx_size))
			continue;
		if (max_idx_size && idx_size > max_idx_size)
			continue;

		string_list_append(candidates, base.buf);
	}

	strbuf_release(&base);
}

static int run_pack_objects_packs(const char *packtmp,
				  const struct string_list *bases,
				  struct string_list *out_hashes)
{
	struct strbuf input = STRBUF_INIT;
	size_t i;
	int ret;

	for (i = 0; i < bases->nr; i++)
		strbuf_addf(&input, "%s.pack\n", bases->items[i].string);
	ret = run_pack_objects(packtmp, 1, &input, out_hashes);
	strbuf_release(&input);
	return ret;
}

/*
 * pack-objects writes its output as <packtmp>-<hash>.<ext>; rename it
 * into place as <packdir>/pack-<hash>.<ext>.  .idx is renamed last so
 * a concurrent reader scanning the pack directory never sees a .idx
 * without its companion .pack.
 */
static int install_pack(struct repository *repo, const char *packtmp,
			const char *packdir, const char *hash)
{
	static const char *exts[] = {
		".pack", ".rev", ".baddeltas", ".idx"
	};
	struct tempfile *files[ARRAY_SIZE(exts)] = { 0 };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(exts); i++) {
		struct strbuf src = STRBUF_INIT;
		struct stat st;

		strbuf_addf(&src, "%s-%s%s", packtmp, hash, exts[i]);
		if (!stat(src.buf, &st)) {
			files[i] = register_tempfile(src.buf);
			if (adjust_shared_perm(repo, src.buf)) {
				error_errno(_("unable to adjust permissions for '%s'"),
					    src.buf);
				strbuf_release(&src);
				goto cleanup;
			}
		} else if (errno != ENOENT) {
			error_errno(_("could not stat '%s'"), src.buf);
			strbuf_release(&src);
			goto cleanup;
		}
		strbuf_release(&src);
	}

	for (i = 0; i < ARRAY_SIZE(exts); i++) {
		struct strbuf dst = STRBUF_INIT;

		if (!files[i])
			continue;

		strbuf_addf(&dst, "%s/pack-%s%s", packdir, hash, exts[i]);
		if (rename_tempfile(&files[i], dst.buf)) {
			error_errno(_("renaming pack to '%s' failed"), dst.buf);
			strbuf_release(&dst);
			goto cleanup;
		}
		strbuf_release(&dst);
	}
	return 0;

cleanup:
	for (i = 0; i < ARRAY_SIZE(files); i++)
		if (files[i])
			delete_tempfile(&files[i]);
	return -1;
}

static int install_packs(struct repository *repo, const char *packtmp,
			 const char *packdir,
			 const struct string_list *hashes,
			 struct strset *output_bases)
{
	struct string_list_item *item;
	struct strbuf base = STRBUF_INIT;
	int ret = 0;

	for_each_string_list_item(item, hashes) {
		struct object_id oid;

		if (strlen(item->string) != repo->hash_algo->hexsz ||
		    get_oid_hex_algop(item->string, &oid, repo->hash_algo))
			return error(_("pack-objects returned an invalid pack hash"));
	}

	for_each_string_list_item(item, hashes) {
		if (install_pack(repo, packtmp, packdir, item->string)) {
			ret = -1;
			break;
		}
		strbuf_reset(&base);
		strbuf_addf(&base, "pack-%s", item->string);
		strset_add(output_bases, base.buf);
	}

	strbuf_release(&base);
	return ret;
}

static void unlink_consumed_packs(const char *packdir,
				  const struct string_list *bases,
				  struct strset *keep_basenames)
{
	static const char *exts[] = {
		"pack", "idx", "rev", "baddeltas", NULL
	};
	size_t i;

	for (i = 0; i < bases->nr; i++) {
		const char *base = bases->items[i].string;
		int j;

		/*
		 * Recheck protective sidecars: a .keep (or similar) may
		 * have appeared between scan and now, in which case the
		 * pack must stay.
		 */
		if (has_protective_sidecar(packdir, base))
			continue;
		/*
		 * An output can be byte-identical to an input, so deleting
		 * that input would delete the output too.
		 */
		if (strset_contains(keep_basenames, base))
			continue;

		for (j = 0; exts[j]; j++) {
			struct strbuf fname = STRBUF_INIT;
			strbuf_addf(&fname, "%s/%s.%s", packdir, base,
				    exts[j]);
			if (unlink(fname.buf) < 0 && errno != ENOENT)
				warning_errno(_("could not unlink '%s'"),
					      fname.buf);
			strbuf_release(&fname);
		}
	}
}

static int run_aggregation(struct repository *repo, const char *packdir,
			   struct strset *midx_exclude,
			   int min_loose, int min_packs,
			   int max_loose_objects, int max_objects,
			   unsigned long max_input_pack_size)
{
	struct oid_array loose_oids = OID_ARRAY_INIT;
	struct string_list loose_paths = STRING_LIST_INIT_DUP;
	struct loose_scan loose_data = {
		.oids = &loose_oids,
		.paths = &loose_paths,
		.limit = max_loose_objects,
		.minimum = min_loose,
	};
	struct strset loose_rollup_exclude = STRSET_INIT;
	struct strset output_bases = STRSET_INIT;
	struct string_list candidates = STRING_LIST_INIT_DUP;
	struct string_list output_hashes = STRING_LIST_INIT_DUP;
	char *packtmp_loose = NULL;
	char *packtmp_packs = NULL;
	int ret = 0;

	/*
	 * Step 1: pack loose objects in bounded batches, installing every
	 * output before removing inputs. The cycle-start cutoff prevents
	 * new arrivals from prolonging this cycle.
	 */
	set_loose_scan_cutoff(repo, &loose_data);
	for_each_loose_file_in_source(repo->objects->sources,
				      loose_scan_cb, NULL, NULL, &loose_data);
	if (loose_data.eligible >= (size_t)min_loose) {
		packtmp_loose = mkpathdup("%s/.tmp-%d-loose-pack",
					  packdir, (int)getpid());
		while (loose_oids.nr) {
			string_list_clear(&output_hashes, 0);
			if (run_pack_objects_loose(packtmp_loose, &loose_oids,
						   &output_hashes)) {
				ret = error(_("pack-objects failed during "
					      "loose-object rollup"));
				goto out;
			}
			if (!output_hashes.nr)
				break;

			if (install_packs(repo, packtmp_loose, packdir,
					  &output_hashes, &loose_rollup_exclude) ||
			    unlink_loose_paths(&loose_paths)) {
				ret = -1;
				goto out;
			}

			oid_array_clear(&loose_oids);
			string_list_clear(&loose_paths, 0);

			/* Once rollup starts, also consume a final partial batch. */
			loose_data.minimum = 1;
			loose_data.eligible = 0;
			for_each_loose_file_in_source(repo->objects->sources,
						      loose_scan_cb, NULL, NULL,
						      &loose_data);
		}
	}

	if (strset_get_size(&loose_rollup_exclude) == 1)
		strset_clear(&loose_rollup_exclude);

	/*
	 * Step 2: aggregate small packs. Let a single pack produced from loose
	 * objects participate, but defer multiple packs to avoid copying
	 * them twice.
	 * Refresh MIDX exclusions before collecting candidates.
	 */
	refresh_midx_exclusions(repo, midx_exclude);
	collect_pack_candidates(repo, packdir,
				&loose_rollup_exclude, midx_exclude, &candidates,
				max_objects_to_idx_size(repo->hash_algo,
							max_objects),
				max_input_pack_size);

	if ((int)candidates.nr < min_packs)
		goto out;

	packtmp_packs = mkpathdup("%s/.tmp-%d-pack",
				  packdir, (int)getpid());
	string_list_clear(&output_hashes, 0);
	if (run_pack_objects_packs(packtmp_packs, &candidates,
				   &output_hashes)) {
		ret = error(_("pack-objects failed during "
			      "pack aggregation"));
		goto out;
	}
	if (output_hashes.nr) {
		if (install_packs(repo, packtmp_packs, packdir,
				  &output_hashes, &output_bases)) {
			ret = -1;
			goto out;
		}
		unlink_consumed_packs(packdir, &candidates, &output_bases);
	}

out:
	free(packtmp_loose);
	free(packtmp_packs);
	string_list_clear(&output_hashes, 0);
	strset_clear(&loose_rollup_exclude);
	strset_clear(&output_bases);
	string_list_clear(&candidates, 0);
	oid_array_clear(&loose_oids);
	string_list_clear(&loose_paths, 0);
	return ret;
}

int cmd_pack_aggregate(int argc, const char **argv,
		       const char *prefix, struct repository *repo)
{
	int min_packs = 5;
	int min_loose = 5;
	int max_loose_objects = -1;
	int max_objects = -1;
	unsigned long max_input_pack_size = 0;
	unsigned long pack_size_limit = 0;
	int once = 0;
	struct option options[] = {
		OPT_BOOL(0, "once", &once,
			 N_("run a single cycle and exit")),
		OPT_INTEGER(0, "min-loose", &min_loose,
			    N_("skip loose-object rollup if fewer "
			       "candidates (default 5)")),
		OPT_INTEGER(0, "min-packs", &min_packs,
			    N_("skip pack aggregation if fewer "
			       "candidates (default 5)")),
		OPT_INTEGER(0, "max-loose-objects", &max_loose_objects,
			    N_("pack at most this many loose objects per "
			       "batch (0 for no limit)")),
		OPT_INTEGER(0, "max-objects", &max_objects,
			    N_("skip packs with more than this many "
			       "objects (0 for no limit)")),
		OPT_UNSIGNED(0, "max-input-pack-size", &max_input_pack_size,
			     N_("skip packs larger than this many bytes "
				"(0 for automatic)")),
		OPT_END(),
	};
	struct strset midx_exclude = STRSET_INIT;
	char *packdir;
	int ret = 0;

	if (repo)
		repo_config_get_ulong(repo, "pack.aggregatemaxinputpacksize",
				      &max_input_pack_size);

	argc = parse_options(argc, argv, prefix, options,
			     pack_aggregate_usage, 0);
	if (argc > 0)
		usage_with_options(pack_aggregate_usage, options);
	if (!once)
		die(_("--once is required"));
	if (min_loose < 1)
		die(_("--min-loose must be at least 1"));
	if (min_packs < 1)
		die(_("--min-packs must be at least 1"));

	if (max_loose_objects < 0 &&
	    repo_config_get_int(repo, "pack.aggregatemaxlooseobjects",
				&max_loose_objects))
		max_loose_objects = DEFAULT_MAX_LOOSE_OBJECTS;
	if (max_loose_objects < 0)
		die(_("pack.aggregateMaxLooseObjects cannot be negative"));

	if (max_objects < 0 &&
	    repo_config_get_int(repo, "pack.aggregatemaxobjects", &max_objects))
		max_objects = DEFAULT_MAX_OBJECTS;
	if (max_objects < 0)
		die(_("pack.aggregateMaxObjects cannot be negative"));

	repo_config_get_ulong(repo, "pack.packsizelimit", &pack_size_limit);
	/* Match pack-objects' minimum nonzero output size limit. */
	if (pack_size_limit && pack_size_limit < 1024 * 1024)
		pack_size_limit = 1024 * 1024;
	if (pack_size_limit) {
		unsigned long ceiling = pack_size_limit / 2;

		if (max_input_pack_size > ceiling)
			die(_("maximum input pack size cannot exceed %lu bytes "
			      "(half the effective pack.packSizeLimit)"), ceiling);
		if (!max_input_pack_size)
			max_input_pack_size = ceiling;
	}

	packdir = mkpathdup("%s/pack", repo_get_object_directory(repo));

	ret = run_aggregation(repo, packdir, &midx_exclude,
			      min_loose, min_packs,
			      max_loose_objects, max_objects,
			      max_input_pack_size);

	strset_clear(&midx_exclude);
	free(packdir);
	return ret ? 1 : 0;
}
