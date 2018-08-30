#include "cache.h"
#include "config.h"
#include "json-writer.h"
#include "quote.h"
#include "run-command.h"
#include "sigchain.h"
#include "thread-utils.h"
#include "version.h"

/*****************************************************************
 * TODO remove this section header
 *****************************************************************/

static struct strbuf tr2sid_buf = STRBUF_INIT;
static int tr2sid_nr_git_parents;

/*
 * Compute a "unique" session id (SID) for the current process.  All events
 * from this process will have this label.  If we were started by another
 * git instance, use our parent's SID as a prefix and count the number of
 * nested git processes.  (This lets us track parent/child relationships
 * even if there is an intermediate shell process.)
 */
static void tr2sid_compute(void)
{
	uint64_t us_now;
	const char *parent_sid;

	if (tr2sid_buf.len)
		return;

	parent_sid = getenv("SLOG_PARENT_SID");
	if (parent_sid && *parent_sid) {
		const char *p;
		for (p = parent_sid; *p; p++)
			if (*p == '/')
				tr2sid_nr_git_parents++;

		strbuf_addstr(&tr2sid_buf, parent_sid);
		strbuf_addch(&tr2sid_buf, '/');
		tr2sid_nr_git_parents++;
	}

	us_now = getnanotime() / 1000;
	strbuf_addf(&tr2sid_buf, "%"PRIuMAX"-%"PRIdMAX,
		    (uintmax_t)us_now, (intmax_t)getpid());

	setenv("SLOG_PARENT_SID", tr2sid_buf.buf, 1);
}

/*****************************************************************
 * TODO remove this section header
 *****************************************************************/

/*
 * Each thread (including the main thread) has a stack of nested regions.
 * This is used to indent messages and provide nested perf times.  The
 * limit here is for simplicity.  Note that the region stack is per-thread
 * and not per-trace_key.
 */
#define TR2_REGION_NESTING_INITIAL_SIZE (100)
#define TR2_INDENT (2)
#define TR2_INDENT_LENGTH(ctx) (((ctx)->nr_open_regions - 1) * TR2_INDENT)

#define TR2_MAX_THREAD_NAME (24)

struct tr2tls_thread_ctx {
	struct strbuf thread_name;
	uint64_t *array_us_start;
	int alloc;
	int nr_open_regions; /* plays role of "nr" in ALLOC_GROW */
	int thread_id;
};

static struct tr2tls_thread_ctx *tr2tls_thread_main;

static pthread_mutex_t tr2tls_mutex;
static pthread_key_t tr2tls_key;
static int tr2tls_initialized;

static struct strbuf tr2_dots = STRBUF_INIT;

static int tr2_next_child_id;
static int tr2_next_thread_id;

/*
 * Create TLS data for the current thread.  This gives us a place to
 * put per-thread data, such as thread start time, function nesting
 * and a per-thread label for our messages.
 *
 * We assume the first thread is "main".  Other threads are given
 * non-zero thread-ids to help distinguish messages from concurrent
 * threads.
 *
 * Truncate the thread name if necessary to help with column alignment
 * in printf-style messages.
 */
static struct tr2tls_thread_ctx *tr2tls_create_self(const char *thread_name)
{
	struct tr2tls_thread_ctx *ctx = xcalloc(1, sizeof(*ctx));

	/*
	 * Implicitly "tr2tls_push_self()" to capture the thread's start
	 * time in array_us_start[0].  For the main thread this gives us the
	 * application run time.
	 */
	ctx->alloc = TR2_REGION_NESTING_INITIAL_SIZE;
	ctx->array_us_start = (uint64_t*)xcalloc(ctx->alloc, sizeof(uint64_t));
	ctx->array_us_start[ctx->nr_open_regions++] = getnanotime() / 1000;

	pthread_mutex_lock(&tr2tls_mutex);
	ctx->thread_id = tr2_next_thread_id++;
	pthread_mutex_unlock(&tr2tls_mutex);

	strbuf_init(&ctx->thread_name, 0);
	if (ctx->thread_id)
		strbuf_addf(&ctx->thread_name, "th%02d:", ctx->thread_id);
	strbuf_addstr(&ctx->thread_name, thread_name);
	if (ctx->thread_name.len > TR2_MAX_THREAD_NAME)
		strbuf_setlen(&ctx->thread_name, TR2_MAX_THREAD_NAME);

	pthread_setspecific(tr2tls_key, ctx);

	return ctx;
}

static struct tr2tls_thread_ctx *tr2tls_get_self(void)
{
	struct tr2tls_thread_ctx * ctx = pthread_getspecific(tr2tls_key);

