#include "builtin.h"
#include "config.h"
#include "commit-graph.h"
#include "midx.h"
#include "object-store.h"
#include "packfile.h"
#include "parse-options.h"
#include "repository.h"
#include "run-command.h"

static char const * const builtin_run_job_usage[] = {
	N_("git run-job (commit-graph|fetch|loose-objects|pack-files) [<options>]"),
	NULL
};

static char const * const builtin_run_job_loose_objects_usage[] = {
	N_("git run-job loose-objects [--batch-size=<count>]"),
	NULL
};

static char const * const builtin_run_job_pack_file_usage[] = {
	N_("git run-job pack-files [--batch-size=<size>]"),
	NULL
};

static int run_write_commit_graph(void)
{
	struct argv_array cmd = ARGV_ARRAY_INIT;

	argv_array_pushl(&cmd, "commit-graph", "write",
			 "--split", "--reachable", "--no-progress",
			 NULL);

	return run_command_v_opt(cmd.argv, RUN_GIT_CMD);
}

static int run_verify_commit_graph(void)
{
	struct argv_array cmd = ARGV_ARRAY_INIT;

	argv_array_pushl(&cmd, "commit-graph", "verify",
			 "--shallow", "--no-progress", NULL);

	return run_command_v_opt(cmd.argv, RUN_GIT_CMD);
}

static int run_commit_graph_job(void)
{
	char *chain_path;

	if (run_write_commit_graph()) {
		error(_("failed to write commit-graph"));
		return 1;
	}

	if (!run_verify_commit_graph())
		return 0;

	warning(_("commit-graph verify caught error, rewriting"));

	chain_path = get_chain_filename(the_repository->objects->odb);
	if (unlink(chain_path)) {
		UNLEAK(chain_path);
		die(_("failed to remove commit-graph at %s"), chain_path);
	}
	free(chain_path);

	if (!run_write_commit_graph())
		return 0;

	error(_("failed to rewrite commit-graph"));
	return 1;
}

static int fetch_remote(const char *remote)
{
	int result;
	struct argv_array cmd = ARGV_ARRAY_INIT;
	struct strbuf refmap = STRBUF_INIT;

	argv_array_pushl(&cmd, "fetch", remote, "--quiet", "--prune",
			 "--no-tags", "--refmap=", NULL);

	strbuf_addf(&refmap, "+refs/heads/*:refs/hidden/%s/*", remote);
	argv_array_push(&cmd, refmap.buf);

	result = run_command_v_opt(cmd.argv, RUN_GIT_CMD);

	strbuf_release(&refmap);
	return result;
}

static int fill_remotes(struct string_list *remotes)
{
	int result = 0;
	FILE *proc_out;
	struct strbuf line = STRBUF_INIT;
	struct child_process *remote_proc = xmalloc(sizeof(*remote_proc));

	child_process_init(remote_proc);

	argv_array_pushl(&remote_proc->args, "git", "remote", NULL);

	remote_proc->out = -1;

	if (start_command(remote_proc)) {
		error(_("failed to start 'git remote' process"));
		result = 1;
		goto cleanup;
	}

	proc_out = xfdopen(remote_proc->out, "r");

	/* if there is no line, leave the value as given */
	while (!strbuf_getline(&line, proc_out))
		string_list_append(remotes, line.buf);

	strbuf_release(&line);

	fclose(proc_out);

	if (finish_command(remote_proc)) {
		error(_("failed to finish 'git remote' process"));
		result = 1;
	}

cleanup:
	free(remote_proc);
	return result;
}

static int run_fetch_job(void)
{
	int result = 0;
	struct string_list_item *item;
	struct string_list remotes = STRING_LIST_INIT_DUP;

	if (fill_remotes(&remotes)) {
		error(_("failed to fill remotes"));
		result = 1;
		goto cleanup;
	}

	/*
	 * Do not modify the result based on the success of the 'fetch'
	 * operation, as a loss of network could cause 'fetch' to fail
	 * quickly. We do not want that to stop the rest of our
	 * background operations.
	 */
	for (item = remotes.items;
	     item && item < remotes.items + remotes.nr;
	     item++)
		fetch_remote(item->string);

cleanup:
	string_list_clear(&remotes, 0);
	return result;
}

