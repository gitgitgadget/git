/*
 * Diff process backend: communicates with a long-running external
 * tool via the pkt-line protocol to obtain custom line-matching
 * results.  Unlike textconv, which transforms the displayed content,
 * hunks from a diff process reference original line numbers and
 * the display shows the actual file content.
 *
 * Protocol: pkt-line over stdin/stdout, following the pattern of
 * the long-running filter process protocol (see convert.c).
 *
 * Handshake:
 *   git> git-diff-client / version=1 / flush
 *   tool< git-diff-server / version=1 / flush
 *   git> capability=hunks / flush
 *   tool< capability=hunks / flush
 *
 * Per-file:
 *   git> command=hunks / pathname=<path> / flush
 *   git> <old content packetized> / flush
 *   git> <new content packetized> / flush
 *   tool< hunk <old_start> <old_count> <new_start> <new_count>
 *   tool< ... / flush
 *   tool< status=success / flush
 *
 * Zero hunks with status=success means the tool considers the
 * files equivalent.  Git will skip the diff for that file.
 */

#include "git-compat-util.h"
#include "diff-process.h"
#include "userdiff.h"
#include "sub-process.h"
#include "pkt-line.h"
#include "strbuf.h"
#include "xdiff/xdiff.h"

#define CAP_HUNKS (1u << 0)

struct diff_subprocess {
	struct subprocess_entry subprocess;
	unsigned int supported_capabilities;
};

static int subprocess_map_initialized;
static struct hashmap subprocess_map;

static int start_diff_process_fn(struct subprocess_entry *subprocess)
{
	static int versions[] = { 1, 0 };
	static struct subprocess_capability capabilities[] = {
		{ "hunks", CAP_HUNKS },
		{ NULL, 0 }
	};
	struct diff_subprocess *entry =
		(struct diff_subprocess *)subprocess;

	/* Uses dying pkt-line variant, same as convert.c filters. */
	return subprocess_handshake(subprocess, "git-diff",
				    versions, NULL,
				    capabilities,
				    &entry->supported_capabilities);
}

static struct diff_subprocess *find_or_start_process(const char *cmd)
{
	struct diff_subprocess *entry;

	if (!subprocess_map_initialized) {
		subprocess_map_initialized = 1;
		hashmap_init(&subprocess_map, cmd2process_cmp, NULL, 0);
	}

	entry = (struct diff_subprocess *)
		subprocess_find_entry(&subprocess_map, cmd);
	if (entry)
		return entry;

	entry = xcalloc(1, sizeof(*entry));
	if (subprocess_start(&subprocess_map, &entry->subprocess,
			     cmd, start_diff_process_fn)) {
		free(entry);
		return NULL;
	}

	return entry;
}

static int send_file_content(int fd, const char *buf, long size)
{
	int ret;

	if (size > 0)
		ret = write_packetized_from_buf_no_flush(buf, size, fd);
	else
		ret = 0;
	if (ret)
		return ret;
	return packet_flush_gently(fd);
}

static int parse_hunk_line(const char *line, struct xdl_hunk *hunk)
{
	char *end;

	/* Format: "hunk <old_start> <old_count> <new_start> <new_count>" */
	if (!skip_prefix(line, "hunk ", &line))
		return -1;

	hunk->old_start = strtol(line, &end, 10);
	if (end == line || *end != ' ')
		return -1;
	line = end;

	hunk->old_count = strtol(line, &end, 10);
	if (end == line || *end != ' ')
		return -1;
	line = end;

	hunk->new_start = strtol(line, &end, 10);
	if (end == line || *end != ' ')
		return -1;
	line = end;

	hunk->new_count = strtol(line, &end, 10);
	if (end == line || *end != '\0')
		return -1;

	return 0;
}

int diff_process_get_hunks(struct userdiff_driver *drv,
			   const char *path,
			   const char *old_buf, long old_size,
			   const char *new_buf, long new_size,
			   struct xdl_hunk **hunks_out,
			   size_t *nr_hunks_out)
{
	struct diff_subprocess *backend;
	struct child_process *process;
	int fd_in, fd_out;
	struct strbuf status = STRBUF_INIT;
	struct xdl_hunk *hunks = NULL;
	struct xdl_hunk hunk;
	size_t nr_hunks = 0, alloc_hunks = 0;
	int len;
	char *line;

	if (!drv || !drv->process)
		return -1;

	backend = find_or_start_process(drv->process);
	if (!backend)
		return -1;

	if (!(backend->supported_capabilities & CAP_HUNKS))
		return -1;

	process = subprocess_get_child_process(&backend->subprocess);
	fd_in = process->in;
	fd_out = process->out;

	/* Send request */
	if (packet_write_fmt_gently(fd_in, "command=hunks\n") ||
	    packet_write_fmt_gently(fd_in, "pathname=%s\n", path) ||
	    packet_flush_gently(fd_in))
		goto error;

	/* Send old file content */
	if (send_file_content(fd_in, old_buf, old_size))
		goto error;

	/* Send new file content */
	if (send_file_content(fd_in, new_buf, new_size))
		goto error;

	/* Read hunks until flush packet */
	while ((len = packet_read_line_gently(fd_out, NULL, &line)) >= 0 &&
	       line) {
		if (parse_hunk_line(line, &hunk) < 0)
			goto error;
		ALLOC_GROW(hunks, nr_hunks + 1, alloc_hunks);
		hunks[nr_hunks++] = hunk;
	}
	if (len < 0)
		goto error;

	/* Read status */
	if (subprocess_read_status(fd_out, &status))
		goto error;

	if (strcmp(status.buf, "success")) {
		if (!strcmp(status.buf, "abort"))
			backend->supported_capabilities &= ~CAP_HUNKS;
		goto error;
	}

	*hunks_out = hunks;
	*nr_hunks_out = nr_hunks;
	strbuf_release(&status);
	return 0;

error:
	free(hunks);
	strbuf_release(&status);
	return -1;
}
