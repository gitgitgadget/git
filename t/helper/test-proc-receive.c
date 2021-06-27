#include "cache.h"
#include "connect.h"
#include "parse-options.h"
#include "pkt-line.h"
#include "sigchain.h"
#include "test-tool.h"

static const char *proc_receive_usage[] = {
	"test-tool proc-receive [<options>...]",
	NULL
};

static int die_read_version;
static int die_write_version;
static int die_read_commands;
static int die_read_push_options;
static int die_write_report;
static int no_push_options;
static int use_atomic;
static int use_push_options;
static int verbose;
static int version = 1;
static struct string_list returns = STRING_LIST_INIT_NODUP;

struct command {
	struct command *next;
	const char *error_string;
	unsigned int skip_update:1,
		     did_not_exist:1;
	int index;
	char line[FLEX_ARRAY]; /* more */
};

static void proc_receive_verison(struct packet_reader *reader) {
	int server_version = 0;

	if (die_read_version)
		die("die with the --die-read-version option");

	for (;;) {
		int linelen;

		if (packet_reader_read(reader) != PACKET_READ_NORMAL)
			break;

		/* Ignore version negotiation for version 0 */
		if (version == 0)
			continue;

		if (reader->pktlen > 8 && starts_with(reader->line, "version=")) {
			server_version = atoi(reader->line+8);
			if (server_version != 1)
				die("bad protocol version: %d", server_version);
			linelen = strlen(reader->line);
			if (linelen < reader->pktlen) {
				const char *feature_list = reader->line + linelen + 1;
				if (parse_feature_request(feature_list, "atomic"))
					use_atomic= 1;
				if (parse_feature_request(feature_list, "push-options"))
					use_push_options = 1;
			}
		}
	}

	if (die_write_version)
		die("die with the --die-write-version option");

	if (version != 0)
		packet_write_fmt(1, "version=%d%c%s\n",
				 version, '\0',
				 use_push_options && !no_push_options ? "push-options": "");
	packet_flush(1);
}

static void proc_receive_read_commands(struct packet_reader *reader,
				       struct command **commands)
{
	struct command **tail = commands;

	for (;;) {
		struct object_id old_oid, new_oid;
		struct command *cmd;
		const char *refname;
		const char *p;

		if (packet_reader_read(reader) != PACKET_READ_NORMAL)
			break;

		if (die_read_commands)
			die("die with the --die-read-commands option");

		FLEX_ALLOC_STR(cmd, line, reader->line);

		*tail = cmd;
		tail = &cmd->next;
	}
}

static void proc_receive_read_push_options(struct packet_reader *reader,
					   struct string_list *options)
{

	if (no_push_options || !use_push_options)
	       return;

	if (die_read_push_options)
		die("die with the --die-read-push-options option");

	while (1) {
		if (packet_reader_read(reader) != PACKET_READ_NORMAL)
			break;

		string_list_append(options, reader->line);
	}
}

int cmd__proc_receive(int argc, const char **argv)
{
	int nongit_ok = 0;
	struct packet_reader reader;
	struct command *commands = NULL;
	struct string_list push_options = STRING_LIST_INIT_DUP;
	struct string_list_item *item;
	struct option options[] = {
		OPT_BOOL(0, "no-push-options", &no_push_options,
			 "disable push options"),
		OPT_BOOL(0, "die-read-version", &die_read_version,
			 "die when reading version"),
		OPT_BOOL(0, "die-write-version", &die_write_version,
			 "die when writing version"),
		OPT_BOOL(0, "die-read-commands", &die_read_commands,
			 "die when reading commands"),
		OPT_BOOL(0, "die-read-push-options", &die_read_push_options,
			 "die when reading push-options"),
		OPT_BOOL(0, "die-write-report", &die_write_report,
			 "die when writing report"),
		OPT_STRING_LIST('r', "return", &returns, "old/new/ref/status/msg",
				"return of results"),
		OPT__VERBOSE(&verbose, "be verbose"),
		OPT_INTEGER('V', "version", &version,
			    "use this protocol version number"),
		OPT_END()
	};

	setup_git_directory_gently(&nongit_ok);

	argc = parse_options(argc, argv, "test-tools", options, proc_receive_usage, 0);
	if (argc > 0)
		usage_msg_opt("Too many arguments.", proc_receive_usage, options);
	packet_reader_init(&reader, 0, NULL, 0,
			   PACKET_READ_CHOMP_NEWLINE |
			   PACKET_READ_GENTLE_ON_EOF);

	sigchain_push(SIGPIPE, SIG_IGN);
	proc_receive_verison(&reader);
	proc_receive_read_commands(&reader, &commands);
	proc_receive_read_push_options(&reader, &push_options);

	if (verbose) {
		struct command *cmd;
		size_t hexsz = reader.hash_algo->hexsz;

		if (use_push_options || use_atomic)
			fprintf(stderr, "proc-receive:%s%s\n",
				use_atomic? " atomic": "",
				use_push_options ? " push_options": "");

		for (cmd = commands; cmd; cmd = cmd->next) {
			size_t reflen = strlen(cmd->line) - 2 * hexsz - 2;
			char old[hexsz], new[hexsz], refname[reflen];
			memset(old, 0, hexsz);
			memset(new, 0, hexsz);
			memset(refname, 0, strlen(cmd->line) - reflen);

			memcpy(old, cmd->line, hexsz);
			memcpy(new, cmd->line + hexsz + 1, hexsz);
			memcpy(refname, cmd->line + 2 * hexsz + 2, reflen);

			old[hexsz] = '\0';
			new[hexsz] = '\0';
			refname[reflen] = '\0';

			fprintf(stderr, "proc-receive< %s %s %s\n", old, new, refname);
		}

		if (push_options.nr > 0)
			for_each_string_list_item(item, &push_options)
				fprintf(stderr, "proc-receive< %s\n", item->string);

		if (returns.nr)
			for_each_string_list_item(item, &returns)
				fprintf(stderr, "proc-receive> %s\n", item->string);
	}

	if (die_write_report)
		die("die with the --die-write-report option");
	if (returns.nr)
		for_each_string_list_item(item, &returns)
			packet_write_fmt(1, "%s\n", item->string);
	packet_flush(1);
	sigchain_pop(SIGPIPE);

	return 0;
}
