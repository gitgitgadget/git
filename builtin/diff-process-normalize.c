/*
 * Built-in diff process that returns zero hunks for files whose
 * only differences are whitespace, and status=error otherwise.
 * See diff-process.c for the protocol and gitattributes(5) for usage.
 *
 * Uses xdiff_compare_lines() with XDF_IGNORE_WHITESPACE to compare
 * lines, giving the same whitespace handling as "git diff -w".
 */

#include "builtin.h"
#include "pkt-line.h"
#include "strbuf.h"
#include "xdiff-interface.h"

/*
 * Read a single pkt-line.  Returns 1 for data, 0 for flush, -1 for EOF.
 */
static int read_pkt(int fd, struct strbuf *line)
{
	int len;
	char *data;

	if (packet_read_line_gently(fd, &len, &data) < 0)
		return -1;
	if (!data || !len)
		return 0; /* flush */
	strbuf_reset(line);
	strbuf_add(line, data, len);
	strbuf_rtrim(line);
	return 1;
}

/*
 * Read packetized content until a flush packet.
 */
static int read_content(int fd, struct strbuf *out)
{
	strbuf_reset(out);
	if (read_packetized_to_strbuf(fd, out, PACKET_READ_GENTLE_ON_EOF) < 0)
		return -1;
	return 0;
}

/*
 * Compare two buffers line by line using xdiff_compare_lines() with
 * XDF_IGNORE_WHITESPACE (same logic as "git diff -w").
 * Returns 1 if all lines match, 0 otherwise.
 */
static int whitespace_equivalent(const char *a, long size_a,
				 const char *b, long size_b)
{
	const char *ea = a + size_a;
	const char *eb = b + size_b;

	while (a < ea && b < eb) {
		const char *eol_a = memchr(a, '\n', ea - a);
		const char *eol_b = memchr(b, '\n', eb - b);
		long len_a = (eol_a ? eol_a : ea) - a;
		long len_b = (eol_b ? eol_b : eb) - b;

		if (!xdiff_compare_lines(a, len_a, b, len_b,
					 XDF_IGNORE_WHITESPACE))
			return 0;

		a += len_a + (eol_a ? 1 : 0);
		b += len_b + (eol_b ? 1 : 0);
	}

	/* Both sides must be exhausted */
	return a >= ea && b >= eb;
}

int cmd_diff_process_normalize(int argc UNUSED, const char **argv UNUSED,
			       const char *prefix UNUSED,
			       struct repository *repo UNUSED)
{
	struct strbuf line = STRBUF_INIT;
	struct strbuf old_content = STRBUF_INIT;
	struct strbuf new_content = STRBUF_INIT;
	int ret;

	/* Handshake: read client greeting */
	ret = read_pkt(0, &line);
	if (ret <= 0 || strcmp(line.buf, "git-diff-client"))
		return 1;
	ret = read_pkt(0, &line);
	if (ret <= 0 || strcmp(line.buf, "version=1"))
		return 1;
	read_pkt(0, &line); /* flush */

	/* Send server greeting */
	packet_write_fmt(1, "git-diff-server\n");
	packet_write_fmt(1, "version=1\n");
	packet_flush(1);

	/* Read client capabilities until flush */
	while ((ret = read_pkt(0, &line)) > 0)
		; /* consume */

	/* Send our capabilities */
	packet_write_fmt(1, "capability=hunks\n");
	packet_flush(1);

	/* Main loop: process file pairs */
	for (;;) {
		int have_command = 0;

		/* Read request headers until flush */
		while ((ret = read_pkt(0, &line)) > 0) {
			if (starts_with(line.buf, "command="))
				have_command = 1;
		}
		if (ret < 0)
			break; /* EOF: client closed connection */
		if (!have_command)
			break;

		/* Read old file content */
		if (read_content(0, &old_content) < 0)
			break;
		/* Read new file content */
		if (read_content(0, &new_content) < 0)
			break;

		if (whitespace_equivalent(old_content.buf, old_content.len,
					  new_content.buf, new_content.len)) {
			/* Whitespace-only differences */
			packet_flush(1); /* zero hunks */
			packet_write_fmt(1, "status=success\n");
			packet_flush(1);
		} else {
			/* Non-whitespace differences: fall back */
			packet_flush(1);
			packet_write_fmt(1, "status=error\n");
			packet_flush(1);
		}
	}

	strbuf_release(&line);
	strbuf_release(&old_content);
	strbuf_release(&new_content);
	return 0;
}
