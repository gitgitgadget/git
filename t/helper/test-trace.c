#include "test-tool.h"
#include "cache.h"
#include "run-command.h"

static struct child_process children[2] = {
	CHILD_PROCESS_INIT,
	CHILD_PROCESS_INIT,
};

#define SAY(child, what) \
	if (write_in_full(children[child].in, \
			  what "\n", strlen(what) + 1) < 0) \
		die("Failed to tell child process #%d to %s", child, what)

#define LISTEN(child, what) \
	if (strbuf_getwholeline_fd(&buf, children[child].out, '\n') < 0) \
		die("Child process #%d failed to acknowledge %s", child, what)

#define ACK(what) \
	if (write_in_full(1, what ": ACK\n", strlen(what) + 6) < 0) \
		die_errno("'%s': %s ACK", child_name, what)

static void contention_orchestrator(const char *argv0)
{
	struct strbuf buf = STRBUF_INIT;
	int i;

	/* Spawn two children and simulate write contention */
	trace_printf("start");

	for (i = 0; i < 2; i++) {
		strbuf_reset(&buf);
		strbuf_addf(&buf, "child #%d", i);
		argv_array_pushl(&children[i].args,
					argv0, "trace", "lock", buf.buf, NULL);
		children[i].in = children[i].out = -1;
		if (start_command(&children[i]) < 0)
			die("Could not spawn child process #%d", i);
	}

	SAY(1, "lock");
	LISTEN(1, "lock");

	SAY(0, "trace delayed");
	SAY(1, "trace while-locked");
	LISTEN(1, "trace");

	SAY(1, "unlock");
	LISTEN(1, "unlock");
	LISTEN(0, "trace");

	SAY(0, "quit");
	SAY(1, "quit");

	if (finish_command(&children[0]) < 0 ||
		finish_command(&children[1]) < 0)
		die("Child process failed to finish");

	strbuf_release(&buf);
}

static void child(const char *child_name)
{
	struct strbuf buf = STRBUF_INIT;
	int fd, locked = 0;
	const char *p;

	/* This is the child process */
	trace_printf("child start: '%s'", child_name);
	fd = trace_default_key.fd;
	if (fd <= 0)
		die("child process: not tracing...");
	while (!strbuf_getwholeline_fd(&buf, 0, '\n')) {
		strbuf_rtrim(&buf);
		if (!strcmp("lock", buf.buf)) {
			if (lock_or_unlock_fd_for_appending(fd, 1) < 0 &&
			    errno != EBADF && errno != EINVAL)
				die_errno("'%s': lock", child_name);
			ACK("lock");
			locked = 1;
		} else if (!strcmp("unlock", buf.buf)) {
			if (lock_or_unlock_fd_for_appending(fd, 0) < 0 &&
			    errno != EBADF && errno != EINVAL)
				die_errno("'%s': unlock", child_name);
			ACK("unlock");
			locked = 0;
		} else if (skip_prefix(buf.buf, "trace ", &p)) {
			if (!locked)
				trace_printf("%s: %s", child_name, p);
			else {
				char *p2 = xstrdup(p);

				/* Give the racy process a run for its money. */
				sleep_millisec(50);

				strbuf_reset(&buf);
				strbuf_addf(&buf, "%s: %s\n",
					    child_name, p2);
				free(p2);

				if (write_in_full(fd, buf.buf, buf.len) < 0)
					die_errno("'%s': trace", child_name);
			}
			ACK("trace");
		} else if (!strcmp("quit", buf.buf)) {
			close(0);
			close(1);
			break;
		} else
			die("Unhandled command: '%s'", buf.buf);

	}

	strbuf_release(&buf);
}

int cmd__trace(int argc, const char **argv)
{
	const char *argv0 = argv[-1];

	if (argc > 1 && !strcmp("lock", argv[1])) {
		if (argc > 2)
			child(argv[2]);
		else
			contention_orchestrator(argv0);
	} else
		die("Usage: %s %s lock [<child-name>]", argv0, argv[0]);

	return 0;
}