	/*
	 * If the thread-proc did not call trace2_thread_start(), we won't
	 * have any TLS data associated with the current thread.  Fix it
	 * here and silently continue.
	 */
	if (!ctx)
		ctx = tr2tls_create_self("unknown");

	return ctx;
}

static void tr2tls_unset_self(void)
{
	struct tr2tls_thread_ctx * ctx;

	ctx = tr2tls_get_self();

	pthread_setspecific(tr2tls_key, NULL);

	free(ctx->array_us_start);
	free(ctx);
}

/*
 * Begin a new nested region and remember the start time.
 */
static void tr2tls_push_self(void)
{
	struct tr2tls_thread_ctx *ctx = tr2tls_get_self();

	ALLOC_GROW(ctx->array_us_start, ctx->nr_open_regions + 1, ctx->alloc);
	ctx->array_us_start[ctx->nr_open_regions++] = getnanotime() / 1000;
}

/*
 * End the innermost nested region.
 */
static void tr2tls_pop_self(void)
{
	struct tr2tls_thread_ctx *ctx = tr2tls_get_self();

	if (!ctx->nr_open_regions)
		BUG("no open regions in thread '%s'", ctx->thread_name.buf);

	ctx->nr_open_regions--;
}

/*
 * Compute elapsed time in innermost nested region.
 */
static uint64_t tr2tls_region_elapsed_self(void)
{
	struct tr2tls_thread_ctx *ctx;
	uint64_t us_start;
	uint64_t us_now;

	ctx = tr2tls_get_self();
	if (!ctx->nr_open_regions)
		return 0;
	
	us_now = getnanotime() / 1000;
	us_start = ctx->array_us_start[ctx->nr_open_regions - 1];

	return us_now - us_start;
}

/*
 * Compute the elapsed run time since the main thread started.
 */
static uint64_t tr2tls_elapsed_main(void)
{
	uint64_t us_start;
	uint64_t us_now;
	uint64_t us_elapsed;

	if (!tr2tls_thread_main)
		return 0;

	us_now = getnanotime() / 1000;
	us_start = tr2tls_thread_main->array_us_start[0];
	us_elapsed = us_now - us_start;

	return us_elapsed;
}

static void tr2tls_init(void)
{
	pthread_key_create(&tr2tls_key, NULL);
	init_recursive_mutex(&tr2tls_mutex);

	tr2tls_thread_main = tr2tls_create_self("main");
	tr2tls_initialized = 1;
}

static void tr2tls_release(void)
{
	tr2tls_unset_self();
	tr2tls_thread_main = NULL;

	tr2tls_initialized = 0;

	pthread_mutex_destroy(&tr2tls_mutex);
	pthread_key_delete(tr2tls_key);
}

/*****************************************************************
 * TODO remove this section header
 *****************************************************************/

static struct trace_key tr2key_default = { "GIT_TR2", 0, 0, 0 };
static struct trace_key tr2key_perf    = { "GIT_TR2_PERFORMANCE", 0, 0, 0 };
static struct trace_key tr2key_event   = { "GIT_TR2_EVENT", 0, 0, 0 };

/*
 * Region depth limit for messages written to the event target.
 *
 * The "region_enter" and "region_leave" messages (especially recursive
 * messages such as those produced while diving the worktree or index)
 * are primarily intended for the performance target during debugging.
 *
 * Some of the outer-most messages, however, may be of interest to the
 * event target.  Set this environment variable to a larger integer for
 * more detail in the event target.
 */
#define GIT_TR2_EVENT_DEPTH "GIT_TR2_EVENT_DEPTH"
static int tr2env_event_depth_wanted = 2;

/*
 * Set this environment variable to true to omit the "<time> <file>:<line>"
 * fields from each line written to the default and performance targets.
 *
 * Unit tests may want to use this to help with testing.
 */
#define GIT_TR2_BARE "GIT_TR2_BARE"
static int tr2env_bare_wanted;

static void tr2key_trace_disable(struct trace_key *key)
{
	if (key->need_close)
		close(key->fd);
	key->fd = 0;
	key->initialized = 1;
	key->need_close = 0;
}

