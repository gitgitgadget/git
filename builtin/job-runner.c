#include "builtin.h"
#include "config.h"
#include "daemon.h"
#include "parse-options.h"
#include "run-command.h"
#include "string-list.h"

#define MAX_TIMESTAMP ((timestamp_t)-1)

static char const * const builtin_job_runner_usage[] = {
	N_("git job-runner [<options>]"),
	NULL
};

static char *config_name(const char *prefix,
			 const char *job,
			 const char *postfix)
{
	int postfix_dot = 0;
	struct strbuf name = STRBUF_INIT;

	if (prefix) {
		strbuf_addstr(&name, prefix);
		strbuf_addch(&name, '.');
	}

	if (job) {
		strbuf_addstr(&name, job);
		postfix_dot = 1;
	}

	if (postfix) {
		if (postfix_dot)
			strbuf_addch(&name, '.');
		strbuf_addstr(&name, postfix);
	}

	return strbuf_detach(&name, NULL);
}

static int try_get_config(const char *job,
			  const char *repo,
			  const char *postfix,
			  char **value)
{
	int result = 0;
	FILE *proc_out;
	struct strbuf line = STRBUF_INIT;
	char *cname = config_name("job", job, postfix);
	struct child_process *config_proc = xmalloc(sizeof(*config_proc));

	child_process_init(config_proc);

	argv_array_push(&config_proc->args, "git");
	argv_array_push(&config_proc->args, "-C");
	argv_array_push(&config_proc->args, repo);
	argv_array_push(&config_proc->args, "config");
	argv_array_push(&config_proc->args, cname);
	free(cname);

	config_proc->out = -1;

	if (start_command(config_proc)) {
		warning(_("failed to start process for repo '%s'"),
			repo);
		result = 1;
		goto cleanup;
	}

	proc_out = xfdopen(config_proc->out, "r");

	/* if there is no line, leave the value as given */
	if (!strbuf_getline(&line, proc_out))
		*value = strbuf_detach(&line, NULL);
	else
		*value = NULL;

	strbuf_release(&line);

	fclose(proc_out);

	/* ignore result as 'git config' fails on non-existent value */
	finish_command(config_proc);

cleanup:
	free(config_proc);
	return result;
}

static int try_get_timestamp(const char *job,
			     const char *repo,
			     const char *postfix,
			     timestamp_t *t)
{
	char *value;
	int result = try_get_config(job, repo, postfix, &value);

	if (!result && value) {
		*t = atol(value);
		free(value);
	}

	return result;
}

static timestamp_t get_last_run(const char *job,
				const char *repo)
{
	timestamp_t last_run = 0;

	/* In an error state, do not run the job */
	if (try_get_timestamp(job, repo, "lastrun", &last_run))
		return MAX_TIMESTAMP;

	return last_run;
}

static timestamp_t get_interval(const char *job,
				const char *repo)
{
	timestamp_t interval = MAX_TIMESTAMP;

	/* In an error state, do not run the job */
	if (try_get_timestamp(job, repo, "interval", &interval))
		return MAX_TIMESTAMP;

	if (interval == MAX_TIMESTAMP) {
		/* load defaults for each job */
		if (!strcmp(job, "commit-graph") || !strcmp(job, "fetch"))
			interval = 60 * 60; /* 1 hour */
		else
			interval = 24 * 60 * 60; /* 1 day */
	}

	return interval;
}

static int set_last_run(const char *job,
			const char *repo,
			timestamp_t last_run)
{
	int result = 0;
	struct strbuf last_run_string = STRBUF_INIT;
	struct child_process *config_proc = xmalloc(sizeof(*config_proc));
	char *cname = config_name("job", job, "lastrun");

	strbuf_addf(&last_run_string, "%"PRItime, last_run);

	child_process_init(config_proc);

	argv_array_push(&config_proc->args, "git");
	argv_array_push(&config_proc->args, "-C");
	argv_array_push(&config_proc->args, repo);
	argv_array_push(&config_proc->args, "config");
	argv_array_push(&config_proc->args, cname);
	argv_array_push(&config_proc->args, last_run_string.buf);
	free(cname);
	strbuf_release(&last_run_string);

	if (start_command(config_proc)) {
		warning(_("failed to start process for repo '%s'"),
			repo);
		result = 1;
		goto cleanup;
	}

	if (finish_command(config_proc)) {
		warning(_("failed to finish process for repo '%s'"),
			repo);
		result = 1;
	}

cleanup:
	free(config_proc);
	return result;
}

