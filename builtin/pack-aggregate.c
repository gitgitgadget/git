#define USE_THE_REPOSITORY_VARIABLE

#include "builtin.h"
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
#include "wrapper.h"

static const char *const pack_aggregate_usage[] = {
	N_("git pack-aggregate (--once | --loop) [--interval=<seconds>]\n"
	   "                  [--min-loose=<n>] [--min-packs=<n>]\n"
	   "                  [--exclude-pack-file=<path>]\n"
	   "                  [--exclude-loose-file=<path>]\n"
	   "                  [--parent-pipe-fd=<n>]"),
	NULL
};

static volatile sig_atomic_t stop_signaled;
static volatile sig_atomic_t active_child_pid;
static int parent_pipe_fd = -1;

static void term_handler(int sig)
{
	stop_signaled = 1;
	/*
	 * Best-effort forward to an in-flight pack-objects so it
	 * doesn't keep running after we have been asked to stop.
	 * The check-and-signal is racy with finish_command() in the
	 * main thread; the worst case is signaling a pid we no longer
	 * own (kill() returns ESRCH and we ignore it).
	 */
	if (active_child_pid > 0)
		kill(active_child_pid, sig);
}

static int is_hex(const char *s, size_t len)
{
	while (len--)
		if (!isxdigit(*s++))
			return 0;
	return 1;
}