static int tr2key_get_trace_fd(struct trace_key *key)
{
	const char *trace;

	/* don't open twice */
	if (key->initialized)
		return key->fd;

	trace = getenv(key->key);

	if (!trace || !strcmp(trace, "") ||
	    !strcmp(trace, "0") || !strcasecmp(trace, "false"))
		key->fd = 0;
	else if (!strcmp(trace, "1") || !strcasecmp(trace, "true"))
		key->fd = STDERR_FILENO;
	else if (strlen(trace) == 1 && isdigit(*trace))
		key->fd = atoi(trace);
	else if (is_absolute_path(trace)) {
		int fd = open(trace, O_WRONLY | O_APPEND | O_CREAT, 0666);
		if (fd == -1) {
			warning("could not open '%s' for tracing: %s",
				trace, strerror(errno));
			tr2key_trace_disable(key);
		} else {
			key->fd = fd;
			key->need_close = 1;
		}
	} else {
		warning("unknown trace value for '%s': %s\n"
			"         If you want to trace into a file, then please set %s\n"
			"         to an absolute pathname (starting with /)",
			key->key, trace, key->key);
		tr2key_trace_disable(key);
	}

	key->initialized = 1;
	return key->fd;
}

static int tr2key_trace_want(struct trace_key *key)
{
	return !!tr2key_get_trace_fd(key);
}

/*
 * Force (rather than lazily) initialize any of the requested
 * primary/builtin trace targets at startup (and before we've
 * seen an actual trace event call) so we can see if we need
 * to setup the TR2 and TLS machinery.
 *
 * Other ad hoc trace targets can just use compatibility mode
 * and and printf-based messages.
 */
static int tr2key_want_builtins(void)
{
	int sum = 0;
	int v;
	char *p;

	sum += tr2key_trace_want(&tr2key_default);
	sum += tr2key_trace_want(&tr2key_perf);
	sum += tr2key_trace_want(&tr2key_event);

	p = getenv(GIT_TR2_EVENT_DEPTH);
	if (p && ((v = atoi(p)) > 0))
		tr2env_event_depth_wanted = v;

	p = getenv(GIT_TR2_BARE);
	if (p && *p && ((v = git_parse_maybe_bool(p)) != -1))
		tr2env_bare_wanted = v;

	return sum;
}

static void tr2key_disable_builtins(void)
{
	tr2key_trace_disable(&tr2key_default);
	tr2key_trace_disable(&tr2key_perf);
	tr2key_trace_disable(&tr2key_event);
}

/*****************************************************************
 * TODO remove this section header
 *****************************************************************/

/*
 * A simple wrapper around a fixed buffer to avoid C syntax
 * quirks and the need to pass around an additional size_t
 * argument.
 */
struct tr2time_buf {
	char buf[24];
};

static void tr2time_current_time(struct tr2time_buf *tb)
{
	struct timeval tv;
	struct tm tm;
	time_t secs;

	gettimeofday(&tv, NULL);
	secs = tv.tv_sec;
	localtime_r(&secs, &tm);

	xsnprintf(tb->buf, sizeof(tb->buf), "%02d:%02d:%02d.%06ld ",
		  tm.tm_hour, tm.tm_min, tm.tm_sec, (long)tv.tv_usec);
}

/*****************************************************************
 * TODO remove this section header
 *****************************************************************/

#define TR2FMT_MAX_EVENT_NAME (12)

/*
 * Format trace line prefix in human-readable classic format for
 * the default target:
 *     "<time> <file>:<line> <bar> <thread_name> <bar> <event_name> <bar> "
 */
static void tr2fmt_prepare_default(
	const char *event_name,
	struct tr2tls_thread_ctx *ctx,
	const char *file, int line,
	struct strbuf *buf)
{
	strbuf_setlen(buf,0);

	if (!tr2env_bare_wanted) {
		struct tr2time_buf tb_now;

		tr2time_current_time(&tb_now);
		strbuf_addstr(buf, tb_now.buf);

		if (file && *file)
			strbuf_addf(buf, "%s:%d ", file, line);
		while (buf->len < 40)
			strbuf_addch(buf, ' ');

		strbuf_addstr(buf, "| ");
	}

	strbuf_addf(buf, "%-*s | %-*s | ",
		    TR2_MAX_THREAD_NAME, ctx->thread_name.buf,
		    TR2FMT_MAX_EVENT_NAME, event_name);
}

/*
 * Format trace line prefix in human-readable classic format for
 * the performance target:
 *     "[<time>] [<file>:<line>] [<thread_name>] [<event_name>] <bar> <indent>"
 */
static void tr2fmt_prepare_perf(
	const char *event_name,
	struct tr2tls_thread_ctx *ctx,
	const char *file, int line,
	uint64_t *p_us_elapsed,
	struct strbuf *buf)
{
	strbuf_setlen(buf,0);

