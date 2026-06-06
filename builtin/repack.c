#define USE_THE_REPOSITORY_VARIABLE
#define DISABLE_SIGN_COMPARE_WARNINGS

#include "builtin.h"
#include "config.h"
#include "environment.h"
#include "gettext.h"
#include "parse-options.h"
#include "path.h"
#include "run-command.h"
#include "server-info.h"
#include "string-list.h"
#include "midx.h"
#include "packfile.h"
#include "prune-packed.h"
#include "promisor-remote.h"
#include "repack.h"
#include "shallow.h"
#include "dir.h"
#include "strbuf.h"
#include "strvec.h"
#include "wrapper.h"

#define ALL_INTO_ONE 1
#define LOOSEN_UNREACHABLE 2
#define PACK_CRUFT 4

#define DELETE_PACK 1
#define RETAIN_PACK 2

static int pack_everything;
static int write_bitmaps = -1;
static int use_delta_islands;
static int run_update_server_info = 1;
static char *packdir, *packtmp_name, *packtmp;
static int midx_must_contain_cruft = 1;
static int aggregate_opt = -1;

static const char *const git_repack_usage[] = {
	N_("git repack [-a] [-A] [-d] [-f] [-F] [-l] [-n] [-q] [-b] [-m]\n"
	   "[--window=<n>] [--depth=<n>] [--threads=<n>] [--keep-pack=<pack-name>]\n"
	   "[--write-midx[=<mode>]] [--name-hash-version=<n>] [--path-walk]\n"
	   "[--[no-]aggregate]"),
	NULL
};

static const char incremental_bitmap_conflict_error[] = N_(
"Incremental repacks are incompatible with bitmap indexes.  Use\n"
"--no-write-bitmap-index or disable the pack.writeBitmaps configuration."
);

#define DEFAULT_MIDX_SPLIT_FACTOR 2
#define DEFAULT_MIDX_NEW_LAYER_THRESHOLD 8

struct repack_config_ctx {
	struct pack_objects_args *po_args;
	struct pack_objects_args *cruft_po_args;
	int midx_split_factor;
	int midx_new_layer_threshold;
};

static int repack_config(const char *var, const char *value,
			 const struct config_context *ctx, void *cb)
{
	struct repack_config_ctx *repack_ctx = cb;
	struct pack_objects_args *po_args = repack_ctx->po_args;
	struct pack_objects_args *cruft_po_args = repack_ctx->cruft_po_args;
	if (!strcmp(var, "repack.usedeltabaseoffset")) {
		po_args->delta_base_offset = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.packkeptobjects")) {
		po_args->pack_kept_objects = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.writebitmaps") ||
	    !strcmp(var, "pack.writebitmaps")) {
		write_bitmaps = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.usedeltaislands")) {
		use_delta_islands = git_config_bool(var, value);
		return 0;
	}
	if (strcmp(var, "repack.updateserverinfo") == 0) {
		run_update_server_info = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.cruftwindow")) {
		free(cruft_po_args->window);
		return git_config_string(&cruft_po_args->window, var, value);
	}
	if (!strcmp(var, "repack.cruftwindowmemory")) {
		free(cruft_po_args->window_memory);
		return git_config_string(&cruft_po_args->window_memory, var, value);
	}
	if (!strcmp(var, "repack.cruftdepth")) {
		free(cruft_po_args->depth);
		return git_config_string(&cruft_po_args->depth, var, value);
	}
	if (!strcmp(var, "repack.cruftthreads")) {
		free(cruft_po_args->threads);
		return git_config_string(&cruft_po_args->threads, var, value);
	}
	if (!strcmp(var, "repack.midxmustcontaincruft")) {
		midx_must_contain_cruft = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.midxsplitfactor")) {
		repack_ctx->midx_split_factor = git_config_int(var, value,
							       ctx->kvi);
		return 0;
	}
	if (!strcmp(var, "repack.midxnewlayerthreshold")) {
		repack_ctx->midx_new_layer_threshold = git_config_int(var, value,
								      ctx->kvi);
		return 0;
	}
	if (!strcmp(var, "repack.aggregate")) {
		aggregate_opt = git_config_bool(var, value);
		return 0;
	}
	return git_default_config(var, value, ctx, cb);
}

static int option_parse_write_midx(const struct option *opt, const char *arg,
				   int unset)
{
	enum repack_write_midx_mode *cfg = opt->value;

	if (unset) {
		*cfg = REPACK_WRITE_MIDX_NONE;
		return 0;
	}

	if (!arg || !*arg)
		*cfg = REPACK_WRITE_MIDX_DEFAULT;
	else if (!strcmp(arg, "incremental"))
		*cfg = REPACK_WRITE_MIDX_INCREMENTAL;
	else
		return error(_("unknown value for %s: %s"), opt->long_name, arg);

	return 0;
}

/*
 * State for the optional `git pack-aggregate` sidecar that we
 * launch alongside the main pack-objects when --aggregate is on.
 */
