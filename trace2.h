#ifndef TRACE2_H
#define TRACE2_H

/*
 * Begin TRACE2 tracing (if any of the builtin TRACE2 targets are
 * enabled in the environment).
 *
 * Emit a 'start' event.
 */
void trace2_start_fl(const char *file, int line,
		     const char **argv);

#define trace2_start(argv) trace2_start_fl(__FILE__, __LINE__, argv)

/*
 * Emit an 'exit' event.
 *
 * Write the exit-code that will be passed to exit() or returned
 * from main().  Use this prior to actually calling exit().
 * See "#define exit()" in git-compat-util.h
 */
int trace2_exit_fl(const char *file, int line, int code);

#define trace2_exit(code) (trace2_exit_fl(__FILE__, __LINE__, (code)))

/*
 * Emit an 'error' event.
 *
 * Write an error message to the TRACE2 targets.
 */
void trace2_error_va_fl(const char *file, int line,
			const char *fmt, va_list ap);

#define trace2_error_va(fmt, ap) \
	trace2_error_va_fl(__FILE__, __LINE__, (fmt), (ap))

void trace2_command_fl(const char *file, int line,
		       const char *command_name);

#define trace2_command(n) \
	trace2_command_fl(__FILE__, __LINE__, (n))

/*
 * Emit a 'worktree' event giving the absolute path of the worktree.
 */
void trace2_worktree_fl(const char *file, int line,
			const char *path);

#define trace2_worktree(path) \
	trace2_worktree_fl(__FILE__, __LINE__, path)

/*
 * Emit an 'alias' expansion event.
 */
void trace2_alias_fl(const char *file, int line,
		     const char *alias, const char **argv);

#define trace2_alias(alias, argv) \
	trace2_alias_fl(__FILE__, __LINE__, (alias), (argv))

/*
 * Emit a 'param' event.
 *
 * Write a (key, value) pair describing some aspect of the run
 * such as an important configuration setting.
 */ 
void trace2_param_fl(const char *file, int line,
		     const char *param, const char *value);

#define trace2_param(param, value) \
	trace2_param_fl(__FILE__, __LINE__, (param), (value))

struct child_process;

/*
 * Emit a 'child_start' event prior to spawning a child process.
 *
 * Before calling optionally set cmd.trace2_child_class to a string
 * describing the type of the child process.  For example, "editor"
 * or "pager".
 */
void trace2_child_start_fl(const char *file, int line,
			   struct child_process *cmd);

#define trace2_child_start(cmd) \
	trace2_child_start_fl(__FILE__, __LINE__, (cmd))

/*
 * Emit a 'child_exit' event after the child process completes. 
 */
void trace2_child_exit_fl(const char *file, int line,
			  struct child_process *cmd,
			  int child_exit_code);

#define trace2_child_exit(cmd, code) \
	trace2_child_exit_fl(__FILE__, __LINE__, (cmd), (code))

/*
 * Emit a 'thread_start' event.  This must be called from inside the
 * thread-proc to set up TLS data for the thread.
 *
 * Thread names should be descriptive, like "preload_index".
 * Thread names will be decorated with an instance number automatically.
 */
void trace2_thread_start_fl(const char *file, int line,
			    const char *thread_name);

#define trace2_thread_start(thread_name) \
	trace2_thread_start_fl(__FILE__, __LINE__, (thread_name))

/*
 * Emit a 'thead_exit' event.  This must be called from inside the
 * thread-proc to report thread-specific data and cleanup TLS data
 * for the thread.
 */
void trace2_thread_exit_fl(const char *file, int line);

#define trace2_thread_exit() trace2_thread_exit_fl(__FILE__, __LINE__)

/*
 * Emit a 'printf' event.
 *
 * Write an arbitrary formatted message to the TRACE2 targets.  These
 * text messages should be considered as human-readable strings without
 * any formatting guidelines.  Post-processors may choose to ignore
 * them.
 */
void trace2_printf_va_fl(const char *file, int line,
			 const char *fmt, va_list ap);

#define trace2_printf_va(fmt, ap) \
	trace2_printf_va_fl(__FILE__, __LINE__, (fmt), (ap))

void trace2_printf_fl(const char *file, int line, const char *fmt, ...);

#ifdef HAVE_VARIADIC_MACROS
#define trace2_printf(...) \
	trace2_printf_fl(__FILE__, __LINE__, __VA_ARGS__)
#else
__attribute__((format (printf, 1, 2)))
void trace2_printf(const char *fmt, ...);
#endif

/*
 * Emit a 'region_enter' event.
 *
 * Enter a new nesting level on the current thread and remember the
 * current time.  This controls the indenting of subsequent thread
 * events.
 */
void trace2_region_enter_va_fl(const char *file, int line,
			       const char *fmt, va_list ap);

#define trace2_region_enter_va(fmt, ap) \
	trace2_region_enter_va_fl(__FILE__, __LINE__, (fmt), (ap))

void trace2_region_enter_fl(const char *file, int line, const char *fmt, ...);

#ifdef HAVE_VARIADIC_MACROS
#define trace2_region_enter(...) \
	trace2_region_enter_fl(__FILE__, __LINE__, __VA_ARGS__)
#else
__attribute__((format (printf, 1, 2)))
void trace2_region_enter(const char *fmt, ...);
#endif

/*
 * Emit a 'region_leave' event.
 *
 * Leave current nesting level and report the elapsed time spent
 * in this nesting level.
 */
void trace2_region_leave_va_fl(const char *file, int line,
			       const char *fmt, va_list ap);

#define trace2_region_enter_va(fmt, ap) \
	trace2_region_enter_va_fl(__FILE__, __LINE__, (fmt), (ap))

void trace2_region_leave_fl(const char *file, int line, const char *fmt, ...);

#ifdef HAVE_VARIADIC_MACROS
#define trace2_region_leave(...) \
	trace2_region_leave_fl(__FILE__, __LINE__, __VA_ARGS__)
#else
__attribute__((format (printf, 1, 2)))
void trace2_region_leave(const char *fmt, ...);
#endif

/*
 * Emit a 'data' event to report a <category>.<key> = <value> pair.
 * This can be used to report global data, such as the size of the index,
 * or by a thread to report TLS data, such as their assigned subset.
 *
 * On event-based TRACE2 targets, this generates a 'data' event suitable
 * for post-processing.  On printf-based TRACE2 targets, this is converted
 * into a fixed-format printf message.
 */
void trace2_data_intmax_fl(const char *file, int line,
			   const char *category,
			   const char *key,
			   intmax_t value);

#define trace2_data_intmax(c, k, v) \
	trace2_data_intmax_fl(__FILE__, __LINE__, (c), (k), (v))

void trace2_data_string_fl(const char *file, int line,
			   const char *category,
			   const char *key,
			   const char *value);

#define trace2_data_string(c, k, v) \
	trace2_data_string_fl(__FILE__, __LINE__, (c), (k), (v))

#endif /* TRACE2_H */
