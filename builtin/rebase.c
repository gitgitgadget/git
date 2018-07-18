/*
 * "git rebase" builtin command
 *
 * Copyright (c) 2018 Pratik Karki
 */

#include "builtin.h"
#include "run-command.h"
#include "exec-cmd.h"
#include "argv-array.h"
#include "dir.h"
#include "packfile.h"
#include "checkout.h"
#include "refs.h"
#include "quote.h"

static GIT_PATH_FUNC(apply_dir, "rebase-apply");
static GIT_PATH_FUNC(merge_dir, "rebase-merge");

enum rebase_type {
	REBASE_AM,
	REBASE_MERGE,
	REBASE_INTERACTIVE,
	REBASE_PRESERVE_MERGES
};

static int use_builtin_rebase(void)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf out = STRBUF_INIT;
	int ret;

	argv_array_pushl(&cp.args,
			 "config", "--bool", "rebase.usebuiltin", NULL);
	cp.git_cmd = 1;
	if (capture_command(&cp, &out, 6))
		return 0;

	strbuf_trim(&out);
	ret = !strcmp("true", out.buf);
	strbuf_release(&out);
	return ret;
}

static int apply_autostash(void)
{
	warning("TODO");
	return 0;
}

struct rebase_options {
	enum rebase_type type;
	const char *state_dir;
	struct commit *upstream;
	const char *upstream_name;
	char *head_name;
	struct object_id orig_head;
	struct commit *onto;
	const char *onto_name;
	const char *revisions;
	const char *root;
};

static int finish_rebase(struct rebase_options *opts)
{
	struct strbuf dir = STRBUF_INIT;
	const char *argv_gc_auto[] = { "gc", "--auto", NULL };

	delete_ref(NULL, "REBASE_HEAD", NULL, REF_NO_DEREF);
	apply_autostash();
	close_all_packs(the_repository->objects);
	/*
	 * We ignore errors in 'gc --auto', since the
	 * user should see them.
	 */
	run_command_v_opt(argv_gc_auto, RUN_GIT_CMD);
	strbuf_addstr(&dir, opts->state_dir);
	remove_dir_recursively(&dir, 0);
	strbuf_release(&dir);

	return 0;
}

static struct commit *peel_committish(const char *name)
{
	struct object *obj;
	struct object_id oid;

	if (get_oid(name, &oid))
		return NULL;
	obj = parse_object(the_repository, &oid);
	return (struct commit *)peel_to_type(name, 0, obj, OBJ_COMMIT);
}

static void add_var(struct strbuf *buf, const char *name, const char *value)
{
	strbuf_addstr(buf, name);
	strbuf_addstr(buf, "=");
	sq_quote_buf(buf, value);
	strbuf_addstr(buf, "; ");
}

static int run_specific_rebase(struct rebase_options *opts)
{
	const char *argv[] = { NULL, NULL };
	struct strbuf script_snippet = STRBUF_INIT;
	int status;
	const char *backend, *backend_func;

	add_var(&script_snippet, "GIT_DIR", absolute_path(get_git_dir()));

	add_var(&script_snippet, "upstream_name", opts->upstream_name);
	add_var(&script_snippet, "upstream",
				 oid_to_hex(&opts->upstream->object.oid));
	add_var(&script_snippet, "head_name", opts->head_name);
	add_var(&script_snippet, "orig_head", oid_to_hex(&opts->orig_head));
	add_var(&script_snippet, "onto", oid_to_hex(&opts->onto->object.oid));
	add_var(&script_snippet, "onto_name", opts->onto_name);
	add_var(&script_snippet, "revisions", opts->revisions);

	switch (opts->type) {
	case REBASE_AM:
		backend = "git-rebase--am";
		backend_func = "git_rebase__am";
		break;
	case REBASE_INTERACTIVE:
		backend = "git-rebase--interactive";
		backend_func = "git_rebase__interactive";
		break;
	case REBASE_MERGE:
		backend = "git-rebase--merge";
		backend_func = "git_rebase__merge";
		break;
	case REBASE_PRESERVE_MERGES:
		backend = "git-rebase--preserve-merges";
		backend_func = "git_rebase__preserve_merges";
		break;
	default:
		BUG("Unhandled rebase type %d", opts->type);
		break;
	}

	strbuf_addf(&script_snippet,
		    ". git-rebase--common && . %s && %s",
		    backend, backend_func);
	argv[0] = script_snippet.buf;

	status = run_command_v_opt(argv, RUN_USING_SHELL);
	if (status == 0)
		finish_rebase(opts);
	else if (status == 2) {
		struct strbuf dir = STRBUF_INIT;

		apply_autostash();
		strbuf_addstr(&dir, opts->state_dir);
		remove_dir_recursively(&dir, 0);
		strbuf_release(&dir);
		die("Nothing to do");
	}

	strbuf_release(&script_snippet);

	return status ? -1 : 0;
}