struct aggregate_sidecar {
	struct child_process cmd;
	char *tmpdir;
	char *exclude_packs_path;
	char *exclude_loose_path;
	/*
	 * Full paths of the ".keep" marker files we created in this
	 * repack.  Cleared (and the files unlinked) after the sidecar
	 * has been reaped, so that pack-aggregate never sees a pack
	 * lose its marker while it is still running.
	 */
	struct string_list installed_keeps;
	/*
	 * Write end of a pipe whose read end the aggregator inherits
	 * (passed via --parent-pipe-fd).  Closing this fd is the
	 * orphan-detection signal: when repack exits without an
	 * explicit stop_aggregate_sidecar(), the aggregator sees EOF
	 * and bails out instead of running unsupervised.
	 */
	int parent_pipe_write_fd;
	int started;
};
#define AGGREGATE_KEEP_MARKER_PREFIX "git-repack-aggregate-temporary"

/*
 * Scan packdir for stale ".keep" markers left behind by previous
 * crashed repacks: files whose contents begin with our marker
 * prefix and whose owning pid is no longer running.  Skip anything
 * we cannot parse (foreign ".keep" files) or whose owner is alive.
 */
static void clean_stale_aggregate_keeps(const char *packdir)
{
	DIR *dir = opendir(packdir);
	struct dirent *ent;
	struct strbuf path = STRBUF_INIT;

	if (!dir)
		return;

	while ((ent = readdir(dir))) {
		const char *p;
		char *end;
		long pid;
		int fd;
		ssize_t n;
		char buf[256];

		if (!ends_with(ent->d_name, ".keep"))
			continue;

		strbuf_reset(&path);
		strbuf_addf(&path, "%s/%s", packdir, ent->d_name);

		fd = open(path.buf, O_RDONLY);
		if (fd < 0)
			continue;
		n = read_in_full(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = '\0';

		if (!skip_prefix(buf, AGGREGATE_KEEP_MARKER_PREFIX " pid=", &p))
			continue;
		errno = 0;
		pid = strtol(p, &end, 10);
		if (errno || end == p || pid <= 0)
			continue;
		if (*end != '\n' && *end != '\0')
			continue;

		/* Send no signal; just check existence of process */
		if (!kill((pid_t)pid, 0))
			continue;
		if (errno != ESRCH)
			continue;

		if (unlink(path.buf) && errno != ENOENT)
			warning_errno(_("could not unlink stale "
					"aggregate .keep marker '%s'"),
				      path.buf);
	}

	closedir(dir);
	strbuf_release(&path);
}

/*
 * Drop a ".keep" marker on every newly written pack so that the
 * aggregator (which respects ".keep") cannot pick up one of the
 * outputs from the primary repacking work.  We do this BEFORE the
 * install loop so the marker is in place the moment
 * generated_pack_install() renames ".idx" into view.
 *
 * We use O_CREAT|O_EXCL: if a ".keep" already exists for that
 * basename (e.g. user-created, or a stale marker we failed to
 * clean), we leave it alone and do NOT record it for cleanup --
 * cleanup must only ever unlink markers we ourselves created.
 */
static void install_aggregate_keep_markers(struct aggregate_sidecar *sc,
					   const char *packdir,
					   const struct string_list *names)
{
	struct strbuf path = STRBUF_INIT;
	struct strbuf content = STRBUF_INIT;
	size_t i;

	strbuf_addf(&content, "%s pid=%ld\n",
		    AGGREGATE_KEEP_MARKER_PREFIX, (long)getpid());

	for (i = 0; i < names->nr; i++) {
		int fd;

		strbuf_reset(&path);
		strbuf_addf(&path, "%s/pack-%s.keep", packdir,
			    names->items[i].string);

		fd = open(path.buf, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (fd < 0) {
			if (errno == EEXIST)
				continue;
			die_errno(_("could not create aggregate "
				    ".keep marker '%s'"), path.buf);
		}
		if (write_in_full(fd, content.buf, content.len) < 0)
			die_errno(_("could not write aggregate "
				    ".keep marker '%s'"), path.buf);
		close(fd);
		string_list_append(&sc->installed_keeps, path.buf);
	}

	strbuf_release(&path);
	strbuf_release(&content);
}

/*
 * Unlink every ".keep" marker we created earlier.  Must be called
 * AFTER stop_aggregate_sidecar() has reaped pack-aggregate, so that
 * the aggregator cannot see a pack lose its protective marker while
 * it is still running.
 */
static void remove_aggregate_keep_markers(struct aggregate_sidecar *sc)
{
	size_t i;

	for (i = 0; i < sc->installed_keeps.nr; i++) {
		const char *p = sc->installed_keeps.items[i].string;
		if (unlink(p) && errno != ENOENT)
			warning_errno(_("could not unlink aggregate "
					".keep marker '%s'"), p);
	}
	string_list_clear(&sc->installed_keeps, 0);
}

static int wait_for_emit_files(const char *packs_path,
			       const char *loose_path,
			       int pack_objects_out_fd)
{
	struct stat st;
	int waited_ms = 0;
	int sleep_ms = 20;
	const int max_wait_ms = 60000;
	struct pollfd pfd;

	/*
	 * POLLHUP is reported in revents regardless of whether it
	 * appears in events, so we leave events==0; a hang-up on
	 * pack-objects' stdout pipe means it has exited.  pack-objects
	 * writes its emit files immediately after enumerating its
	 * inputs and well before producing any output, so during this
	 * wait there is nothing readable on the pipe to confuse us.
	 *
	 * We never waitpid() here: finish_pack_objects_cmd() reaps the
	 * child later; reaping twice would be incorrect.
	 */
	pfd.fd = pack_objects_out_fd;
	pfd.events = 0;

	while (waited_ms < max_wait_ms) {
		int ok_packs = (stat(packs_path, &st) == 0);
		int ok_loose = (stat(loose_path, &st) == 0);
		int ret;
		if (ok_packs && ok_loose)
			return 0;
		pfd.revents = 0;
		ret = poll(&pfd, 1, sleep_ms);
		if (ret < 0 && errno != EINTR)
			return error_errno(_("poll on pack-objects "
					     "pipe failed"));
		if (ret > 0 &&
		    (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)))
			return error(_("pack-objects exited before "
				       "emitting exclude files"));
		waited_ms += sleep_ms;
		if (sleep_ms < 200)
			sleep_ms *= 2;
	}
	return error(_("timed out waiting for pack-objects to write "
		       "exclude files"));
}

static int start_aggregate_sidecar(struct aggregate_sidecar *sc,
				   int pack_objects_out_fd)
{
	int pp[2];
	int flags;

	if (wait_for_emit_files(sc->exclude_packs_path,
				sc->exclude_loose_path,
				pack_objects_out_fd))
		return -1;

	/*
	 * Build the orphan-detection pipe: child inherits the read
	 * end as --parent-pipe-fd; we keep the write end.  Mark our
	 * end close-on-exec so the exec'd aggregator does not inherit
	 * a second writer that would prevent it from seeing EOF when
	 * we exit.
	 */
	if (pipe(pp))
		return error_errno(_("could not create aggregate "
				     "parent pipe"));
	flags = fcntl(pp[1], F_GETFD);
	if (flags >= 0)
		fcntl(pp[1], F_SETFD, flags | FD_CLOEXEC);

	strvec_pushl(&sc->cmd.args, "pack-aggregate", "--loop", NULL);
	strvec_pushf(&sc->cmd.args, "--exclude-pack-file=%s",
		     sc->exclude_packs_path);
	strvec_pushf(&sc->cmd.args, "--exclude-loose-file=%s",
		     sc->exclude_loose_path);
	strvec_pushf(&sc->cmd.args, "--parent-pipe-fd=%d", pp[0]);
	/*
	 * Allow tests (which cannot sit through the default 60s
	 * aggregator interval) to override --interval, --min-packs,
	 * and --min-loose without us having to add separate
	 * pass-through options.
	 */
	if (getenv("GIT_TEST_PACK_AGGREGATE_INTERVAL"))
		strvec_pushf(&sc->cmd.args, "--interval=%s",
			     getenv("GIT_TEST_PACK_AGGREGATE_INTERVAL"));
	if (getenv("GIT_TEST_PACK_AGGREGATE_MIN_PACKS"))
		strvec_pushf(&sc->cmd.args, "--min-packs=%s",
			     getenv("GIT_TEST_PACK_AGGREGATE_MIN_PACKS"));
	if (getenv("GIT_TEST_PACK_AGGREGATE_MIN_LOOSE"))
		strvec_pushf(&sc->cmd.args, "--min-loose=%s",
			     getenv("GIT_TEST_PACK_AGGREGATE_MIN_LOOSE"));
	sc->cmd.git_cmd = 1;
	if (start_command(&sc->cmd)) {
		close(pp[0]);
		close(pp[1]);
		return error(_("could not start git pack-aggregate"));
	}
	/*
	 * The aggregator now has its own copy of pp[0]; the write end
	 * is what gives us orphan detection, and the parent's read end
	 * is just a held-open fd.  Free it.
	 */
	close(pp[0]);
	sc->parent_pipe_write_fd = pp[1];
	sc->started = 1;
	return 0;
}

static void stop_aggregate_sidecar(struct aggregate_sidecar *sc)
{
	/*
	 * Close the parent pipe up front: if the aggregator is between
	 * cycles it will wake from poll() and exit on its own, which
	 * lets the SIGTERM/wait dance below complete quickly.
	 */
	if (sc->parent_pipe_write_fd >= 0) {
		close(sc->parent_pipe_write_fd);
		sc->parent_pipe_write_fd = -1;
	}
	if (sc->started) {
		sc->started = 0;
		if (sc->cmd.pid > 0) {
			pid_t pid = sc->cmd.pid;
			int waited_ms = 0;
			int status;
			pid_t r;

			kill(pid, SIGTERM);
			/*
			 * Wait briefly for graceful exit before reaping;
			 * pack-aggregate is expected to finish quite quickly,
			 * so a few seconds should be plenty.
			 */
			while (waited_ms < 5000) {
				r = waitpid(pid, &status, WNOHANG);
				if (r == pid || (r < 0 && errno != EINTR))
					break;
				sleep_millisec(50);
				waited_ms += 50;
			}
			if (r != pid) {
				kill(pid, SIGKILL);
				while (waitpid(pid, &status, 0) < 0 &&
				       errno == EINTR)
					; /* nothing */
			}
			/*
			 * We never call finish_command() here, so the
			 * normal cleanup-on-exit list still has us
			 * registered; explicitly remove ourselves so
			 * the cleanup handler does not try to reap us
			 * again.
			 */
			sc->cmd.pid = -1;
			child_process_clear(&sc->cmd);
		}
	}
	if (sc->tmpdir) {
		struct strbuf path = STRBUF_INIT;
		strbuf_addstr(&path, sc->tmpdir);
		remove_dir_recursively(&path, 0);
		strbuf_release(&path);
		FREE_AND_NULL(sc->tmpdir);
	}
	FREE_AND_NULL(sc->exclude_packs_path);
	FREE_AND_NULL(sc->exclude_loose_path);

	/*
	 * Cleanup of the ".keep" markers we dropped on our own
	 * outputs MUST happen after the sidecar has exited,
	 * otherwise pack-aggregate could see a pack lose its marker
	 * and aggregate it out from under us.
	 */
	remove_aggregate_keep_markers(sc);
}

/*
 * Initialize the aggregator's tempdir and per-file paths.  This is
 * called before pack-objects starts so the files' paths can be
 * passed to it via --emit-input-{packs,loose}.  We also opportunistically
 * sweep any stale ".keep" markers left behind by a previous repack
 * that crashed; doing this once at init avoids repeated scans.
 */
static int init_aggregate_sidecar(struct repository *repo,
				  struct aggregate_sidecar *sc)
{
	struct strbuf tmpl = STRBUF_INIT;
	char *pdir;

	pdir = mkpathdup("%s/pack", repo_get_object_directory(repo));
	clean_stale_aggregate_keeps(pdir);
	free(pdir);

	strbuf_addf(&tmpl, "%s/pack-aggregate.XXXXXX",
		    repo_get_object_directory(repo));
	if (!git_mkdtemp(tmpl.buf)) {
		error_errno(_("could not create pack-aggregate tempdir"));
		strbuf_release(&tmpl);
		return -1;
	}
	sc->tmpdir = strbuf_detach(&tmpl, NULL);
	sc->exclude_packs_path = xstrfmt("%s/packs", sc->tmpdir);
	sc->exclude_loose_path = xstrfmt("%s/loose", sc->tmpdir);
	return 0;
}

int cmd_repack(int argc,
	       const char **argv,
	       const char *prefix,
	       struct repository *repo)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct aggregate_sidecar sidecar = {
		.cmd = CHILD_PROCESS_INIT,
		.installed_keeps = STRING_LIST_INIT_DUP,
		.parent_pipe_write_fd = -1,
	};
	struct string_list_item *item;
	struct string_list names = STRING_LIST_INIT_DUP;
	struct existing_packs existing = EXISTING_PACKS_INIT;
	struct pack_geometry geometry = { 0 };
	struct tempfile *refs_snapshot = NULL;
	int i, ret;
	int show_progress;

	/* variables to be filled by option parsing */
	struct repack_config_ctx config_ctx;
	int delete_redundant = 0;
	const char *unpack_unreachable = NULL;
	int keep_unreachable = 0;
	struct string_list keep_pack_list = STRING_LIST_INIT_NODUP;
	struct pack_objects_args po_args = PACK_OBJECTS_ARGS_INIT;
	struct pack_objects_args cruft_po_args = PACK_OBJECTS_ARGS_INIT;
	enum repack_write_midx_mode write_midx = REPACK_WRITE_MIDX_NONE;
	const char *cruft_expiration = NULL;
	const char *expire_to = NULL;
	const char *filter_to = NULL;
	const char *opt_window = NULL;
	const char *opt_window_memory = NULL;
	const char *opt_depth = NULL;
	const char *opt_threads = NULL;
	unsigned long combine_cruft_below_size = 0ul;

	struct option builtin_repack_options[] = {
		OPT_BIT('a', NULL, &pack_everything,
				N_("pack everything in a single pack"), ALL_INTO_ONE),
		OPT_BIT('A', NULL, &pack_everything,
				N_("same as -a, and turn unreachable objects loose"),
				   LOOSEN_UNREACHABLE | ALL_INTO_ONE),
		OPT_BIT(0, "cruft", &pack_everything,
				N_("same as -a, pack unreachable cruft objects separately"),
				   PACK_CRUFT),
		OPT_STRING(0, "cruft-expiration", &cruft_expiration, N_("approxidate"),
				N_("with --cruft, expire objects older than this")),
		OPT_UNSIGNED(0, "combine-cruft-below-size",
			     &combine_cruft_below_size,
			     N_("with --cruft, only repack cruft packs smaller than this")),
		OPT_UNSIGNED(0, "max-cruft-size", &cruft_po_args.max_pack_size,
			     N_("with --cruft, limit the size of new cruft packs")),
		OPT_BOOL('d', NULL, &delete_redundant,
				N_("remove redundant packs, and run git-prune-packed")),
		OPT_BOOL('f', NULL, &po_args.no_reuse_delta,
				N_("pass --no-reuse-delta to git-pack-objects")),
		OPT_BOOL('F', NULL, &po_args.no_reuse_object,
				N_("pass --no-reuse-object to git-pack-objects")),
		OPT_INTEGER(0, "name-hash-version", &po_args.name_hash_version,
				N_("specify the name hash version to use for grouping similar objects by path")),
		OPT_BOOL(0, "path-walk", &po_args.path_walk,
				N_("pass --path-walk to git-pack-objects")),
		OPT_NEGBIT('n', NULL, &run_update_server_info,
				N_("do not run git-update-server-info"), 1),
		OPT__QUIET(&po_args.quiet, N_("be quiet")),
		OPT_BOOL('l', "local", &po_args.local,
				N_("pass --local to git-pack-objects")),
		OPT_BOOL('b', "write-bitmap-index", &write_bitmaps,
				N_("write bitmap index")),
		OPT_BOOL('i', "delta-islands", &use_delta_islands,
				N_("pass --delta-islands to git-pack-objects")),
		OPT_STRING(0, "unpack-unreachable", &unpack_unreachable, N_("approxidate"),
				N_("with -A, do not loosen objects older than this")),
		OPT_BOOL('k', "keep-unreachable", &keep_unreachable,
				N_("with -a, repack unreachable objects")),
		OPT_STRING(0, "window", &opt_window, N_("n"),
				N_("size of the window used for delta compression")),
		OPT_STRING(0, "window-memory", &opt_window_memory, N_("bytes"),
				N_("same as the above, but limit memory size instead of entries count")),
		OPT_STRING(0, "depth", &opt_depth, N_("n"),
				N_("limits the maximum delta depth")),
		OPT_STRING(0, "threads", &opt_threads, N_("n"),
				N_("limits the maximum number of threads")),
		OPT_UNSIGNED(0, "max-pack-size", &po_args.max_pack_size,
			     N_("maximum size of each packfile")),
		OPT_PARSE_LIST_OBJECTS_FILTER(&po_args.filter_options),
		OPT_BOOL(0, "pack-kept-objects", &po_args.pack_kept_objects,
				N_("repack objects in packs marked with .keep")),
		OPT_STRING_LIST(0, "keep-pack", &keep_pack_list, N_("name"),
				N_("do not repack this pack")),
		OPT_INTEGER('g', "geometric", &geometry.split_factor,
			    N_("find a geometric progression with factor <N>")),
		OPT_CALLBACK_F(0, "write-midx", &write_midx,
			   N_("mode"),
			   N_("write a multi-pack index of the resulting packs"),
			   PARSE_OPT_OPTARG, option_parse_write_midx),
		OPT_SET_INT_F('m', NULL, &write_midx,
			   N_("write a multi-pack index of the resulting packs"),
			   REPACK_WRITE_MIDX_DEFAULT,
			   PARSE_OPT_HIDDEN),
		OPT_STRING(0, "expire-to", &expire_to, N_("dir"),
			   N_("pack prefix to store a pack containing pruned objects")),
		OPT_STRING(0, "filter-to", &filter_to, N_("dir"),
			   N_("pack prefix to store a pack containing filtered out objects")),
		OPT_BOOL(0, "aggregate", &aggregate_opt,
			 N_("run a background pack-aggregate to roll up small packs and loose objects while this repack runs")),
		OPT_END()
	};

	list_objects_filter_init(&po_args.filter_options);

	memset(&config_ctx, 0, sizeof(config_ctx));
	config_ctx.po_args = &po_args;
	config_ctx.cruft_po_args = &cruft_po_args;
	config_ctx.midx_split_factor = DEFAULT_MIDX_SPLIT_FACTOR;
	config_ctx.midx_new_layer_threshold = DEFAULT_MIDX_NEW_LAYER_THRESHOLD;

	repo_config(repo, repack_config, &config_ctx);

	argc = parse_options(argc, argv, prefix, builtin_repack_options,
				git_repack_usage, 0);

	po_args.window = xstrdup_or_null(opt_window);
	po_args.window_memory = xstrdup_or_null(opt_window_memory);
	po_args.depth = xstrdup_or_null(opt_depth);
	po_args.threads = xstrdup_or_null(opt_threads);

	if (delete_redundant && repo->repository_format_precious_objects)
		die(_("cannot delete packs in a precious-objects repo"));

	die_for_incompatible_opt3(unpack_unreachable || (pack_everything & LOOSEN_UNREACHABLE), "-A",
				  keep_unreachable, "-k/--keep-unreachable",
				  pack_everything & PACK_CRUFT, "--cruft");

	if (pack_everything & PACK_CRUFT)
		pack_everything |= ALL_INTO_ONE;

	if (write_bitmaps < 0) {
		if (write_midx == REPACK_WRITE_MIDX_NONE &&
		    (!(pack_everything & ALL_INTO_ONE) || !is_bare_repository()))
			write_bitmaps = 0;
	}
	if (po_args.pack_kept_objects < 0)
		po_args.pack_kept_objects = write_bitmaps > 0 &&
			write_midx == REPACK_WRITE_MIDX_NONE;

	if (write_bitmaps && !(pack_everything & ALL_INTO_ONE) &&
	    write_midx == REPACK_WRITE_MIDX_NONE)
		die(_(incremental_bitmap_conflict_error));

	if (write_bitmaps && po_args.local &&
	    odb_has_alternates(repo->objects)) {
		/*
		 * When asked to do a local repack, but we have
		 * packfiles that are inherited from an alternate, then
		 * we cannot guarantee that the multi-pack-index would
		 * have full coverage of all objects. We thus disable
		 * writing bitmaps in that case.
		 */
		warning(_("disabling bitmap writing, as some objects are not being packed"));
		write_bitmaps = 0;
	}

	if (config_ctx.midx_split_factor < 2)
		die(_("invalid value for %s: %d"), "--midx-split-factor",
		    config_ctx.midx_split_factor);
	if (config_ctx.midx_new_layer_threshold < 1)
		die(_("invalid value for %s: %d"), "--midx-new-layer-threshold",
		    config_ctx.midx_new_layer_threshold);

	if (write_midx != REPACK_WRITE_MIDX_NONE && write_bitmaps) {
		struct strbuf path = STRBUF_INIT;

		strbuf_addf(&path, "%s/%s_XXXXXX",
			    repo_get_object_directory(repo),
			    "bitmap-ref-tips");

		refs_snapshot = xmks_tempfile(path.buf);
		midx_snapshot_refs(repo, refs_snapshot);

		strbuf_release(&path);
	}

	packdir = mkpathdup("%s/pack", repo_get_object_directory(repo));
	packtmp_name = xstrfmt(".tmp-%d-pack", (int)getpid());
	packtmp = mkpathdup("%s/%s", packdir, packtmp_name);

	existing.repo = repo;
	existing_packs_collect(&existing, &keep_pack_list);

	if (geometry.split_factor) {
		if (pack_everything)
			die(_("options '%s' and '%s' cannot be used together"), "--geometric", "-A/-a");
		if (write_midx == REPACK_WRITE_MIDX_INCREMENTAL) {
			geometry.midx_layer_threshold = config_ctx.midx_new_layer_threshold;
			geometry.midx_layer_threshold_set = true;
		}
		pack_geometry_init(&geometry, &existing, &po_args);
		pack_geometry_split(&geometry);
	}

	prepare_pack_objects(&cmd, &po_args, packtmp);

	show_progress = !po_args.quiet && isatty(2);

	/*
	 * If --aggregate is requested, set up a tempdir and tell the
	 * main pack-objects to write its input pack/loose lists into
	 * it.
	 */
	if (aggregate_opt > 0) {
		if (init_aggregate_sidecar(repo, &sidecar)) {
			/* fall through: error already reported */
			aggregate_opt = 0;
		} else {
			strvec_pushf(&cmd.args, "--emit-input-packs=%s",
				     sidecar.exclude_packs_path);
			strvec_pushf(&cmd.args, "--emit-input-loose=%s",
				     sidecar.exclude_loose_path);
		}
	}

	strvec_push(&cmd.args, "--keep-true-parents");
	for (i = 0; i < keep_pack_list.nr; i++)
		strvec_pushf(&cmd.args, "--keep-pack=%s",
			     keep_pack_list.items[i].string);
	strvec_push(&cmd.args, "--non-empty");
	if (!geometry.split_factor) {
		/*
		 * We need to grab all reachable objects, including those that
		 * are reachable from reflogs and the index.
		 *
		 * When repacking into a geometric progression of packs,
		 * however, we ask 'git pack-objects --stdin-packs', and it is
		 * not about packing objects based on reachability but about
		 * repacking all the objects in specified packs and loose ones
		 * (indeed, --stdin-packs is incompatible with these options).
		 */
		strvec_push(&cmd.args, "--all");
		strvec_push(&cmd.args, "--reflog");
		strvec_push(&cmd.args, "--indexed-objects");
	}
	if (repo_has_promisor_remote(repo))
		strvec_push(&cmd.args, "--exclude-promisor-objects");
	if (write_midx == REPACK_WRITE_MIDX_NONE) {
		if (write_bitmaps > 0)
			strvec_push(&cmd.args, "--write-bitmap-index");
		else if (write_bitmaps < 0)
			strvec_push(&cmd.args, "--write-bitmap-index-quiet");
	}
	if (use_delta_islands)
		strvec_push(&cmd.args, "--delta-islands");

	if (pack_everything & ALL_INTO_ONE) {
		repack_promisor_objects(repo, &po_args, &names, packtmp);

		if (existing_packs_has_non_kept(&existing) &&
		    delete_redundant &&
		    !(pack_everything & PACK_CRUFT)) {
			for_each_string_list_item(item, &names) {
				strvec_pushf(&cmd.args, "--keep-pack=%s-%s.pack",
					     packtmp_name, item->string);
			}
			if (unpack_unreachable) {
				strvec_pushf(&cmd.args,
					     "--unpack-unreachable=%s",
					     unpack_unreachable);
			} else if (pack_everything & LOOSEN_UNREACHABLE) {
				strvec_push(&cmd.args,
					    "--unpack-unreachable");
			} else if (keep_unreachable) {
				strvec_push(&cmd.args, "--keep-unreachable");
			}
		}

		if (keep_unreachable && delete_redundant &&
		    !(pack_everything & PACK_CRUFT))
			strvec_push(&cmd.args, "--pack-loose-unreachable");
	} else if (geometry.split_factor) {
		pack_geometry_repack_promisors(repo, &po_args, &geometry,
					       &names, packtmp);

		if (midx_must_contain_cruft)
			strvec_push(&cmd.args, "--stdin-packs");
		else
			strvec_push(&cmd.args, "--stdin-packs=follow");
		strvec_push(&cmd.args, "--unpacked");
	} else {
		strvec_push(&cmd.args, "--unpacked");
		strvec_push(&cmd.args, "--incremental");
	}

	if (po_args.filter_options.choice)
		strvec_pushf(&cmd.args, "--filter=%s",
			     expand_list_objects_filter_spec(&po_args.filter_options));
	else if (filter_to)
		die(_("option '%s' can only be used along with '%s'"), "--filter-to", "--filter");

	if (geometry.split_factor)
		cmd.in = -1;
	else
		cmd.no_stdin = 1;

	ret = start_command(&cmd);
	if (ret)
		goto cleanup;

	/*
	 * The main pack-objects is now running.  Once it has
	 * emitted both exclude files (which it does immediately
	 * after enumerating its inputs and well before it finishes
	 * writing its output), we can safely launch the aggregator
	 * to roll up anything else that lands in objects/ in the
	 * meantime.
	 *
	 * Mark our ends of the pack-objects pipes close-on-exec
	 * first: start_command() uses plain pipe() (not
	 * pipe2(O_CLOEXEC)), so without this the aggregator would
	 * inherit them and pack-objects would never see EOF on stdin
	 * (which matters for --geometric where we feed it via
	 * cmd.in).  Test for ">0" rather than ">=0": fd 0 is the
	 * CHILD_PROCESS_INIT default for cmd.in in the non-geometric
	 * (no_stdin) path, which is *not* a pipe we own.
	 */
	if (aggregate_opt > 0) {
		if (cmd.in > 0) {
			int flags = fcntl(cmd.in, F_GETFD);
			if (flags >= 0)
				fcntl(cmd.in, F_SETFD, flags | FD_CLOEXEC);
		}
		if (cmd.out > 0) {
			int flags = fcntl(cmd.out, F_GETFD);
			if (flags >= 0)
				fcntl(cmd.out, F_SETFD, flags | FD_CLOEXEC);
		}
		start_aggregate_sidecar(&sidecar, cmd.out);
	}

	if (geometry.split_factor) {
		FILE *in = xfdopen(cmd.in, "w");
		/*
		 * The resulting pack should contain all objects in packs that
		 * are going to be rolled up, but exclude objects in packs which
		 * are being left alone.
		 */
		for (i = 0; i < geometry.split; i++)
			fprintf(in, "%s\n", pack_basename(geometry.pack[i]));
		for (i = geometry.split; i < geometry.pack_nr; i++) {
			const char *basename = pack_basename(geometry.pack[i]);
			char marker = '^';

			if (!midx_must_contain_cruft &&
			    !string_list_has_string(&existing.midx_packs,
						    basename)) {
				/*
				 * Assume non-MIDX'd packs are not
				 * necessarily closed under
				 * reachability.
				 */
				marker = '!';
			}

			fprintf(in, "%c%s\n", marker, basename);
		}
		fclose(in);
	}

	{
		struct write_pack_opts opts = {
			.packdir = packdir,
			.destination = packdir,
			.packtmp = packtmp,
		};
		ret = finish_pack_objects_cmd(repo->hash_algo, &opts, &cmd,
					      &names);
		if (ret)
			goto cleanup;
	}

	if (!names.nr) {
		if (!po_args.quiet)
			printf_ln(_("Nothing new to pack."));
		/*
		 * If we didn't write any new packs, the non-cruft packs
		 * may refer to once-unreachable objects in the cruft
		 * pack(s).
		 *
		 * If there isn't already a MIDX, the one we write
		 * must include the cruft pack(s), in case the
		 * non-cruft pack(s) refer to once-cruft objects.
		 *
		 * If there is already a MIDX, we can punt here, since
		 * midx_has_unknown_packs() will make the decision for
		 * us.
		 */
		if (!get_multi_pack_index(existing.source))
			midx_must_contain_cruft = 1;
	}

	if (pack_everything & PACK_CRUFT) {
		struct write_pack_opts opts = {
			.po_args = &cruft_po_args,
			.destination = packtmp,
			.packtmp = packtmp,
			.packdir = packdir,
		};

		if (!cruft_po_args.window)
			cruft_po_args.window = xstrdup_or_null(po_args.window);
		if (!cruft_po_args.window_memory)
			cruft_po_args.window_memory = xstrdup_or_null(po_args.window_memory);
		if (!cruft_po_args.depth)
			cruft_po_args.depth = xstrdup_or_null(po_args.depth);
		if (!cruft_po_args.threads)
			cruft_po_args.threads = xstrdup_or_null(po_args.threads);
		if (!cruft_po_args.max_pack_size)
			cruft_po_args.max_pack_size = po_args.max_pack_size;

		cruft_po_args.local = po_args.local;
		cruft_po_args.quiet = po_args.quiet;
		cruft_po_args.delta_base_offset = po_args.delta_base_offset;
		cruft_po_args.pack_kept_objects = 0;

		ret = write_cruft_pack(&opts, cruft_expiration,
				       combine_cruft_below_size, &names,
				       &existing);
		if (ret)
			goto cleanup;

		if (delete_redundant && expire_to) {
			/*
			 * If `--expire-to` is given with `-d`, it's possible
			 * that we're about to prune some objects. With cruft
			 * packs, pruning is implicit: any objects from existing
			 * packs that weren't picked up by new packs are removed
			 * when their packs are deleted.
			 *
			 * Generate an additional cruft pack, with one twist:
			 * `names` now includes the name of the cruft pack
			 * written in the previous step. So the contents of
			 * _this_ cruft pack exclude everything contained in the
			 * existing cruft pack (that is, all of the unreachable
			 * objects which are no older than
			 * `--cruft-expiration`).
			 *
			 * To make this work, cruft_expiration must become NULL
			 * so that this cruft pack doesn't actually prune any
			 * objects. If it were non-NULL, this call would always
			 * generate an empty pack (since every object not in the
			 * cruft pack generated above will have an mtime older
			 * than the expiration).
			 *
			 * Pretend we don't have a `--combine-cruft-below-size`
			 * argument, since we're not selectively combining
			 * anything based on size to generate the limbo cruft
			 * pack, but rather removing all cruft packs from the
			 * main repository regardless of size.
			 */
			opts.destination = expire_to;
			ret = write_cruft_pack(&opts, NULL, 0ul, &names,
					       &existing);
			if (ret)
				goto cleanup;
		}
	}

	if (po_args.filter_options.choice) {
		struct write_pack_opts opts = {
			.po_args = &po_args,
			.destination = filter_to,
			.packdir = packdir,
			.packtmp = packtmp,
		};

		if (!opts.destination)
			opts.destination = packtmp;

		ret = write_filtered_pack(&opts, &existing, &names);
		if (ret)
			goto cleanup;
	}

	string_list_sort(&names);

	if (sidecar.started)
		install_aggregate_keep_markers(&sidecar, packdir, &names);

	odb_close(repo->objects);

	/*
	 * Ok we have prepared all new packfiles.
	 */
	for_each_string_list_item(item, &names)
		generated_pack_install(item->util, item->string, packdir,
				       packtmp);
	/* End of pack replacement. */

	if (delete_redundant && pack_everything & ALL_INTO_ONE) {
		if (write_midx == REPACK_WRITE_MIDX_INCREMENTAL)
			existing_packs_retain_midx_packs(&existing);
		existing_packs_mark_for_deletion(&existing, &names);
	}

	if (write_midx != REPACK_WRITE_MIDX_NONE) {
		struct repack_write_midx_opts opts = {
			.existing = &existing,
			.geometry = &geometry,
			.names = &names,
			.refs_snapshot = refs_snapshot ? get_tempfile_path(refs_snapshot) : NULL,
			.packdir = packdir,
			.show_progress = show_progress,
			.write_bitmaps = write_bitmaps > 0,
			.midx_must_contain_cruft = midx_must_contain_cruft,
			.midx_split_factor = config_ctx.midx_split_factor,
			.midx_new_layer_threshold = config_ctx.midx_new_layer_threshold,
			.mode = write_midx,
		};

		ret = repack_write_midx(&opts);
		if (ret)
			goto cleanup;
	}

	/*
	 * Cruft has been written, all new packs are installed (and
	 * ".keep"-marked), and the MIDX writer has read its explicit
	 * include list; the remaining work is small and fast.  Stop
	 * the pack-aggregate sidecar now, *before* odb_reprepare() or
	 * any delete/prune step that might otherwise race it.
	 */
	stop_aggregate_sidecar(&sidecar);

	odb_reprepare(repo->objects);

	if (delete_redundant) {
		int opts = 0;
		bool wrote_incremental_midx = write_midx == REPACK_WRITE_MIDX_INCREMENTAL;

		existing_packs_remove_redundant(&existing, packdir,
						wrote_incremental_midx);

		if (geometry.split_factor)
			pack_geometry_remove_redundant(&geometry, &names,
						       &existing, packdir,
						       wrote_incremental_midx);
		if (show_progress)
			opts |= PRUNE_PACKED_VERBOSE;
		prune_packed_objects(opts);

		if (!keep_unreachable &&
		    (!(pack_everything & LOOSEN_UNREACHABLE) ||
		     unpack_unreachable) &&
		    is_repository_shallow(repo))
			prune_shallow(PRUNE_QUICK);
	}

	if (run_update_server_info)
		update_server_info(repo, 0);

	if (git_env_bool(GIT_TEST_MULTI_PACK_INDEX, 0)) {
		unsigned flags = 0;
		if (git_env_bool(GIT_TEST_MULTI_PACK_INDEX_WRITE_INCREMENTAL, 0))
			flags |= MIDX_WRITE_INCREMENTAL;
		write_midx_file(existing.source, NULL, NULL, flags);
	}

cleanup:
	stop_aggregate_sidecar(&sidecar);
	string_list_clear(&keep_pack_list, 0);
	string_list_clear(&names, 1);
	existing_packs_release(&existing);
	pack_geometry_release(&geometry);
	pack_objects_args_release(&po_args);
	pack_objects_args_release(&cruft_po_args);

	return ret;
}
