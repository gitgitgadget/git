#include "git-compat-util.h"
#include "json-writer.h"
#include "version.h"
#include "thread-utils.h"

#ifndef NO_CURL
#include <curl/curl.h>
#endif

/*
 * Fixed details of the compiled architecture.
 */
static void cmd__arch(struct json_writer *jw)
{
	/*
	 * We do not probe the system for the actual CPU architecture name.
	 * We just report the value of the #define from the Makefile.
	 */
	jw_object_string(jw, "defined", GIT_HOST_CPU);

	jw_object_intmax(jw, "sizeof_long", sizeof(long));
	jw_object_intmax(jw, "sizeof_size_t", sizeof(size_t));
	jw_object_intmax(jw, "sizeof_intmax_t", sizeof(intmax_t));
	jw_object_intmax(jw, "sizeof_pointer", sizeof(void *));
}

#ifdef GIT_WINDOWS_NATIVE
static void system_helper_windows(struct json_writer *jw)
{
	SYSTEM_INFO info;
	GetSystemInfo(&info);

	jw_object_inline_begin_object(jw, "windows");

	jw_object_intmax(jw, "processor_architecture",
			 info.wProcessorArchitecture);
	jw_object_intmax(jw, "processor_level", info.wProcessorLevel);
	jw_object_intmax(jw, "processor_revision", info.wProcessorRevision);

	jw_end(jw);
}
#endif

/*
 * Details of the actual running system.
 */
static void cmd__system(struct json_writer *jw)
{
	jw_object_intmax(jw, "online_cpus", online_cpus());

#ifdef GIT_WINDOWS_NATIVE
	system_helper_windows(jw);
#endif
}

/*
 * Details of the running version of Git.
 */
static void cmd__version(struct json_writer *jw)
{
	jw_object_string(jw, "git", git_version_string);

	jw_object_bool(jw, "built_from_commit", git_built_from_commit_string[0]);
	if (git_built_from_commit_string[0])
		jw_object_string(jw, "commit", git_built_from_commit_string);
}

static void do_string_or_null(struct json_writer *jw, const char *key,
			      const char *value)
{
	if (value)
		jw_object_string(jw, key, value);
	else
		jw_object_null(jw, key);
}

/*
 * Details of libcurl.
 */
static void cmd__libcurl(struct json_writer *jw)
{
#ifdef NO_CURL
	jw_object_false(jw, "available");
#else
	curl_version_info_data *data;

	jw_object_true(jw, "available");
	jw_object_intmax(jw, "dot_h_version", CURLVERSION_NOW);

	curl_global_init(CURL_GLOBAL_ALL);

	data = curl_version_info(CURLVERSION_NOW);

	if (data->age >= 0) {
		jw_object_intmax(jw, "age", data->age);
		jw_object_string(jw, "version", data->version);
		jw_object_string(jw, "host", data->host);
		do_string_or_null(jw, "libssl_version", data->ssl_version);
		do_string_or_null(jw, "libz_version", data->libz_version);

		/* print just the feature bits we care about */
		jw_object_inline_begin_object(jw, "features");
		jw_object_intmax(jw, "flags", data->features);
		jw_object_bool(jw, "ssl", (data->features & CURL_VERSION_SSL));
		jw_object_bool(jw, "libz", (data->features & CURL_VERSION_LIBZ));
		jw_object_bool(jw, "ntml", (data->features & CURL_VERSION_NTLM));
		jw_object_bool(jw, "conv", (data->features & CURL_VERSION_CONV));
#ifdef CURL_VERSION_MULTI_SSL
		jw_object_bool(jw, "multi_ssl", (data->features & CURL_VERSION_MULTI_SSL));
#endif
		jw_end(jw);

		if (data->protocols) {
			const char *const *p = data->protocols;

			jw_object_inline_begin_array(jw, "protocols");
			while (*p)
				jw_array_string(jw, *p++);
			jw_end(jw);
		}
	}

	/* omit "area" fields in (age >= 1) section */
	/* omit "libidn" fields in (age >= 2) section */

	if (data->age >= 3) {
		jw_object_intmax(jw, "iconv_version", data->iconv_ver_num);
		do_string_or_null(jw, "libssh_version", data->libssh_version);
	}

	/* omit "brotli" fields in (age >= 4) section */
#endif
}


struct diag_cmd {
	const char *name;
	void (*fn)(struct json_writer *jw);
};

static struct diag_cmd cmd_array[] = {
	{ "arch", cmd__arch },
	{ "system", cmd__system },
	{ "version", cmd__version },
	{ "libcurl", cmd__libcurl },
};

static void diag_usage(void)
{
	size_t k;

	fprintf(stderr, "usage: diag [-p | --pretty] [<name>]\n");
	for (k = 0; k < ARRAY_SIZE(cmd_array); k++)
		fprintf(stderr, "  %s\n", cmd_array[k].name);
}

static struct diag_cmd *lookup_cmd(const char *name)
{
	int k;

	for (k = 0; k < ARRAY_SIZE(cmd_array); k++)
		if (!strcmp(cmd_array[k].name, name))
			return &cmd_array[k];

	error("unknown diagnostic: '%s'", name);
	diag_usage();
	exit(128);
}

static void run_cmd(struct diag_cmd *cmd, struct json_writer *jw)
{
	jw_object_inline_begin_object(jw, cmd->name);

	cmd->fn(jw);

	jw_end(jw);
}

static void run_all_cmds(struct json_writer *jw)
{
	int k;

	for (k = 0; k < ARRAY_SIZE(cmd_array); k++)
		run_cmd(&cmd_array[k], jw);
}

int cmd_main(int argc, const char **argv)
{
	int pretty = 0;
	struct json_writer jw = JSON_WRITER_INIT;

	argc--;
	argv++;

	while (argc && argv[0]) {
		if (*argv[0] != '-')
			break;

		if (!strcmp(argv[0], "-h")) {
			diag_usage();
			exit(0);
		}

		if (!strcmp(argv[0], "-p") || !strcmp(argv[0], "--pretty")) {
			pretty = 1;
			argc--;
			argv++;
			continue;
		}
	}

	jw_object_begin(&jw, pretty);

	if (argc)
		run_cmd(lookup_cmd(argv[0]), &jw);
	else
		run_all_cmds(&jw);

	jw_end(&jw);

	printf("%s\n", jw.json.buf);
	jw_release(&jw);

	return 0;
}