int cmd_rebase(int argc, const char **argv, const char *prefix)
{
	struct rebase_options options = { -1 };
	const char *branch_name;
	int ret, flags, quiet = 0;
	struct strbuf msg = STRBUF_INIT;
	struct strbuf revisions = STRBUF_INIT;
	const char *restrict_revision = NULL;

	/*
	 * NEEDSWORK: Once the builtin rebase has been tested enough
	 * and git-legacy-rebase.sh is retired to contrib/, this preamble
	 * can be removed.
	 */

	if (!use_builtin_rebase()) {
		const char *path = mkpath("%s/git-legacy-rebase",
					  git_exec_path());

		if (sane_execvp(path, (char **)argv) < 0)
			die_errno("could not exec %s", path);
		else
			die("sane_execvp() returned???");
	}

	if (argc != 2)
		die("Usage: %s <base>", argv[0]);
	prefix = setup_git_directory();
	trace_repo_setup(prefix);
	setup_work_tree();

	options.type = REBASE_AM;

	switch (options.type) {
	case REBASE_AM:
		options.state_dir = apply_dir();
		break;
	case REBASE_MERGE:
	case REBASE_INTERACTIVE:
	case REBASE_PRESERVE_MERGES:
		options.state_dir = merge_dir();
		break;
	}
	if (!options.root) {
		if (argc != 2)
			die("TODO: handle @{upstream}");
		else {
			options.upstream_name = argv[1];
			argc--;
			argv++;
			if (!strcmp(options.upstream_name, "-"))
				options.upstream_name = "@{-1}";
		}
		options.upstream = peel_committish(options.upstream_name);
		if (!options.upstream)
			die(_("invalid upstream '%s'"), options.upstream_name);
	} else
		die("TODO: upstream for --root");

	/* Make sure the branch to rebase onto is valid. */
	if (!options.onto_name)
		options.onto_name = options.upstream_name;
	if (strstr(options.onto_name, "...")) {
		die("TODO");
	} else {
		options.onto = peel_committish(options.onto_name);
		if (!options.onto)
			die(_("Does not point to a valid commit '%s'"),
				options.onto_name);
	}

	/*
	 * If the branch to rebase is given, that is the branch we will rebase
	 * branch_name -- branch/commit being rebased, or
	 * 		  HEAD (already detached)
	 * orig_head -- commit object name of tip of the branch before rebasing
	 * head_name -- refs/heads/<that-branch> or "detached HEAD"
	 */
	if (argc > 1)
		 die("TODO: handle switch_to");
	else {
		/* Do not need to switch branches, we are already on it. */
		options.head_name =
			xstrdup_or_null(resolve_ref_unsafe("HEAD", 0, NULL,
					 &flags));
		if (!options.head_name)
			die(_("No such ref: %s"), "HEAD");
		if (flags & REF_ISSYMREF) {
			if (!skip_prefix(options.head_name,
					 "refs/heads/", &branch_name))
				branch_name = options.head_name;

		} else {
			options.head_name = xstrdup("detached HEAD");
			branch_name = "HEAD";
		}
		if (get_oid("HEAD", &options.orig_head))
			die(_("Could not resolve HEAD to a revision"));
	}

	/* Detach HEAD and reset the tree */
	if (!quiet)
		printf("First, rewinding head to replay your work on top of it...");

	strbuf_addf(&msg, "rebase: checkout %s", options.onto_name);
	if (detach_head_to(&options.onto->object.oid, "checkout", msg.buf))
		die(_("Could not detach HEAD"));
	strbuf_release(&msg);
	if (update_ref("rebase", "ORIG_HEAD", &options.orig_head, NULL, 0,
		       UPDATE_REFS_MSG_ON_ERR) < 0)
		die(_("Could not update ORIG_HEAD to '%s'"),
		    oid_to_hex(&options.orig_head));

	strbuf_addf(&revisions, "%s..%s",
		!options.root ? oid_to_hex(&options.onto->object.oid) :
		    (restrict_revision ? restrict_revision :
			    oid_to_hex(&options.upstream->object.oid)),
			    oid_to_hex(&options.orig_head));

	options.revisions = revisions.buf;

	ret = !!run_specific_rebase(&options);

	strbuf_release(&revisions);
	free(options.head_name);
	return ret;
}