static int looks_like_pack_basename(const char *name, size_t hexsz)
{
	static const char prefix[] = "pack-";
	size_t plen = strlen(prefix);
	size_t nlen = strlen(name);

	/* Does this look like "pack-<hex>" with no extension? */
	if (nlen != plen + hexsz || strncmp(name, prefix, plen))
		return 0;
	return is_hex(name + plen, hexsz);
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

static int has_idx(const char *packdir, const char *basename)
{
	return has_sidecar(packdir, basename, "idx");
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
	struct multi_pack_index *m;
	int had_midx = 0;

	strset_clear(set);

	odb_reprepare(repo->objects);
	m = get_multi_pack_index(repo->objects->sources);
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
	return had_midx;
}

/* ---------- loose-object pre-pass ---------- */

struct loose_scan {
	struct strset *exclude;
	struct oid_array *oids;
	struct string_list *paths;
};

static int loose_scan_cb(const struct object_id *oid, const char *path,
			 void *cb_data)
{
	struct loose_scan *data = cb_data;

	if (strset_contains(data->exclude, oid_to_hex(oid)))
		return 0;
	oid_array_append(data->oids, oid);
	string_list_append(data->paths, path);
	return 0;
}

static int run_pack_objects_loose(const char *packtmp, struct oid_array *oids,
				  struct strbuf *out_hash)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct strbuf line = STRBUF_INIT;
	FILE *in, *out;
	size_t i;
	int ret;

	strvec_pushl(&cmd.args,
		     "pack-objects",
		     "--window=0",
		     "--mark-bad-deltas",
		     "--delta-base-offset",
		     "--no-write-bitmap-index",
		     "--quiet",
		     packtmp,
		     NULL);
	cmd.git_cmd = 1;
	cmd.in = -1;
	cmd.out = -1;

	if (start_command(&cmd))
		return -1;
	active_child_pid = cmd.pid;

	/*
	 * The order is "write all stdin, then read all stdout".  This
	 * is only safe because --quiet bounds pack-objects's stdout to
	 * a single short pack-hash line, which fits in the pipe buffer;
	 * pack-objects can therefore drain its stdin without blocking
	 * on stdout writes back to us.
	 */
	in = xfdopen(cmd.in, "w");
	for (i = 0; i < oids->nr; i++)
		fprintf(in, "%s\n", oid_to_hex(&oids->oid[i]));
	if (fclose(in))
		warning_errno(_("could not close pack-objects input"));

	/*
	 * Capture the first line as the pack hash, then drain to EOF so
	 * pack-objects observes a clean close on its stdout and we never
	 * end up in finish_command() with bytes left in the pipe.
	 */
	out = xfdopen(cmd.out, "r");
	while (strbuf_getline_lf(&line, out) != EOF) {
		if (!out_hash->len)
			strbuf_addbuf(out_hash, &line);
	}
	fclose(out);

	ret = finish_command(&cmd);
	active_child_pid = 0;
	strbuf_release(&line);
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

struct pack_scan {
	const char *packdir;
	struct strset *file_exclude;
	struct strset *midx_exclude;
	struct string_list *candidates;
	size_t hexsz;
};

static void pack_scan_cb(const char *full_path UNUSED,
			 size_t full_path_len UNUSED,
			 const char *file_name, void *cb_data)
{
	struct pack_scan *data = cb_data;
	struct strbuf base = STRBUF_INIT;

	if (!ends_with(file_name, ".pack"))
		return;
	strbuf_addstr(&base, file_name);
	strbuf_setlen(&base, base.len - strlen(".pack"));

	if (!looks_like_pack_basename(base.buf, data->hexsz))
		goto out;
	if (strset_contains(data->file_exclude, base.buf))
		goto out;
	if (strset_contains(data->midx_exclude, base.buf))
		goto out;
	if (has_protective_sidecar(data->packdir, base.buf))
		goto out;
	if (!has_idx(data->packdir, base.buf))
		goto out;

	string_list_append(data->candidates, base.buf);
out:
	strbuf_release(&base);
}

static int run_pack_objects_packs(const char *packtmp,
				  const struct string_list *bases,
				  struct strbuf *out_hash)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct strbuf line = STRBUF_INIT;
	FILE *in, *out;
	size_t i;
	int ret;

	strvec_pushl(&cmd.args,
		     "pack-objects",
		     "--stdin-packs",
		     "--window=0",
		     "--mark-bad-deltas",
		     "--delta-base-offset",
		     "--no-write-bitmap-index",
		     "--quiet",
		     packtmp,
		     NULL);
	cmd.git_cmd = 1;
	cmd.in = -1;
	cmd.out = -1;

	if (start_command(&cmd))
		return -1;
	active_child_pid = cmd.pid;

	/* See run_pack_objects_loose() for why write-then-read is safe. */
	in = xfdopen(cmd.in, "w");
	for (i = 0; i < bases->nr; i++)
		fprintf(in, "%s.pack\n", bases->items[i].string);
	if (fclose(in))
		warning_errno(_("could not close pack-objects input"));

	out = xfdopen(cmd.out, "r");
	while (strbuf_getline_lf(&line, out) != EOF) {
		if (!out_hash->len)
			strbuf_addbuf(out_hash, &line);
	}
	fclose(out);

	ret = finish_command(&cmd);
	active_child_pid = 0;
	strbuf_release(&line);
	return ret;
}

/*
 * pack-objects writes its output as <packtmp>-<hash>.<ext>; rename it
 * into place as <packdir>/pack-<hash>.<ext>.  .idx is renamed last so
 * a concurrent reader scanning the pack directory never sees a .idx
 * without its companion .pack.
 */
static void install_pack(const char *packtmp, const char *packdir,
			 const char *hash)
{
	static const char *exts[] = {
		".pack", ".rev", ".baddeltas", ".idx"
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(exts); i++) {
		struct strbuf src = STRBUF_INIT;
		struct strbuf dst = STRBUF_INIT;
		struct stat st;

		strbuf_addf(&src, "%s-%s%s", packtmp, hash, exts[i]);
		strbuf_addf(&dst, "%s/pack-%s%s", packdir, hash, exts[i]);

		if (!stat(src.buf, &st)) {
			if (chmod(src.buf, st.st_mode & ~(S_IWUSR | S_IWGRP | S_IWOTH)))
				warning_errno(_("could not make '%s' read-only"),
					      src.buf);
			if (rename(src.buf, dst.buf))
				die_errno(_("renaming '%s' to '%s' failed"),
					  src.buf, dst.buf);
		} else if (errno != ENOENT) {
			die_errno(_("could not stat '%s'"), src.buf);
		}
		strbuf_release(&src);
		strbuf_release(&dst);
	}
}

