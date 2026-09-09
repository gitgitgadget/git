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
#include "sigchain.h"
#include "strbuf.h"
#include "string-list.h"
#include "strmap.h"
#include "strvec.h"
#include "tempfile.h"
#include "trace2.h"
#include "wrapper.h"

static const char *const pack_aggregate_usage[] = {
	N_("git pack-aggregate (--once | --loop) [--interval=<seconds>]\n"
	   "                  [--min-loose=<n>] [--min-packs=<n>]\n"
	   "                  [--max-loose-objects=<n>]\n"
	   "                  [--max-objects=<n>] [--max-packs=<n>]\n"
	   "                  [--exclude-pack-file=<path>]\n"
	   "                  [--exclude-loose-file=<path>]\n"
	   "                  [--parent-pipe-fd=<n>]"),
	NULL
};

#define DEFAULT_MAX_LOOSE_OBJECTS 100000

#define DEFAULT_MAX_OBJECTS 100000

#define DEFAULT_MAX_PACKS 10000

static volatile sig_atomic_t stop_signaled;
static int parent_pipe_fd = -1;

static void term_handler(int sig UNUSED)
{
	stop_signaled = 1;
}

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
 * Gauge pack heft by object count rather than byte size because the work
 * this gate bounds -- enumerating objects and rebuilding the output index --
 * scales with object count.  A pack containing a few enormous blobs can
 * still fall below the cap, but that is uncommon in the target workload and
 * aggregation copies its existing representation without delta search or
 * recompression.
 *
 * Convert that object-count cap into the maximum size (in bytes) of a v2
 * pack `.idx`.  The v2 index is linear in the number of objects N:
 *
 *   size = 8 (header) + 1024 (fanout) + N*(rawsz + 8) + 2*rawsz (trailer)
 *
 * Reusing the lstat() performed by idx_file_size() is much faster than
 * opening, mapping, and reading each candidate index.  In particular,
 * p->num_objects is populated only by open_pack_index(), which mmaps the
 * index; mapping every candidate merely to apply this gate can also exhaust
 * vm.max_map_count on repositories with an enormous number of packs.
 *
 * The format also uses an 8-byte entry per large offset (objects at pack
 * offset >= 2GiB), which we ignore here.  Ignoring it makes our estimated N
 * slightly high, i.e. we err on the side of skipping a borderline pack,
 * which is the safe direction for this "don't touch large packs" gate.  A
 * cap of 0 disables the limit and is represented by a returned size of 0.
 */
static off_t max_objects_to_idx_size(const struct git_hash_algo *algo,
				     int max_objects)
{
	off_t rawsz = algo->rawsz;

	if (!max_objects)
		return 0;
	return 8 + 1024 + 2 * rawsz + (rawsz + 8) * (off_t)max_objects;
}

static void load_exclusions_from_file(const char *path, struct strset *set)
{
	FILE *fp;
	struct strbuf line = STRBUF_INIT;

	fp = fopen(path, "r");
	if (!fp)
		die_errno(_("could not open exclude file '%s'"), path);

	while (strbuf_getline_lf(&line, fp) != EOF) {
		strbuf_trim(&line);
		if (!line.len || line.buf[0] == '#')
			continue;
		strbuf_strip_suffix(&line, ".pack");
		strbuf_strip_suffix(&line, ".idx");
		strset_add(set, line.buf);
	}
	strbuf_release(&line);
	fclose(fp);
}

/*
 * Re-read the multi-pack-index (and any incremental layers) and
 * populate `set` with the basenames of every referenced pack.  The
 * strset is cleared first so this is safe to call once per cycle.
 */
static int refresh_midx_exclusions(struct repository *repo,
				   struct strset *set)
{
	struct odb_source *source;
	int had_midx = 0;

	strset_clear(set);