	if (!tr2env_bare_wanted) {

		struct tr2time_buf tb_now;

		tr2time_current_time(&tb_now);
		strbuf_addstr(buf, tb_now.buf);

		if (file && *file)
			strbuf_addf(buf, "%s:%d ", file, line);
		while (buf->len < 40)
			strbuf_addch(buf, ' ');

		strbuf_addstr(buf, "| ");
	}

	strbuf_addf(buf, "%-*s | %-*s | ",
		    TR2_MAX_THREAD_NAME, ctx->thread_name.buf,
		    TR2FMT_MAX_EVENT_NAME, event_name);

	if (p_us_elapsed)
		strbuf_addf(buf, "%9.6f | ",
			    ((double)(*p_us_elapsed)) / 1000000.0);
	else
		strbuf_addf(buf, "%9s | ", " ");
		
	if (ctx->nr_open_regions > 0) {
		int len_indent = TR2_INDENT_LENGTH(ctx);
		while (len_indent > tr2_dots.len) {
			strbuf_addf(buf, "%s", tr2_dots.buf);
			len_indent -= tr2_dots.len;
		}
		strbuf_addf(buf, "%.*s", len_indent, tr2_dots.buf);
	}
}

/*
 * Append common key-value pairs to the currently open JSON object.
 *     "event:"<event_name>"
 *      "sid":"<sid>"
 *   "thread":"<thread_name>"
 *     "time":"<time>"
 *     "file":"<filename>"
 *     "line":<line_number>
 */
static void tr2fmt_prepare_json_trace_line(
	const char *event_name,
	const char *file, int line,
	struct json_writer *jw)
{
	struct tr2tls_thread_ctx *ctx = tr2tls_get_self();
	struct tr2time_buf tb_now;

	jw_object_string(jw, "event", event_name);
	jw_object_string(jw, "sid", tr2sid_buf.buf);
	jw_object_string(jw, "thread", ctx->thread_name.buf);

	tr2time_current_time(&tb_now);
	jw_object_string(jw, "time", tb_now.buf);

	if (file && *file) {
		jw_object_string(jw, "file", file);
		jw_object_intmax(jw, "line", line);
	}
}

/*****************************************************************
 * TODO remove this section header
 *****************************************************************/

static void tr2io_write_line(struct trace_key *key, struct strbuf *buf_line)
{
	strbuf_complete_line(buf_line); /* ensure final NL on buffer */

	// TODO we don't really want short-writes to try again when
	// TODO we are in append mode...

	if (write_in_full(tr2key_get_trace_fd(key),
			  buf_line->buf, buf_line->len) < 0) {
		warning("unable to write trace for '%s': %s",
			key->key, strerror(errno));
		tr2key_trace_disable(key);
	}
}

static void tr2io_write_default_fl(const char *file, int line,
				   const char *event_name,
				   const struct strbuf *buf_payload)
{
	struct tr2tls_thread_ctx *ctx = tr2tls_get_self();
	struct strbuf buf_line = STRBUF_INIT;

	tr2fmt_prepare_default(event_name, ctx, file, line, &buf_line);

	strbuf_addbuf(&buf_line, buf_payload);

	tr2io_write_line(&tr2key_default, &buf_line);

	strbuf_release(&buf_line);
}

static void tr2io_write_perf_fl(const char *file, int line,
				const char *event_name,
				uint64_t *p_us_elapsed,
				const struct strbuf *buf_payload)
{
	struct tr2tls_thread_ctx *ctx = tr2tls_get_self();
	struct strbuf buf_line = STRBUF_INIT;

	tr2fmt_prepare_perf(event_name, ctx, file, line,
			    p_us_elapsed, &buf_line);

	strbuf_addbuf(&buf_line, buf_payload);

	tr2io_write_line(&tr2key_perf, &buf_line);

	strbuf_release(&buf_line);
}

/*****************************************************************
 * TODO remove this section header
 *****************************************************************/

/*
 * Write 'version' event message to the builtin targets.
 */
static void tr2evt_version_fl(const char *file, int line)
{
	const char *event_name = "version";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "%s", git_version_string);

		tr2io_write_default_fl(file, line, event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "%s", git_version_string);

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_string(&jw, "git", git_version_string);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

/*
 * Write 'start' event message to the builtin targets.
 */