static void unlink_consumed_packs(const char *packdir,
				  const struct string_list *bases,
				  const char *keep_basename)
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
			int min_loose, int min_packs)
{
	struct oid_array loose_oids = OID_ARRAY_INIT;
	struct string_list loose_paths = STRING_LIST_INIT_DUP;
	struct loose_scan loose_data = {
		.exclude = loose_exclude,
		.oids = &loose_oids,
		.paths = &loose_paths,
	};
	struct string_list candidates = STRING_LIST_INIT_DUP;
	struct pack_scan pack_data = {
		.packdir = packdir,
		.file_exclude = pack_exclude,
		.midx_exclude = midx_exclude,
		.candidates = &candidates,
		.hexsz = repo->hash_algo->hexsz,
	};
	struct strbuf loose_hash = STRBUF_INIT;
	struct strbuf packs_hash = STRBUF_INIT;
	char *packtmp_loose = NULL;
	char *packtmp_packs = NULL;
	int ret = 0;

	/*
	 * Step 1: bundle local loose objects (minus excluded ones) into
	 * a single new pack and remove the on-disk loose copies.  The
	 * resulting pack picks up a .baddeltas marker thanks to
	 * --mark-bad-deltas, and is in turn a candidate for the pack
	 * aggregation step below.
	 */
	for_each_loose_file_in_source(repo->objects->sources,
				      loose_scan_cb, NULL, NULL, &loose_data);
	if ((int)loose_oids.nr >= min_loose && !stop_signaled) {
		packtmp_loose = mkpathdup("%s/.tmp-%d-loose-pack",
					  packdir, (int)getpid());
		if (run_pack_objects_loose(packtmp_loose, &loose_oids,
					   &loose_hash)) {
			ret = error(_("pack-objects failed during "
				      "loose-object rollup"));
			goto out;
		}
		if (loose_hash.len) {
			install_pack(packtmp_loose, packdir, loose_hash.buf);
			unlink_loose_paths(&loose_paths);
		}
	}

	if (stop_signaled)
		goto out;

	/*
	 * Step 2: aggregate small packs into a single bigger pack.  The
	 * freshly-installed loose-rollup pack from step 1 is picked up
	 * naturally by the directory scan below (it has no protective
	 * sidecars and isn't in any exclusion set).  Re-read the MIDX
	 * just before scanning so a pack added to a MIDX between cycles
	 * is honored.
	 */
	refresh_midx_exclusions(repo, midx_exclude);
	for_each_file_in_pack_dir(repo_get_object_directory(repo),
				  pack_scan_cb, &pack_data);

	if ((int)candidates.nr < min_packs)
		goto out;

	packtmp_packs = mkpathdup("%s/.tmp-%d-pack",
				  packdir, (int)getpid());
	if (run_pack_objects_packs(packtmp_packs, &candidates,
				   &packs_hash)) {
		ret = error(_("pack-objects failed during "
			      "pack aggregation"));
		goto out;
	}
	if (packs_hash.len) {
		struct strbuf output_base = STRBUF_INIT;
		install_pack(packtmp_packs, packdir, packs_hash.buf);
		strbuf_addf(&output_base, "pack-%s", packs_hash.buf);
		unlink_consumed_packs(packdir, &candidates, output_base.buf);
		strbuf_release(&output_base);
	}

out:
	free(packtmp_loose);
	free(packtmp_packs);
	strbuf_release(&loose_hash);
	strbuf_release(&packs_hash);
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
	int once = 0;
	int loop = 0;
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
		ret = do_one_cycle(repo, packdir, &pack_exclude,
				   &loose_exclude, &midx_exclude,
				   min_loose, min_packs);
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
