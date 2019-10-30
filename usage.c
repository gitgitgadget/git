/*
 * GIT - The information manager from hell
 *
 * Copyright (C) Linus Torvalds, 2005
 */
#include "git-compat-util.h"
#include "cache.h"

static void replace_control_chars(char *str, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++) {
		if (iscntrl(str[i]) && str[i] != '\t' && str[i] != '\n')
			str[i] = '?';
	}
}

/* Atomically report (prefix + vsnprintf(err, params) + '\n') to stderr. */
void vreportf(const char *prefix, const char *err, va_list params)
{
	char msg[4096];
	int ret;
	size_t prefix_size, total_size;

	prefix_size = strlen(prefix);
	if (prefix_size >= sizeof(msg))
		BUG("vreportf: prefix is too long");

	memcpy(msg, prefix, prefix_size);

	ret = vsnprintf(msg + prefix_size, sizeof(msg) - prefix_size, err, params);
	if (ret < 0)
		BUG("your vsnprintf is broken (returned %d)", ret);

	total_size = strlen(msg);   /* vsnprintf() returns _desired_ size */
	msg[total_size++] = '\n';   /* it's ok to overwrite terminating NULL */

	replace_control_chars(msg, total_size);

	fflush(stderr);             /* flush FILE* before writing lowlevel fd */
	xwrite(2, msg, total_size); /* writing directly to fd is most atomic */
}

static NORETURN void usage_builtin(const char *err, va_list params)
{
	vreportf("usage: ", err, params);

	/*
	 * When we detect a usage error *before* the command dispatch in
	 * cmd_main(), we don't know what verb to report.  Force it to this
	 * to facilitate post-processing.
	 */
	trace2_cmd_name("_usage_");

	/*
	 * Currently, the (err, params) are usually just the static usage
	 * string which isn't very useful here.  Usually, the call site
	 * manually calls fprintf(stderr,...) with the actual detailed
	 * syntax error before calling usage().
	 *
	 * TODO It would be nice to update the call sites to pass both
	 * the static usage string and the detailed error message.
	 */

	exit(129);
}

static NORETURN void die_builtin(const char *err, va_list params)
{
	/*
	 * We call this trace2 function first and expect it to va_copy 'params'
	 * before using it (because an 'ap' can only be walked once).
	 */
	trace2_cmd_error_va(err, params);

	vreportf("fatal: ", err, params);

	exit(128);
}

static void error_builtin(const char *err, va_list params)
{
	/*
	 * We call this trace2 function first and expect it to va_copy 'params'
	 * before using it (because an 'ap' can only be walked once).
	 */
	trace2_cmd_error_va(err, params);

	vreportf("error: ", err, params);
}

static void warn_builtin(const char *warn, va_list params)
{
	vreportf("warning: ", warn, params);
}

static int die_is_recursing_builtin(void)
{
	static int dying;
	/*
	 * Just an arbitrary number X where "a < x < b" where "a" is
	 * "maximum number of pthreads we'll ever plausibly spawn" and
	 * "b" is "something less than Inf", since the point is to
	 * prevent infinite recursion.
	 */
	static const int recursion_limit = 1024;

	dying++;
	if (dying > recursion_limit) {
		return 1;
	} else if (dying == 2) {
		warning("die() called many times. Recursion error or racy threaded death!");
		return 0;
	} else {
		return 0;
	}
}

/* If we are in a dlopen()ed .so write to a global variable would segfault
 * (ugh), so keep things static. */
static NORETURN_PTR void (*usage_routine)(const char *err, va_list params) = usage_builtin;
static NORETURN_PTR void (*die_routine)(const char *err, va_list params) = die_builtin;
static void (*error_routine)(const char *err, va_list params) = error_builtin;
static void (*warn_routine)(const char *err, va_list params) = warn_builtin;
static int (*die_is_recursing)(void) = die_is_recursing_builtin;

void set_die_routine(NORETURN_PTR void (*routine)(const char *err, va_list params))
{
	die_routine = routine;
}

void set_error_routine(void (*routine)(const char *err, va_list params))
{
	error_routine = routine;
}