static struct string_list arg_repos = STRING_LIST_INIT_DUP;

static int arg_repos_append(const struct option *opt,
			    const char *arg, int unset)
{
	string_list_append(&arg_repos, arg);
	return 0;
}

static int load_active_repos(struct string_list *repos)
{
	struct string_list_item *item;
	const struct string_list *config_repos;

	if (arg_repos.nr) {
		struct string_list_item *item;
		for (item = arg_repos.items;
		     item && item < arg_repos.items + arg_repos.nr;
		     item++)
			string_list_append(repos, item->string);
		return 0;
	}

	config_repos = git_config_get_value_multi("job.repo");

	for (item = config_repos->items;
	     item && item < config_repos->items + config_repos->nr;
	     item++) {
		DIR *dir = opendir(item->string);

		if (!dir)
			continue;

		closedir(dir);
		string_list_append(repos, item->string);
	}

	string_list_sort(repos);
	string_list_remove_duplicates(repos, 0);

	return 0;
}

static int job_disabled(const char *job, const char *repo)
{
	char *enabled;

	if (!try_get_config(job, repo, "enabled", &enabled)) {
		int enabled_int = git_parse_maybe_bool(enabled);
		free(enabled);
		return !enabled_int;
	}

	return 0;
}

static int run_job(const char *job, const char *repo)
{
	int result;
	struct argv_array cmd = ARGV_ARRAY_INIT;
	timestamp_t now = time(NULL);
	timestamp_t last_run, interval;

	if (job_disabled(job, repo))
		return 0;

	last_run = get_last_run(job, repo);
	interval = get_interval(job, repo);

	if (last_run + interval > now)
		return 0;

	argv_array_pushl(&cmd, "-C", repo, "run-job", job, NULL);
	result = run_command_v_opt(cmd.argv, RUN_GIT_CMD);

	set_last_run(job, repo, now);
	return result;
}

static int run_job_loop_step(struct string_list *list)
{
	int result = 0;
	struct string_list_item *job;
	struct string_list repos = STRING_LIST_INIT_DUP;

	if ((result = load_active_repos(&repos)))
		return result;

	for (job = list->items;
	     !result && job && job < list->items + list->nr;
	     job++) {
		struct string_list_item *repo;

		if (job_disabled(job->string, "."))
			continue;

		for (repo = repos.items;
		     !result && repo && repo < repos.items + repos.nr;
		     repo++)
			result = run_job(job->string,
					 repo->string);
	}

	string_list_clear(&repos, 0);
	return result;
}

static unsigned int arg_interval = 0;

static unsigned int get_loop_interval(void)
{
	/* Default: 30 minutes */
	timestamp_t interval = 30 * 60;

	if (arg_interval)
		return arg_interval;

	try_get_timestamp(NULL, ".", "loopinterval", &interval);

	return interval;
}

static int initialize_jobs(struct string_list *list)
{
	string_list_append(list, "commit-graph");
	string_list_append(list, "fetch");
	string_list_append(list, "loose-objects");
	string_list_append(list, "pack-files");
	return 0;
}

static int arg_daemonize = 0;
int cmd_job_runner(int argc, const char **argv, const char *prefix)
{
	int result;
	struct string_list job_list = STRING_LIST_INIT_DUP;
	static struct option builtin_job_runner_options[] = {
		OPT_CALLBACK_F(0, "repo",
			       NULL,
			       N_("<path>"),
			       N_("run jobs on the repository at <path>"),
			       PARSE_OPT_NONEG, arg_repos_append),
		OPT_INTEGER(0, "interval", &arg_interval,
			    N_("seconds to pause between running any jobs")),
		OPT_BOOL(0, "daemonize", &arg_daemonize,
			 N_("request to spawn a background process")),
		OPT_END(),
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_job_runner_usage,
				   builtin_job_runner_options);

	argc = parse_options(argc, argv, prefix,
			     builtin_job_runner_options,
			     builtin_job_runner_usage,
			     0);

	if (arg_daemonize && daemonize())
		die(_("failed to daemonize; this may not be available on your platform."));

	result = initialize_jobs(&job_list);

	while (!(result = run_job_loop_step(&job_list))) {
		unsigned int interval = get_loop_interval();
		sleep(interval);
	}

	return result;
}