static int prune_packed(void)
{
	struct argv_array cmd = ARGV_ARRAY_INIT;
	argv_array_pushl(&cmd, "prune-packed", NULL);
	return run_command_v_opt(cmd.argv, RUN_GIT_CMD);
}

struct write_loose_object_data {
	FILE *in;
	int count;
	int batch_size;
};

static int loose_object_exists(const struct object_id *oid,
			       const char *path,
			       void *data)
{
	return 1;
}

static int write_loose_object_to_stdin(const struct object_id *oid,
				       const char *path,
				       void *data)
{
	struct write_loose_object_data *d = (struct write_loose_object_data *)data;

	fprintf(d->in, "%s\n", oid_to_hex(oid));

	return ++(d->count) > d->batch_size;
}

static int pack_loose(int batch_size)
{
	int result = 0;
	struct write_loose_object_data data;
	struct strbuf prefix = STRBUF_INIT;
	struct child_process *pack_proc;

	/*
	 * Do not start pack-objects process
	 * if there are no loose objects.
	 */
	if (!for_each_loose_file_in_objdir(the_repository->objects->odb->path,
					   loose_object_exists,
					   NULL, NULL, NULL))
		return 0;

	pack_proc = xmalloc(sizeof(*pack_proc));

	child_process_init(pack_proc);

	strbuf_addstr(&prefix, the_repository->objects->odb->path);
	strbuf_addstr(&prefix, "/pack/loose");

	argv_array_pushl(&pack_proc->args, "git", "pack-objects",
			 "--quiet", prefix.buf, NULL);

	pack_proc->in = -1;

	if (start_command(pack_proc)) {
		error(_("failed to start 'git pack-objects' process"));
		result = 1;
		goto cleanup;
	}

	data.in = xfdopen(pack_proc->in, "w");
	data.count = 0;
	data.batch_size = batch_size;

	for_each_loose_file_in_objdir(the_repository->objects->odb->path,
				      write_loose_object_to_stdin,
				      NULL,
				      NULL,
				      &data);

	fclose(data.in);

	if (finish_command(pack_proc)) {
		error(_("failed to finish 'git pack-objects' process"));
		result = 1;
	}

cleanup:
	strbuf_release(&prefix);
	free(pack_proc);
	return result;
}

static int run_loose_objects_job(int argc, const char **argv)
{
	static int batch_size;
	static struct option builtin_run_job_loose_objects_options[] = {
		OPT_INTEGER(0, "batch-size", &batch_size,
			    N_("specify the maximum number of loose objects to store in a pack-file")),
		OPT_END(),
	};

	if (repo_config_get_int(the_repository,
				"job.loose-objects.batchsize",
				&batch_size))
		batch_size = 50000;

	argc = parse_options(argc, argv, NULL,
			     builtin_run_job_loose_objects_options,
			     builtin_run_job_loose_objects_usage, 0);

	return prune_packed() || pack_loose(batch_size);
}

static int multi_pack_index_write(void)
{
	struct argv_array cmd = ARGV_ARRAY_INIT;
	argv_array_pushl(&cmd, "multi-pack-index", "write",
			 "--no-progress", NULL);
	return run_command_v_opt(cmd.argv, RUN_GIT_CMD);
}

static int rewrite_multi_pack_index(void)
{
	char *midx_name = get_midx_filename(the_repository->objects->odb->path);

	unlink(midx_name);
	free(midx_name);

	if (multi_pack_index_write()) {
		error(_("failed to rewrite multi-pack-index"));
		return 1;
	}

	return 0;
}

static int multi_pack_index_verify(void)
{
	struct argv_array cmd = ARGV_ARRAY_INIT;
	argv_array_pushl(&cmd, "multi-pack-index", "verify",
			 "--no-progress", NULL);
	return run_command_v_opt(cmd.argv, RUN_GIT_CMD);
}

static int multi_pack_index_expire(void)
{
	struct argv_array cmd = ARGV_ARRAY_INIT;
	argv_array_pushl(&cmd, "multi-pack-index", "expire",
			 "--no-progress", NULL);
	return run_command_v_opt(cmd.argv, RUN_GIT_CMD);
}

#define TWO_GIGABYTES (2147483647)