static void tr2evt_start_fl(const char *file, int line,
			    const char **argv)
{
	const char *event_name = "start";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;

		sq_quote_argv_pretty(&buf_payload, argv);

		tr2io_write_default_fl(file, line, event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		sq_quote_argv_pretty(&buf_payload, argv);

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_inline_begin_array(&jw, "argv");
		jw_array_argv(&jw, argv);
		jw_end(&jw);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

/*
 * Write 'exit' event message to the builtin targets.
 */
static void tr2evt_exit_fl(const char *file, int line,
			   int code, uint64_t us_elapsed)
{
	const char *event_name = "exit";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;
		double elapsed = (double)us_elapsed / 1000000.0;

		strbuf_addf(&buf_payload, "elapsed:%.6f code:%d",
			    elapsed, code);

		tr2io_write_default_fl(file, line, event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "code:%d", code);

		tr2io_write_perf_fl(file, line, event_name,
				    &us_elapsed, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;
		double elapsed = (double)us_elapsed / 1000000.0;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_double(&jw, "elapsed", 6, elapsed);
		jw_object_intmax(&jw, "code", code);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

/*
 * Write 'signal' event message to the builtin targets.
 */
static void tr2evt_signal(int signo, uint64_t us_elapsed)
{
	const char *event_name = "signal";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;
		double elapsed = (double)us_elapsed / 1000000.0;

		strbuf_addf(&buf_payload, "elapsed:%.6f signo:%d",
			    elapsed, signo);

		tr2io_write_default_fl(__FILE__, __LINE__,
				       event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "signo:%d", signo);

		tr2io_write_perf_fl(__FILE__, __LINE__,
				    event_name, &us_elapsed, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;
		double elapsed = (double)us_elapsed / 1000000.0;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, __FILE__, __LINE__, &jw);
		jw_object_double(&jw, "elapsed", 6, elapsed);
		jw_object_intmax(&jw, "signo", signo);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

/*
 * Write 'atexit' event message to the builtin targets.
 */
static void tr2evt_atexit(int code, uint64_t us_elapsed)
{
	const char *event_name = "atexit";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;
		double elapsed = (double)us_elapsed / 1000000.0;

		strbuf_addf(&buf_payload, "elapsed:%.6f code:%d",
			    elapsed, code);

		tr2io_write_default_fl(__FILE__, __LINE__,
				       event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "code:%d", code);

		tr2io_write_perf_fl(__FILE__, __LINE__,
				    event_name, &us_elapsed, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;
		double elapsed = (double)us_elapsed / 1000000.0;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, __FILE__, __LINE__, &jw);
		jw_object_double(&jw, "elapsed", 6, elapsed);
		jw_object_intmax(&jw, "code", code);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

static void tr2evt_error_va_fl(const char *file, int line,
			       const char *fmt, va_list ap)
{
	const char *event_name = "error";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_vaddf(&buf_payload, fmt, ap);

		tr2io_write_default_fl(file, line, event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_vaddf(&buf_payload, fmt, ap);

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;
		struct strbuf buf_msg = STRBUF_INIT;

		strbuf_vaddf(&buf_msg, fmt, ap);

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_string(&jw, "msg", buf_msg.buf);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
		strbuf_release(&buf_msg);
	}
}

static void tr2evt_command_fl(const char *file, int line,
			      const char *command_name)
{
	const char *event_name = "command";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addstr(&buf_payload, command_name);

		tr2io_write_default_fl(file, line, event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addstr(&buf_payload, command_name);

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_string(&jw, "name", command_name);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

static void tr2evt_worktree_fl(const char *file, int line,
			       const char *path)
{
	const char *event_name = "worktree";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addstr(&buf_payload, path);

		tr2io_write_default_fl(file, line, event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addstr(&buf_payload, path);

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_string(&jw, "path", path);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

static void tr2evt_alias_fl(const char *file, int line,
			    const char *alias, const char **argv)
{
	const char *event_name = "alias";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "alias:%s argv:", alias);
		sq_quote_argv_pretty(&buf_payload, argv);

		tr2io_write_default_fl(file, line, event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "alias:%s argv:", alias);
		sq_quote_argv_pretty(&buf_payload, argv);

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_string(&jw, "alias", alias);
		jw_object_inline_begin_array(&jw, "argv");
		jw_array_argv(&jw, argv);
		jw_end(&jw);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

static void tr2evt_param_fl(const char *file, int line,
			    const char *param, const char *value)
{
	const char *event_name = "param";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "%s:%s", param, value);

		tr2io_write_default_fl(file, line, event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "%s:%s", param, value);

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_string(&jw, "param", param);
		jw_object_string(&jw, "value", value);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

/*
 * Write a common 'child_start' message to the builtin targets.
 */
static void tr2evt_child_start_fl(const char *file, int line,
				  int child_id, const char *child_class,
				  const char **argv)
{
	const char *event_name = "child_start";

	if (!child_class)
		child_class = "?";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "[ch%d] class:%s argv:",
			    child_id, child_class);
		sq_quote_argv_pretty(&buf_payload, argv);

		tr2io_write_default_fl(file, line, event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "[ch%d] class:%s argv:",
			    child_id, child_class);
		sq_quote_argv_pretty(&buf_payload, argv);

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_intmax(&jw, "child_id", child_id);
		jw_object_string(&jw, "child_class", child_class);
		jw_object_inline_begin_array(&jw, "argv");
		jw_array_argv(&jw, argv);
		jw_end(&jw);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

static void tr2evt_child_exit_fl(const char *file, int line,
				 int child_id,
				 int child_exit_code,
				 uint64_t us_elapsed)
{
	const char *event_name = "child_exit";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;
		double elapsed = (double)us_elapsed / 1000000.0;

		strbuf_addf(&buf_payload, "[ch%d] code:%d elapsed:%.6f",
			    child_id, child_exit_code, elapsed);
		
		tr2io_write_default_fl(file, line, event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "[ch%d] code:%d",
			    child_id, child_exit_code);
		
		tr2io_write_perf_fl(file, line,
				    event_name, &us_elapsed, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;
		double elapsed = (double)us_elapsed / 1000000.0;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_intmax(&jw, "child_id", child_id);
		jw_object_intmax(&jw, "code", child_exit_code);
		jw_object_double(&jw, "elapsed", 6, elapsed);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);

		jw_release(&jw);
	}
}

static void tr2evt_thread_start_fl(const char *file, int line)
{
	const char *event_name = "thread_start";

	/* Default target does not care about threads. */

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

static void tr2evt_thread_exit_fl(const char *file, int line,
				  uint64_t us_elapsed)
{
	const char *event_name = "thread_exit";

	/* Default target does not care about threads. */

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		tr2io_write_perf_fl(file, line,
				    event_name, &us_elapsed, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;
		double elapsed = (double)us_elapsed / 1000000.0;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_double(&jw, "elapsed", 6, elapsed);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

static void tr2evt_region_enter_va_fl(const char *file, int line,
				      const char *fmt, va_list ap)
{
	const char *event_name = "region_enter";

	/* Default target does not care about nesting. */

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_vaddf(&buf_payload, fmt, ap);

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct tr2tls_thread_ctx *ctx = tr2tls_get_self();
		if (ctx->nr_open_regions <= tr2env_event_depth_wanted) {
			struct json_writer jw = JSON_WRITER_INIT;
			struct strbuf buf = STRBUF_INIT;

			strbuf_vaddf(&buf, fmt, ap);

			jw_object_begin(&jw, 0);
			tr2fmt_prepare_json_trace_line(
				event_name, file, line, &jw);
			jw_object_intmax(&jw, "depth", ctx->nr_open_regions);
			jw_object_string(&jw, "msg", buf.buf);
			jw_end(&jw);

			tr2io_write_line(&tr2key_event, &jw.json);
			jw_release(&jw);
			strbuf_release(&buf);
		}
	}
}

static void tr2evt_region_leave_va_fl(const char *file, int line,
				      uint64_t us_elapsed,
				      const char *fmt, va_list ap)
{
	const char *event_name = "region_leave";

	/* Default target does not care about nesting. */

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_vaddf(&buf_payload, fmt, ap);

		tr2io_write_perf_fl(file, line,
				    event_name, &us_elapsed, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct tr2tls_thread_ctx *ctx = tr2tls_get_self();
		if (ctx->nr_open_regions <= tr2env_event_depth_wanted) {
			struct json_writer jw = JSON_WRITER_INIT;
			struct strbuf buf = STRBUF_INIT;
			double elapsed = (double)us_elapsed / 1000000.0;

			strbuf_vaddf(&buf, fmt, ap);

			jw_object_begin(&jw, 0);
			tr2fmt_prepare_json_trace_line(
				event_name, file, line, &jw);
			jw_object_double(&jw, "elapsed", 6, elapsed);
			jw_object_intmax(&jw, "depth", ctx->nr_open_regions);
			jw_object_string(&jw, "msg", buf.buf);
			jw_end(&jw);

			tr2io_write_line(&tr2key_event, &jw.json);
			jw_release(&jw);
			strbuf_release(&buf);
		}
	}
}

static void tr2evt_printf_va_fl(const char *file, int line,
				const char *fmt, va_list ap)
{
	const char *event_name = "printf";

	if (tr2key_trace_want(&tr2key_default)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_vaddf(&buf_payload, fmt, ap);

		tr2io_write_default_fl(file, line, event_name, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_vaddf(&buf_payload, fmt, ap);

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	/* Event target does not care about printf messages. */
}

static void tr2evt_data_fl(const char *file, int line,
			   const char *category,
			   const char *key,
			   const char *value)
{
	const char *event_name = "data";

	/* Default target does not care about data messages. */

	if (tr2key_trace_want(&tr2key_perf)) {
		struct strbuf buf_payload = STRBUF_INIT;

		strbuf_addf(&buf_payload, "category:%s key:%s value:%s",
			    category, key, value);

		tr2io_write_perf_fl(file, line,
				    event_name, NULL, &buf_payload);
		strbuf_release(&buf_payload);
	}

	if (tr2key_trace_want(&tr2key_event)) {
		struct json_writer jw = JSON_WRITER_INIT;

		jw_object_begin(&jw, 0);
		tr2fmt_prepare_json_trace_line(
			event_name, file, line, &jw);
		jw_object_string(&jw, "category", category);
		jw_object_string(&jw, "key", key);
		jw_object_string(&jw, "value", value);
		jw_end(&jw);

		tr2io_write_line(&tr2key_event, &jw.json);
		jw_release(&jw);
	}
}

/*****************************************************************
 * TODO remove this section header
 *****************************************************************/

static int tr2main_exit_code;

/*
 * Our atexit routine should run after everything has finished.
 * The system should call it on the main thread.
 *
 * Note that events generated here might not actually appear if
 * we are writing to fd 1 or 2 and our atexit routine runs after
 * the pager's atexit routine (since it closes them to shutdown
 * the pipes).
 */
static void tr2main_atexit_handler(void)
{
	assert(tr2tls_get_self() == tr2tls_thread_main);

	/*
	 * Pop any extra open regions on the main thread and discard.
	 * Normally, we should only have the region[0] that was pushed
	 * in trace2_start(), but there may be more if someone calls
	 * die() for example.
	 */
	while (tr2tls_thread_main->nr_open_regions > 1)
		tr2tls_pop_self();

	/*
	 * Issue single 'atexit' event with the elapsed time since
	 * the main thread was started.
	 */
	tr2evt_atexit(tr2main_exit_code, tr2tls_elapsed_main());
		
	tr2key_disable_builtins();

	tr2tls_release();

	strbuf_release(&tr2sid_buf);
	strbuf_release(&tr2_dots);
}

static void tr2main_signal_handler(int signo)
{
	tr2evt_signal(signo, tr2tls_elapsed_main());

	sigchain_pop(signo);
	raise(signo);
}

/*****************************************************************
 * TODO remove this section header
 *****************************************************************/

void trace2_start_fl(const char *file, int line, const char **argv)
{
	if (!tr2key_want_builtins())
		return;

	tr2sid_compute();
	strbuf_addchars(&tr2_dots, '.',
			TR2_REGION_NESTING_INITIAL_SIZE * TR2_INDENT);

	atexit(tr2main_atexit_handler);
	sigchain_push(SIGPIPE, tr2main_signal_handler);

	tr2tls_init();

	tr2evt_version_fl(file, line);
	tr2evt_start_fl(file, line, argv);
}

int trace2_exit_fl(const char *file, int line, int code)
{
	code &= 0xff;

	if (!tr2tls_initialized)
		return code;

	tr2main_exit_code = code;

	tr2evt_exit_fl(file, line, code, tr2tls_elapsed_main());

	return code;
}

void trace2_error_va_fl(const char *file, int line,
			const char *fmt, va_list ap)
{
	if (!tr2tls_initialized)
		return;

	tr2evt_error_va_fl(file, line, fmt, ap);
}

void trace2_command_fl(const char *file, int line,
		       const char *command_name)
{
	if (!tr2tls_initialized)
		return;

	tr2evt_command_fl(file, line, command_name);
}

void trace2_worktree_fl(const char *file, int line,
			const char *path)
{
	if (!tr2tls_initialized)
		return;

	tr2evt_worktree_fl(file, line, path);
}

void trace2_alias_fl(const char *file, int line,
		     const char *alias, const char **argv)
{
	if (!tr2tls_initialized)
		return;

	tr2evt_alias_fl(file, line, alias, argv);
}

void trace2_param_fl(const char *file, int line,
		     const char *param, const char *value)
{
	if (!tr2tls_initialized)
		return;

	tr2evt_param_fl(file, line, param, value);
}

void trace2_child_start_fl(const char *file, int line,
			   struct child_process *cmd)
{
	if (!tr2tls_initialized)
		return;

	pthread_mutex_lock(&tr2tls_mutex);
	cmd->trace2_child_id = tr2_next_child_id++;
	pthread_mutex_unlock(&tr2tls_mutex);

	cmd->trace2_child_us_start = getnanotime() / 1000;

	tr2evt_child_start_fl(file, line,
			      cmd->trace2_child_id,
			      cmd->trace2_child_class,
			      cmd->argv);
}

void trace2_child_exit_fl(const char *file, int line,
			  struct child_process *cmd,
			  int child_exit_code)
{
	uint64_t us_elapsed;

	if (!tr2tls_initialized)
		return;

	if (cmd->trace2_child_us_start)
		us_elapsed = (getnanotime() / 1000) - cmd->trace2_child_us_start;
	else
		us_elapsed = 0;
	
	tr2evt_child_exit_fl(file, line,
			     cmd->trace2_child_id,
			     child_exit_code,
			     us_elapsed);
}

void trace2_thread_start_fl(const char *file, int line,
			    const char *thread_name)
{
	if (!tr2tls_initialized)
		return;

	tr2tls_create_self(thread_name);

	tr2evt_thread_start_fl(file, line);
}

void trace2_thread_exit_fl(const char *file, int line)
{
	struct tr2tls_thread_ctx *ctx;

	if (!tr2tls_initialized)
		return;

	/*
	 * Pop any extra open regions on the current thread and discard.
	 * Normally, we should only have the region[0] that was pushed
	 * in trace2_thread_start() if the thread exits normally.
	 */
	ctx = tr2tls_get_self();
	while (ctx->nr_open_regions > 1)
		tr2tls_pop_self();

	tr2evt_thread_exit_fl(file, line, tr2tls_region_elapsed_self());

	tr2tls_unset_self();
}

void trace2_region_enter_va_fl(const char *file, int line,
			       const char *fmt, va_list ap)
{
	if (!tr2tls_initialized)
		return;

	if (fmt && *fmt)
		tr2evt_region_enter_va_fl(file, line, fmt, ap);

	tr2tls_push_self();
}

void trace2_region_enter_fl(const char *file, int line,
			    const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	trace2_region_enter_va_fl(file, line, fmt, ap);
	va_end(ap);
}

#ifndef HAVE_VARIADIC_MACROS
void trace2_region_enter(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	trace2_region_enter_va_fl(NULL, 0, fmt, ap);
	va_end(ap);
}
#endif

void trace2_region_leave_va_fl(const char *file, int line,
			       const char *fmt, va_list ap)
{
	uint64_t us_elapsed;

	if (!tr2tls_initialized)
		return;

	/*
	 * Get the elapsed time in the current region before we
	 * pop it off the stack.  Pop the stack.  And then print
	 * the perf message at the new (shallower) level so that
	 * it lines up with the corresponding push/enter.
	 */
	us_elapsed = tr2tls_region_elapsed_self();
	
	tr2tls_pop_self();

	if (fmt && *fmt)
		tr2evt_region_leave_va_fl(file, line, us_elapsed, fmt, ap);
}

void trace2_region_leave_fl(const char *file, int line,
			    const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	trace2_region_leave_va_fl(file, line, fmt, ap);
	va_end(ap);
}

#ifndef HAVE_VARIADIC_MACROS
void trace2_region_leave(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	trace2_region_leave_va_fl(NULL, 0, fmt, ap);
	va_end(ap);
}
#endif

void trace2_printf_va_fl(const char *file, int line,
			 const char *fmt, va_list ap)
{
	if (!tr2tls_initialized)
		return;

	tr2evt_printf_va_fl(file, line, fmt, ap);
}

void trace2_printf_fl(const char *file, int line,
		      const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	trace2_printf_va_fl(file, line, fmt, ap);
	va_end(ap);
}

#ifndef HAVE_VARIADIC_MACROS
void trace2_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	trace2_printf_va_fl(NULL, 0, fmt, ap);
	va_end(ap);
}
#endif

void trace2_data_intmax_fl(const char *file, int line,
			   const char *category,
			   const char *key,
			   intmax_t value)
{
	struct strbuf buf_string = STRBUF_INIT;

	if (!tr2tls_initialized)
		return;

	strbuf_addf(&buf_string, "%"PRIdMAX, value);
	tr2evt_data_fl(file, line, category, key, buf_string.buf);
	strbuf_release(&buf_string);
}

void trace2_data_string_fl(const char *file, int line,
			   const char *category,
			   const char *key,
			   const char *value)
{
	if (!tr2tls_initialized)
		return;

	tr2evt_data_fl(file, line, category, key, value);
}