void (*get_error_routine(void))(const char *err, va_list params)
{
	return error_routine;
}

void set_warn_routine(void (*routine)(const char *warn, va_list params))
{
	warn_routine = routine;
}

void (*get_warn_routine(void))(const char *warn, va_list params)
{
	return warn_routine;
}

void set_die_is_recursing_routine(int (*routine)(void))
{
	die_is_recursing = routine;
}

void NORETURN usagef(const char *err, ...)
{
	va_list params;

	va_start(params, err);
	usage_routine(err, params);
	va_end(params);
}

void NORETURN usage(const char *err)
{
	usagef("%s", err);
}

void NORETURN die(const char *err, ...)
{
	va_list params;

	if (die_is_recursing()) {
		fputs("fatal: recursion detected in die handler\n", stderr);
		exit(128);
	}

	va_start(params, err);
	die_routine(err, params);
	va_end(params);
}

static const char *fmt_with_err(char *buf, int n, const char *fmt)
{
	char str_error[256], *err;
	int i, j;

	err = strerror(errno);
	for (i = j = 0; err[i] && j < sizeof(str_error) - 1; ) {
		if ((str_error[j++] = err[i++]) != '%')
			continue;
		if (j < sizeof(str_error) - 1) {
			str_error[j++] = '%';
		} else {
			/* No room to double the '%', so we overwrite it with
			 * '\0' below */
			j--;
			break;
		}
	}
	str_error[j] = 0;
	/* Truncation is acceptable here */
	snprintf(buf, n, "%s: %s", fmt, str_error);
	return buf;
}

void NORETURN die_errno(const char *fmt, ...)
{
	char buf[1024];
	va_list params;

	if (die_is_recursing()) {
		fputs("fatal: recursion detected in die_errno handler\n",
			stderr);
		exit(128);
	}

	va_start(params, fmt);
	die_routine(fmt_with_err(buf, sizeof(buf), fmt), params);
	va_end(params);
}

#undef error_errno
int error_errno(const char *fmt, ...)
{
	char buf[1024];
	va_list params;

	va_start(params, fmt);
	error_routine(fmt_with_err(buf, sizeof(buf), fmt), params);
	va_end(params);
	return -1;
}

#undef error
int error(const char *err, ...)
{
	va_list params;

	va_start(params, err);
	error_routine(err, params);
	va_end(params);
	return -1;
}

void warning_errno(const char *warn, ...)
{
	char buf[1024];
	va_list params;

	va_start(params, warn);
	warn_routine(fmt_with_err(buf, sizeof(buf), warn), params);
	va_end(params);
}

void warning(const char *warn, ...)
{
	va_list params;

	va_start(params, warn);
	warn_routine(warn, params);
	va_end(params);
}

/* Only set this, ever, from t/helper/, when verifying that bugs are caught. */
int BUG_exit_code;

static NORETURN void BUG_vfl(const char *file, int line, const char *fmt, va_list params)
{
	char prefix[256];

	/* truncation via snprintf is OK here */
	if (file)
		snprintf(prefix, sizeof(prefix), "BUG: %s:%d: ", file, line);
	else
		snprintf(prefix, sizeof(prefix), "BUG: ");

	vreportf(prefix, fmt, params);
	if (BUG_exit_code)
		exit(BUG_exit_code);
	abort();
}

#ifdef HAVE_VARIADIC_MACROS
NORETURN void BUG_fl(const char *file, int line, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	BUG_vfl(file, line, fmt, ap);
	va_end(ap);
}
#else
NORETURN void BUG(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	BUG_vfl(NULL, 0, fmt, ap);
	va_end(ap);
}
#endif

#ifdef SUPPRESS_ANNOTATED_LEAKS
void unleak_memory(const void *ptr, size_t len)
{
	static struct suppressed_leak_root {
		struct suppressed_leak_root *next;
		char data[FLEX_ARRAY];
	} *suppressed_leaks;
	struct suppressed_leak_root *root;

	FLEX_ALLOC_MEM(root, data, ptr, len);
	root->next = suppressed_leaks;
	suppressed_leaks = root;
}
#endif