static off_t get_auto_pack_size(int *count)
{
	/*
	 * The "auto" value is special: we optimize for
	 * one large pack-file (i.e. from a clone) and
	 * expect the rest to be small and they can be
	 * repacked quickly. Find the sum of the sizes
	 * other than the largest pack-file, then use
	 * that as the batch size.
	 */
	off_t total_size = 0;
	off_t max_size = 0;
	off_t result_size;
	struct packed_git *p;

	*count = 0;

	reprepare_packed_git(the_repository);
	for (p = get_all_packs(the_repository); p; p = p->next) {
		(*count)++;
		total_size += p->pack_size;

		if (p->pack_size > max_size)
			max_size = p->pack_size;
	}

	result_size = total_size - max_size;

	/* But limit ourselves to a batch size of 2g */
	if (result_size > TWO_GIGABYTES)
		result_size = TWO_GIGABYTES;

	return result_size;
}

#define UNSET_BATCH_SIZE ((unsigned long)-1)
static int multi_pack_index_repack(unsigned long batch_size)
{
	int result;
	struct argv_array cmd = ARGV_ARRAY_INIT;
	struct strbuf batch_arg = STRBUF_INIT;
	const char *config_value;
	int count;
	off_t default_size = get_auto_pack_size(&count);

	if (count <= 2)
		return 0;

	strbuf_addstr(&batch_arg, "--batch-size=");

	if (batch_size != UNSET_BATCH_SIZE)
		strbuf_addf(&batch_arg, "\"%"PRIuMAX"\"", (uintmax_t) batch_size);
	else if (!repo_config_get_string_const(the_repository,
					       "job.pack-file.batchsize",
					       &config_value))
		strbuf_addf(&batch_arg, "\"%s\"", config_value);
	else
		strbuf_addf(&batch_arg, "%"PRIuMAX,
			    (uintmax_t)default_size);

	argv_array_pushl(&cmd, "multi-pack-index", "repack",
			 "--no-progress", batch_arg.buf, NULL);
	result = run_command_v_opt(cmd.argv, RUN_GIT_CMD);

	strbuf_release(&batch_arg);

	/*
	 * Verify here to avoid verifying again when there are two
	 * or fewer pack-files.
	 */
	if (!result && multi_pack_index_verify()) {
		warning(_("multi-pack-index verify failed after repack"));
		result = rewrite_multi_pack_index();
	}

	return result;
}

static int run_pack_files_job(int argc, const char **argv)
{
	static unsigned long batch_size = UNSET_BATCH_SIZE;
	static struct option builtin_run_job_pack_file_options[] = {
		OPT_MAGNITUDE(0, "batch-size", &batch_size,
			      N_("specify a batch-size for the incremental repack")),
		OPT_END(),
	};

	argc = parse_options(argc, argv, NULL,
			     builtin_run_job_pack_file_options,
			     builtin_run_job_pack_file_usage, 0);

	if (multi_pack_index_write()) {
		error(_("failed to write multi-pack-index"));
		return 1;
	}

	if (multi_pack_index_verify()) {
		warning(_("multi-pack-index verify failed after initial write"));
		return rewrite_multi_pack_index();
	}

	if (multi_pack_index_expire()) {
		error(_("multi-pack-index expire failed"));
		return 1;
	}

	if (multi_pack_index_verify()) {
		warning(_("multi-pack-index verify failed after expire"));
		return rewrite_multi_pack_index();
	}

	if (multi_pack_index_repack(batch_size)) {
		error(_("multi-pack-index repack failed"));
		return 1;
	}

	return 0;
}

int cmd_run_job(int argc, const char **argv, const char *prefix)
{
	static struct option builtin_run_job_options[] = {
		OPT_END(),
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_run_job_usage,
				   builtin_run_job_options);

	git_config(git_default_config, NULL);
	argc = parse_options(argc, argv, prefix,
			     builtin_run_job_options,
			     builtin_run_job_usage,
			     PARSE_OPT_KEEP_UNKNOWN);

	if (argc > 0) {
		if (!strcmp(argv[0], "commit-graph"))
			return run_commit_graph_job();
		if (!strcmp(argv[0], "fetch"))
			return run_fetch_job();
		if (!strcmp(argv[0], "loose-objects"))
			return run_loose_objects_job(argc, argv);
		if (!strcmp(argv[0], "pack-files"))
			return run_pack_files_job(argc, argv);
	}

	usage_with_options(builtin_run_job_usage,
			   builtin_run_job_options);
}