	odb_reprepare(repo->objects);
	for (source = repo->objects->sources; source; source = source->next) {
		struct odb_source_files *files = odb_source_files_downcast(source);
		struct multi_pack_index *m = get_multi_pack_index(files->packed);
		for (; m; m = m->base_midx) {
			uint32_t i;
			had_midx = 1;
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
	return had_midx;
}

/* ---------- loose-object pre-pass ---------- */

struct loose_scan {
	struct strset *exclude;
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

	if (strset_contains(data->exclude, oid_to_hex(oid)))
		return 0;
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
			    const struct strbuf *input, struct strbuf *out_hash)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct strbuf output = STRBUF_INIT;
	const char *newline;
	int ret;

	strvec_push(&cmd.args, "pack-objects");
	if (stdin_packs)
		strvec_push(&cmd.args, "--stdin-packs");
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

	newline = memchr(output.buf, '\n', output.len);
	strbuf_add(out_hash, output.buf,
		   newline ? (size_t)(newline - output.buf) : output.len);

	strbuf_release(&output);
	return ret;
}

static int run_pack_objects_loose(const char *packtmp, struct oid_array *oids,
				  struct strbuf *out_hash)
{
	struct strbuf input = STRBUF_INIT;
	size_t i;
	int ret;

	for (i = 0; i < oids->nr; i++)
		strbuf_addf(&input, "%s\n", oid_to_hex(&oids->oid[i]));
	ret = run_pack_objects(packtmp, 0, &input, out_hash);
	strbuf_release(&input);
	return ret;
}

static void unlink_loose_paths(const struct string_list *paths)
{
	size_t i;

	for (i = 0; i < paths->nr; i++) {
		const char *p = paths->items[i].string;
		if (unlink(p) < 0 && errno != ENOENT)
			warning_errno(_("could not unlink loose object '%s'"),
				      p);
	}
}

/* ---------- pack aggregation ---------- */

/*
 * True only for a durably installed "pack-<hash>" basename.  This
 * rejects an in-flight ".tmp-<pid>-pack-<hash>" staging file written by
 * a concurrent repack or pack-aggregate, which the object-store scan
 * (matching any "*.idx") would otherwise surface as a candidate.
 */
static int is_canonical_pack_base(const char *base)
{
	const char *hex;
	size_t i, hexsz = the_hash_algo->hexsz;

	if (!skip_prefix(base, "pack-", &hex))
		return 0;
	for (i = 0; i < hexsz; i++)
		if (!isxdigit(hex[i]))
			return 0;
	return hex[hexsz] == '\0';
}

static void collect_pack_candidates(struct repository *repo,
				    const char *packdir,
				    struct strset *file_exclude,
				    struct strset *cycle_exclude,
				    struct strset *midx_exclude,
				    struct string_list *candidates,
				    off_t max_idx_size)
{
	struct packed_git *p;
	struct strbuf base = STRBUF_INIT;
	off_t idx_size = 0;

	repo_for_each_pack(repo, p) {
		if (!p->pack_local)
			continue;

		strbuf_reset(&base);
		strbuf_addstr(&base, pack_basename(p));
		if (!strbuf_strip_suffix(&base, ".pack"))
			continue;

		if (!is_canonical_pack_base(base.buf))
			continue;

		if (strset_contains(file_exclude, base.buf))
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
				  size_t begin, size_t count,
				  struct strbuf *out_hash)
{
	struct strbuf input = STRBUF_INIT;
	size_t i;
	int ret;

	for (i = begin; i < begin + count; i++)
		strbuf_addf(&input, "%s.pack\n", bases->items[i].string);
	ret = run_pack_objects(packtmp, 1, &input, out_hash);
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

static void unlink_consumed_packs(const char *packdir,
				  const struct string_list *bases,
				  size_t begin, size_t count,
				  const char *keep_basename)
{
	static const char *exts[] = {
		"pack", "idx", "rev", "baddeltas", NULL
	};
	size_t i;

	for (i = begin; i < begin + count; i++) {
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
		 * Never unlink the pack we just installed.  Aggregating
		 * packs whose union equals one of the inputs (e.g. one
		 * input is a strict superset of the others) can yield a
		 * byte-identical output, which lands at the same final
		 * name as that input.  In that case, we do not want the
		 * file serving as both input and output to be deleted.
		 */
		if (keep_basename && !strcmp(base, keep_basename))
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

static int do_one_cycle(struct repository *repo, const char *packdir,
			struct strset *pack_exclude,
			struct strset *loose_exclude,
			struct strset *midx_exclude,
			int min_loose, int min_packs,
			int max_loose_objects, int max_objects,
			int max_packs)
{
	struct oid_array loose_oids = OID_ARRAY_INIT;
	struct string_list loose_paths = STRING_LIST_INIT_DUP;
	struct loose_scan loose_data = {
		.exclude = loose_exclude,
		.oids = &loose_oids,
		.paths = &loose_paths,
		.limit = max_loose_objects,
		.minimum = min_loose,
	};
	struct strset loose_rollup_exclude = STRSET_INIT;
	struct string_list candidates = STRING_LIST_INIT_DUP;
	struct strbuf first_loose_rollup = STRBUF_INIT;
	struct strbuf loose_hash = STRBUF_INIT;
	char *packtmp_loose = NULL;
	char *packtmp_packs = NULL;
	int ret = 0;

	/*
	 * Step 1: bundle local loose objects (minus excluded ones) into
	 * bounded packs and remove the on-disk loose copies after each
	 * pack is installed.  Ignore objects newer than the cycle-start
	 * timestamp so concurrent writers cannot keep this cycle running
	 * indefinitely.
	 */
	set_loose_scan_cutoff(repo, &loose_data);
	for_each_loose_file_in_source(repo->objects->sources,
				      loose_scan_cb, NULL, NULL, &loose_data);
	if (loose_data.eligible >= (size_t)min_loose && !stop_signaled) {
		size_t loose_pack_count = 0;

		packtmp_loose = mkpathdup("%s/.tmp-%d-loose-pack",
					  packdir, (int)getpid());
		while (loose_oids.nr && !stop_signaled) {
			strbuf_reset(&loose_hash);
			if (run_pack_objects_loose(packtmp_loose, &loose_oids,
						   &loose_hash)) {
				ret = error(_("pack-objects failed during "
					      "loose-object rollup"));
				goto out;
			}
			if (!loose_hash.len)
				break;

			if (loose_hash.len != repo->hash_algo->hexsz) {
				ret = error(_("pack-objects returned an invalid "
					      "pack hash"));
				goto out;
			}
			if (install_pack(repo, packtmp_loose, packdir,
					 loose_hash.buf)) {
				ret = -1;
				goto out;
			}
			unlink_loose_paths(&loose_paths);
			loose_pack_count++;
			if (loose_pack_count == 1) {
				strbuf_addf(&first_loose_rollup, "pack-%s",
					    loose_hash.buf);
			} else {
				struct strbuf base = STRBUF_INIT;

				if (loose_pack_count == 2)
					strset_add(&loose_rollup_exclude,
						   first_loose_rollup.buf);
				strbuf_addf(&base, "pack-%s", loose_hash.buf);
				strset_add(&loose_rollup_exclude, base.buf);
				strbuf_release(&base);
			}

			oid_array_clear(&loose_oids);
			string_list_clear(&loose_paths, 0);
			if (stop_signaled)
				break;

			/* Once rollup starts, also consume a final partial tranche. */
			loose_data.minimum = 1;
			loose_data.eligible = 0;
			for_each_loose_file_in_source(repo->objects->sources,
						      loose_scan_cb, NULL, NULL,
						      &loose_data);
		}
	}

	if (stop_signaled)
		goto out;

	/*
	 * Step 2: aggregate small packs into one or more output packs.  A
	 * single loose-rollup pack is picked up naturally below.  When
	 * step 1 needed multiple tranches, leave those outputs alone this
	 * cycle rather than immediately copying the entire backlog again.
	 * Re-read the MIDX just before scanning so a pack added to a MIDX
	 * between cycles is honored.
	 */
	refresh_midx_exclusions(repo, midx_exclude);
	collect_pack_candidates(repo, packdir, pack_exclude,
				&loose_rollup_exclude, midx_exclude, &candidates,
				max_objects_to_idx_size(repo->hash_algo,
							max_objects));

	if ((int)candidates.nr < min_packs)
		goto out;

	packtmp_packs = mkpathdup("%s/.tmp-%d-pack",
				  packdir, (int)getpid());

	/*
	 * Split the candidates into evenly-sized batches of at most
	 * max_packs and roll each batch up into its own output pack (see
	 * --max-packs).  Splitting on whole-pack boundaries lets each
	 * batch reuse the packs' existing deltas as-is: on-disk packs are
	 * self-contained, so a delta and its base never land in different
	 * batches.  Size batches as ceil(total / ceil(total / max_packs))
	 * to avoid a tiny trailing batch; max_packs == 0 means one batch
	 * holding everything.
	 */
	{
		size_t total = candidates.nr;
		size_t batch_size = total;
		size_t start;

		if (max_packs > 0 && total > (size_t)max_packs) {
			size_t num_batches = DIV_ROUND_UP(total,
							  (size_t)max_packs);
			batch_size = DIV_ROUND_UP(total, num_batches);
		}

		for (start = 0; start < total && !stop_signaled;
		     start += batch_size) {
			size_t count = batch_size;
			struct strbuf batch_hash = STRBUF_INIT;

			if (start + count > total)
				count = total - start;

			if (run_pack_objects_packs(packtmp_packs, &candidates,
						   start, count, &batch_hash)) {
				ret = error(_("pack-objects failed during "
					      "pack aggregation"));
				strbuf_release(&batch_hash);
				goto out;
			}
			if (batch_hash.len) {
				struct strbuf output_base = STRBUF_INIT;

				if (batch_hash.len != repo->hash_algo->hexsz) {
					ret = error(_("pack-objects returned an "
						      "invalid pack hash"));
					strbuf_release(&batch_hash);
					strbuf_release(&output_base);
					goto out;
				}
				if (install_pack(repo, packtmp_packs, packdir,
						 batch_hash.buf)) {
					ret = -1;
					strbuf_release(&batch_hash);
					strbuf_release(&output_base);
					goto out;
				}
				strbuf_addf(&output_base, "pack-%s",
					    batch_hash.buf);
				unlink_consumed_packs(packdir, &candidates,
						      start, count,
						      output_base.buf);
				strbuf_release(&output_base);
			}
			strbuf_release(&batch_hash);
		}
	}

out:
	free(packtmp_loose);
	free(packtmp_packs);
	strbuf_release(&first_loose_rollup);
	strbuf_release(&loose_hash);
	strset_clear(&loose_rollup_exclude);
	string_list_clear(&candidates, 0);
	oid_array_clear(&loose_oids);
	string_list_clear(&loose_paths, 0);
	return ret;
}

static void interruptible_sleep(unsigned int seconds)
{
	struct pollfd pfd;
	int timeout_ms;

	if (stop_signaled)
		return;

	if (parent_pipe_fd < 0) {
		unsigned int remaining = seconds;
		while (remaining > 0 && !stop_signaled)
			remaining = sleep(remaining);
		return;
	}

	/*
	 * Watch the parent pipe so we wake immediately if the process
	 * that spawned us exits.  POLLHUP is reported in revents
	 * regardless of whether it appears in events, so we leave
	 * events==0; any of POLLHUP/POLLERR/POLLNVAL/POLLIN means the
	 * other end of the pipe is gone and we should stop.
	 */
	pfd.fd = parent_pipe_fd;
	pfd.events = 0;
	timeout_ms = (seconds > INT_MAX / 1000) ? INT_MAX
					       : (int)(seconds * 1000);

	while (!stop_signaled) {
		int ret;
		pfd.revents = 0;
		ret = poll(&pfd, 1, timeout_ms);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ret == 0)
			break;
		if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL | POLLIN)) {
			stop_signaled = 1;
			break;
		}
	}
}

int cmd_pack_aggregate(int argc, const char **argv,
		       const char *prefix, struct repository *repo)
{
	const char *exclude_pack_file = NULL;
	const char *exclude_loose_file = NULL;
	int interval = 60;
	int min_packs = 5;
	int min_loose = 5;
	int max_loose_objects = -1;
	int max_objects = -1;
	int max_packs = -1;
	int once = 0;
	int loop = 0;
	uintmax_t cycle_count = 0;
	struct option options[] = {
		OPT_BOOL(0, "once", &once,
			 N_("run a single cycle and exit")),
		OPT_BOOL(0, "loop", &loop,
			 N_("loop forever, sleeping --interval seconds "
			    "between cycles")),
		OPT_INTEGER(0, "interval", &interval,
			    N_("seconds to sleep between cycles "
			       "(default 60)")),
		OPT_INTEGER(0, "min-loose", &min_loose,
			    N_("skip loose-object rollup if fewer "
			       "candidates (default 5)")),
		OPT_INTEGER(0, "min-packs", &min_packs,
			    N_("skip pack aggregation if fewer "
			       "candidates (default 5)")),
		OPT_INTEGER(0, "max-loose-objects", &max_loose_objects,
			    N_("pack at most this many loose objects into "
			       "each output pack (0 for no limit)")),
		OPT_INTEGER(0, "max-objects", &max_objects,
			    N_("skip packs with more than this many "
			       "objects (0 for no limit)")),
		OPT_INTEGER(0, "max-packs", &max_packs,
			    N_("aggregate at most this many packs into "
			       "each output pack (0 for no limit)")),
		OPT_STRING(0, "exclude-pack-file", &exclude_pack_file,
			   N_("file"),
			   N_("file listing pack basenames never to "
			      "touch")),
		OPT_STRING(0, "exclude-loose-file", &exclude_loose_file,
			   N_("file"),
			   N_("file listing loose object OIDs never to "
			      "touch")),
		OPT_INTEGER(0, "parent-pipe-fd", &parent_pipe_fd,
			    N_("inherited fd of a pipe whose write end "
			       "the parent holds; EOF triggers exit")),
		OPT_END(),
	};
	struct strset pack_exclude = STRSET_INIT;
	struct strset loose_exclude = STRSET_INIT;
	struct strset midx_exclude = STRSET_INIT;
	char *packdir;
	int ret = 0;

	argc = parse_options(argc, argv, prefix, options,
			     pack_aggregate_usage, 0);
	if (argc > 0)
		usage_with_options(pack_aggregate_usage, options);
	if (once == loop)
		die(_("exactly one of --once or --loop is required"));
	if (interval < 1)
		die(_("--interval must be at least 1"));
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

	if (max_packs < 0 &&
	    repo_config_get_int(repo, "pack.aggregatemaxpacks", &max_packs))
		max_packs = DEFAULT_MAX_PACKS;
	if (max_packs < 0)
		die(_("pack.aggregateMaxPacks cannot be negative"));

	packdir = mkpathdup("%s/pack", repo_get_object_directory(repo));

	if (exclude_pack_file)
		load_exclusions_from_file(exclude_pack_file, &pack_exclude);
	if (exclude_loose_file)
		load_exclusions_from_file(exclude_loose_file, &loose_exclude);

	sigchain_push(SIGTERM, term_handler);
	sigchain_push(SIGHUP, term_handler);
	sigchain_push(SIGINT, term_handler);

	do {
		if (stop_signaled)
			break;
		trace2_region_enter("pack-aggregate", "cycle", repo);
		ret = do_one_cycle(repo, packdir, &pack_exclude,
				   &loose_exclude, &midx_exclude,
				   min_loose, min_packs,
				   max_loose_objects, max_objects,
				   max_packs);
		trace2_data_intmax("pack-aggregate", repo, "cycle-num",
				   ++cycle_count);
		trace2_region_leave("pack-aggregate", "cycle", repo);
		if (ret || once || stop_signaled)
			break;
		interruptible_sleep((unsigned int)interval);
	} while (!stop_signaled);

	strset_clear(&pack_exclude);
	strset_clear(&loose_exclude);
	strset_clear(&midx_exclude);
	free(packdir);
	return ret ? 1 : 0;
}
