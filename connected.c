#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "commit.h"
#include "commit-reach.h"
#include "config.h"
#include "gettext.h"
#include "hex.h"
#include "odb.h"
#include "oid-array.h"
#include "replace-object.h"
#include "run-command.h"
#include "sigchain.h"
#include "connected.h"
#include "strbuf.h"
#include "tag.h"
#include "trace2.h"
#include "transport.h"
#include "packfile.h"
#include "promisor-remote.h"
#include "tree-walk.h"
#include "tree.h"
#include "refs.h"

static int promised_object_cb(const struct object_id *oid UNUSED,
			      struct object_info *oi UNUSED,
			      void *payload)
{
	bool *found = payload;
	*found = true;
	return 1;
}

/*
 * For partial clones, we don't want to have to do a regular connectivity check
 * because we have to enumerate and exclude all promisor objects (slow), and
 * then the connectivity check itself becomes a no-op because in a partial
 * clone every object is a promisor object. Instead, just make sure we
 * received, in a promisor packfile, the objects pointed to by each wanted ref.
 *
 * Before checking for promisor packs, be sure we have the latest pack-files
 * loaded into memory.
 *
 * Returns 1 when all object IDs have been found in promisor packs, in which
 * case we're fully connected and thus done. Returns 0 when we have found
 * objects in non-promisor packs, in which case we'll have to fall back to the
 * rev-list-based connectivity checks. Returns a negative error code on error.
 */
static int check_connected_promisor(oid_iterate_fn fn,
				    void *cb_data,
				    const struct object_id **oid)
{
	struct odb_for_each_object_options opts = {
		.flags = ODB_FOR_EACH_OBJECT_PROMISOR_ONLY,
		.prefix_hex_len = the_repository->hash_algo->hexsz,
	};
	int err;

	odb_reprepare(the_repository->objects);
	do {
		bool found = false;

		opts.prefix = *oid;

		err = odb_for_each_object_ext(the_repository->objects, NULL,
					      promised_object_cb, &found, &opts);
		if (err < 0)
			return err;

		/*
		 * We have found an object that is not part of a promisor pack,
		 * and thus we cannot skip the full connectivity check.
		 */
		if (!found)
			return 0;
	} while ((*oid = fn(cb_data)) != NULL);

	return 1;
}

/*
 * If index-pack already verified that the new pack is self-contained
 * (no dangling pointers), return the pack so tips found in it can
 * skip connectivity checking.
 */
static struct packed_git *get_self_contained_pack(struct transport *transport)
{
	size_t base_len;

	if (transport && transport->smart_options &&
	    transport->smart_options->self_contained_and_connected &&
	    transport->pack_lockfiles.nr == 1 &&
	    strip_suffix(transport->pack_lockfiles.items[0].string,
			 ".keep", &base_len)) {
		struct strbuf idx_file = STRBUF_INIT;
		struct packed_git *pack;

		strbuf_add(&idx_file,
			   transport->pack_lockfiles.items[0].string,
			   base_len);
		strbuf_addstr(&idx_file, ".idx");
		pack = add_packed_git(the_repository, idx_file.buf,
				      idx_file.len, 1);
		strbuf_release(&idx_file);
		return pack;
	}
	return NULL;
}

/*
 * Incremental connectivity verification.
 *
 * Instead of a full rev-list --objects traversal, verify each new
 * commit's tree by walking its entries and skipping any that were
 * previously verified:
 *
 *  - Before walking a commit's tree, the top-level entries from each
 *    direct parent tree are added to the verified set.  Matching
 *    entries in the child are skipped, since the entire subtree is
 *    already verified.
 *  - Only new or changed entries cause recursive descent and blob
 *    verification.
 *  - The verified set persists across commits, letting us skip
 *    subtrees seen earlier (change-then-revert, subtree moves,
 *    merges).
 */

struct loaded_tree {
	struct tree_desc desc;
	void *buf;
};

/* Read a tree without triggering lazy promisor fetches. */
static int read_tree_nofetch(struct loaded_tree *pt,
			     const struct object_id *oid,
			     enum object_type *actual_type)
{
	enum object_type type;
	size_t size;
	struct object_info oi = OBJECT_INFO_INIT;

	oi.typep = &type;
	oi.sizep = &size;
	oi.contentp = &pt->buf;
	if (odb_read_object_info_extended(the_repository->objects, oid, &oi,
					  OBJECT_INFO_SKIP_FETCH_OBJECT |
					  OBJECT_INFO_DIE_IF_CORRUPT |
					  OBJECT_INFO_LOOKUP_REPLACE) < 0) {
		if (actual_type)
			*actual_type = OBJ_NONE;
		return -1;
	}
	if (type != OBJ_TREE) {
		FREE_AND_NULL(pt->buf);
		if (actual_type)
			*actual_type = type;
		return -1;
	}
	init_tree_desc(&pt->desc, oid, pt->buf, size);
	return 0;
}

static int tree_seek(struct tree_desc *desc,
		     const struct name_entry *want)
{
	while (desc->size) {
		const struct name_entry *have = &desc->entry;
		int cmp = base_name_compare(
				have->path, have->pathlen, have->mode,
				want->path, want->pathlen, want->mode);
		if (cmp > 0)
			return 0;
		if (cmp == 0)
			return 1;
		update_tree_entry(desc);
	}
	return 0;
}

struct verify_state {
	struct oidset verified_trees;
	struct oidset verified_blobs;
	int trees_walked;
	int blobs_checked;
	int err_fd;
	int quiet;
};

__attribute__((format (printf, 2, 3)))
static void verify_error(struct verify_state *vs, const char *fmt, ...)
{
	va_list ap;
	struct strbuf buf = STRBUF_INIT;

	if (vs->quiet && !vs->err_fd)
		return;

	if (vs->err_fd) {
		strbuf_addstr(&buf, "error: ");
		va_start(ap, fmt);
		strbuf_vaddf(&buf, fmt, ap);
		va_end(ap);
		strbuf_addch(&buf, '\n');
		sigchain_push(SIGPIPE, SIG_IGN);
		write_in_full(vs->err_fd, buf.buf, buf.len);
		sigchain_pop(SIGPIPE);
	} else {
		va_start(ap, fmt);
		strbuf_vaddf(&buf, fmt, ap);
		va_end(ap);
		error("%s", buf.buf);
	}
	strbuf_release(&buf);
}

static int verify_tree(const struct object_id *new_tree_oid,
		       const struct oid_array *base_trees,
		       struct verify_state *vs, int depth);

static int verify_subtree(const struct name_entry *entry,
			  struct loaded_tree *parents,
			  size_t nr_parents,
			  struct verify_state *vs, int depth)
{
	struct oid_array sub_bases = OID_ARRAY_INIT;
	size_t i;
	int ret;

	/*
	 * tree_seek() advances each parent desc destructively.
	 * This is safe because both sides are in canonical sort
	 * order and verify_tree() calls us in that same order.
	 */
	for (i = 0; i < nr_parents; i++) {
		if (!tree_seek(&parents[i].desc, entry))
			continue;
		if (S_ISDIR(parents[i].desc.entry.mode))
			oid_array_append(&sub_bases,
					 &parents[i].desc.entry.oid);
	}

	ret = verify_tree(&entry->oid, &sub_bases, vs, depth + 1);
	oid_array_clear(&sub_bases);
	return ret;
}

static int verify_tree(const struct object_id *new_tree_oid,
		       const struct oid_array *base_trees,
		       struct verify_state *vs, int depth)
{
	struct loaded_tree new_tree = { 0 };
	struct loaded_tree *parents = NULL;
	struct tree_desc scan;
	struct name_entry entry, scan_entry;
	enum object_type type;
	struct object_info oi = OBJECT_INFO_INIT;
	size_t nr_parents = 0;
	size_t i;
	int ret = 0;

	if (depth > the_repository->settings.max_allowed_tree_depth) {
		verify_error(vs, _("exceeded maximum allowed tree depth"));
		return -1;
	}

	if (oidset_contains(&vs->verified_trees, new_tree_oid))
		return 0;

	if (read_tree_nofetch(&new_tree, new_tree_oid, &type)) {
		if (is_promisor_object(the_repository, new_tree_oid)) {
			oidset_insert(&vs->verified_trees, new_tree_oid);
			return 0;
		}
		if (type != OBJ_NONE)
			verify_error(vs, _("object %s is a %s, not a tree"),
				     oid_to_hex(new_tree_oid),
				     type_name(type));
		else
			verify_error(vs, _("bad tree object %s"),
				     oid_to_hex(new_tree_oid));
		return -1;
	}
	vs->trees_walked++;

	if (base_trees->nr)
		CALLOC_ARRAY(parents, base_trees->nr);
	for (i = 0; i < base_trees->nr; i++) {
		if (read_tree_nofetch(&parents[nr_parents], &base_trees->oid[i], NULL))
			continue;
		scan = parents[nr_parents].desc;
		while (tree_entry(&scan, &scan_entry)) {
			if (S_ISGITLINK(scan_entry.mode))
				continue;
			if (S_ISDIR(scan_entry.mode))
				oidset_insert(&vs->verified_trees, &scan_entry.oid);
			else
				oidset_insert(&vs->verified_blobs, &scan_entry.oid);
		}
		nr_parents++;
	}

	while (tree_entry(&new_tree.desc, &entry)) {
		if (S_ISGITLINK(entry.mode))
			continue;

		if (S_ISDIR(entry.mode)) {
			if (oidset_contains(&vs->verified_trees, &entry.oid))
				continue;
			ret = verify_subtree(&entry, parents, nr_parents,
					     vs, depth);
			if (ret)
				break;
			oidset_insert(&vs->verified_trees, &entry.oid);
			continue;
		}

		if (oidset_contains(&vs->verified_blobs, &entry.oid))
			continue;
		vs->blobs_checked++;

		oi.typep = &type;
		if (odb_read_object_info_extended(
				the_repository->objects, &entry.oid, &oi,
				OBJECT_INFO_SKIP_FETCH_OBJECT | OBJECT_INFO_LOOKUP_REPLACE) < 0) {
			if (is_promisor_object(the_repository, &entry.oid)) {
				oidset_insert(&vs->verified_blobs, &entry.oid);
				continue;
			}
			verify_error(vs, _("missing blob object '%s'"),
				     oid_to_hex(&entry.oid));
			ret = -1;
			break;
		}
		if (type != OBJ_BLOB) {
			verify_error(vs, _("object %s is a %s, not a blob"),
				     oid_to_hex(&entry.oid),
				     type_name(type));
			ret = -1;
			break;
		}
		oidset_insert(&vs->verified_blobs, &entry.oid);
	}

	if (!ret)
		oidset_insert(&vs->verified_trees, new_tree_oid);
	free(new_tree.buf);
	for (i = 0; i < nr_parents; i++)
		free(parents[i].buf);
	free(parents);
	return ret;
}

/* Read a tag's target OID without triggering lazy promisor fetches. */
static int read_tag_target_nofetch(const struct object_id *tag_oid,
				   struct object_id *target)
{
	enum object_type type;
	size_t size;
	void *buf;
	struct object_info oi = OBJECT_INFO_INIT;
	struct object *obj;
	int eaten;

	oi.typep = &type;
	oi.sizep = &size;
	oi.contentp = &buf;
	if (odb_read_object_info_extended(
			the_repository->objects, tag_oid, &oi,
			OBJECT_INFO_SKIP_FETCH_OBJECT | OBJECT_INFO_DIE_IF_CORRUPT |
			OBJECT_INFO_LOOKUP_REPLACE) < 0)
		return -1;
	if (type != OBJ_TAG) {
		free(buf);
		return -1;
	}

	obj = parse_object_buffer(the_repository, tag_oid, type,
				  (unsigned long)size, buf, &eaten);
	if (!eaten)
		free(buf);
	if (!obj || obj->type != OBJ_TAG || !((struct tag *)obj)->tagged)
		return -1;

	oidcpy(target, get_tagged_oid((struct tag *)obj));
	return 0;
}

/* Peel tags without triggering lazy promisor fetches. */
enum peel_nofetch_result {
	PEEL_NOFETCH_OK = 0,
	PEEL_NOFETCH_PROMISOR = 1,
	PEEL_NOFETCH_ERROR = -1,
};

static enum peel_nofetch_result peel_to_non_tag_nofetch(struct object_id *oid,
							enum object_type *type,
							struct verify_state *vs)
{
	struct object_info oi = OBJECT_INFO_INIT;
	struct object_id target;

	oi.typep = type;
	for (;;) {
		if (odb_read_object_info_extended(
				the_repository->objects, oid, &oi,
				OBJECT_INFO_SKIP_FETCH_OBJECT | OBJECT_INFO_LOOKUP_REPLACE) < 0) {
			if (is_promisor_object(the_repository, oid))
				return PEEL_NOFETCH_PROMISOR;
			verify_error(vs, _("unable to read object %s"),
				     oid_to_hex(oid));
			return PEEL_NOFETCH_ERROR;
		}
		if (*type != OBJ_TAG)
			return PEEL_NOFETCH_OK;
		if (read_tag_target_nofetch(oid, &target)) {
			verify_error(vs, _("unable to peel tag %s"),
				     oid_to_hex(oid));
			return PEEL_NOFETCH_ERROR;
		}
		oidcpy(oid, &target);
	}
}

/*
 * Consume the tip iterator, peel all tags, and verify non-commit
 * objects immediately.  Returns commit OIDs in commit_tips for
 * boundary finding.  Tips already in the self-contained pack
 * (verified by index-pack) are skipped.
 */
static int collect_and_peel_tips(const struct object_id *oid,
				 oid_iterate_fn fn, void *cb_data,
				 struct transport *transport,
				 struct oid_array *commit_tips,
				 struct verify_state *vs)
{
	struct packed_git *new_pack = get_self_contained_pack(transport);
	int err = 0;

	do {
		struct object_id peeled;
		enum object_type type;
		enum peel_nofetch_result peel_ret;

		if (new_pack && find_pack_entry_one(oid, new_pack))
			continue;

		oidcpy(&peeled, oid);
		peel_ret = peel_to_non_tag_nofetch(&peeled, &type, vs);
		if (peel_ret == PEEL_NOFETCH_PROMISOR)
			continue;
		if (peel_ret == PEEL_NOFETCH_ERROR) {
			err = -1;
			break;
		}

		switch (type) {
		case OBJ_COMMIT:
			oid_array_append(commit_tips, &peeled);
			break;
		case OBJ_TREE: {
			const struct oid_array empty = OID_ARRAY_INIT;
			err = verify_tree(&peeled, &empty, vs, 0);
			break;
		}
		case OBJ_BLOB:
			/* existence confirmed by peel step */
			break;
		default:
			verify_error(vs, _("unknown object type %d for %s"),
				     type, oid_to_hex(&peeled));
			err = -1;
			break;
		}
	} while (!err && (oid = fn(cb_data)) != NULL);

	if (new_pack) {
		close_pack(new_pack);
		free(new_pack);
	}
	return err;
}

static int verify_commit_tree(struct commit *commit,
			      const struct oidset *shallow_commits,
			      struct verify_state *vs)
{
	struct oid_array base_trees = OID_ARRAY_INIT;
	struct commit_list *p;
	int ret;

	p = oidset_contains(shallow_commits, &commit->object.oid)
		? NULL : commit->parents;
	for (; p; p = p->next) {
		const struct object_id *tree_oid;
		if (repo_parse_commit_gently(the_repository, p->item, 1))
			continue;
		tree_oid = get_commit_tree_oid(p->item);
		oidset_insert(&vs->verified_trees, tree_oid);
		oid_array_append(&base_trees, tree_oid);
	}

	ret = verify_tree(get_commit_tree_oid(commit),
			  &base_trees, vs, 0);
	oid_array_clear(&base_trees);
	return ret;
}

/*
 * Walk new commits in topological order (parents before children)
 * and verify each commit's tree, skipping previously verified entries.
 *
 * Shallow commits have no parents.
 */
static int verify_new_commits(struct commit_list **new_commits,
			      const struct oidset *shallow_commits,
			      struct verify_state *vs)
{
	struct commit_list *iter;
	unsigned nr_before;
	int err = 0;

	nr_before = commit_list_count(*new_commits);
	sort_in_topological_order(new_commits, REV_SORT_IN_GRAPH_ORDER);
	/*
	 * sort_in_topological_order() uses an in-degree-based algorithm
	 * that drops commits involved in cycles; a count decrease means
	 * a cycle was present.
	 */
	if (commit_list_count(*new_commits) < nr_before) {
		verify_error(vs, _("cycle detected in incoming commit graph"));
		return -1;
	}

	*new_commits = commit_list_reverse(*new_commits);

	for (iter = *new_commits; !err && iter; iter = iter->next)
		err = verify_commit_tree(iter->item, shallow_commits, vs);

	return err;
}

/*
 * oidset_parse_file() cannot be reused because it calls die() on errors.
 */
static int parse_shallow_file_gently(const char *path,
				     struct oidset *shallow_commits,
				     struct verify_state *vs)
{
	FILE *fp;
	struct strbuf line = STRBUF_INIT;
	struct object_id oid;
	int err = 0;

	fp = fopen(path, "r");
	if (!fp) {
		verify_error(vs, _("unable to open shallow file '%s': %s"),
			     path, strerror(errno));
		return -1;
	}
	while (strbuf_getline(&line, fp) != EOF) {
		const char *end;
		if (parse_oid_hex(line.buf, &oid, &end) || *end) {
			verify_error(vs, _("bad shallow line: %s"), line.buf);
			err = -1;
			break;
		}
		oidset_insert(shallow_commits, &oid);
	}
	fclose(fp);
	strbuf_release(&line);
	return err;
}

/* TODO: make seed refs configurable (e.g. transfer.connectivitySeedRefs) */
static const char *boundary_seed_refs[] = {
	"HEAD",
	"refs/remotes/origin/HEAD",
	"refs/remotes/origin/master",
};

static int find_boundary_from_commit_graph(struct oid_array *commit_tips,
					   struct oid_array *old_tips,
					   struct commit_list **new_commits)
{
	struct commit **bases = NULL;
	size_t nr_bases = 0;
	size_t alloc_bases;
	struct commit **tips;
	int ret;
	size_t i;

	alloc_bases = ARRAY_SIZE(boundary_seed_refs) + old_tips->nr;
	ALLOC_ARRAY(bases, alloc_bases);

	for (i = 0; i < old_tips->nr; i++) {
		struct commit *c;
		c = lookup_commit(the_repository, &old_tips->oid[i]);
		if (!c || repo_parse_commit(the_repository, c))
			continue;
		bases[nr_bases++] = c;
	}

	for (i = 0; i < ARRAY_SIZE(boundary_seed_refs); i++) {
		struct object_id oid;
		struct commit *c;
		if (!refs_resolve_ref_unsafe(get_main_ref_store(the_repository),
					     boundary_seed_refs[i],
					     RESOLVE_REF_READING, &oid, NULL))
			continue;
		c = lookup_commit(the_repository, &oid);
		if (!c || repo_parse_commit(the_repository, c))
			continue;
		bases[nr_bases++] = c;
	}
	if (!nr_bases) {
		free(bases);
		return -1;
	}

	ALLOC_ARRAY(tips, commit_tips->nr);
	for (i = 0; i < commit_tips->nr; i++) {
		tips[i] = lookup_commit(the_repository, &commit_tips->oid[i]);
		if (!tips[i] || repo_parse_commit(the_repository, tips[i])) {
			free(tips);
			free(bases);
			return -1;
		}
	}

	ret = repo_find_boundary_commits(the_repository,
					 bases, nr_bases,
					 commit_tips->nr, tips, new_commits);
	free(tips);
	free(bases);
	return ret;
}

/*
 * Find the connectivity boundary: the set of new commits not yet
 * reachable from local refs.  Feeds commit_tips to rev-list via
 * stdin, collects output commit OIDs into new_commits.
 */
static int find_connectivity_boundary(struct check_connected_options *opt,
				      struct oid_array *commit_tips,
				      struct commit_list **new_commits,
				      struct verify_state *vs)
{
	struct child_process rev_list = CHILD_PROCESS_INIT;
	FILE *rev_list_in;
	FILE *rev_list_out;
	struct strbuf line = STRBUF_INIT;
	int err = 0;
	size_t i;

	if (opt->shallow_file) {
		strvec_push(&rev_list.args, "--shallow-file");
		strvec_push(&rev_list.args, opt->shallow_file);
	}
	strvec_push(&rev_list.args, "rev-list");
	strvec_push(&rev_list.args, "--stdin");
	if (repo_has_promisor_remote(the_repository))
		strvec_push(&rev_list.args, "--exclude-promisor-objects");
	if (!opt->is_deepening_fetch) {
		strvec_push(&rev_list.args, "--not");
		if (opt->exclude_hidden_refs_section)
			strvec_pushf(&rev_list.args, "--exclude-hidden=%s",
				     opt->exclude_hidden_refs_section);
		strvec_push(&rev_list.args, "--all");
	}
	strvec_push(&rev_list.args, "--alternate-refs");
	if (opt->progress)
		strvec_pushf(&rev_list.args, "--progress=%s",
			     _("Finding connectivity boundary"));

	rev_list.git_cmd = 1;
	if (opt->env)
		strvec_pushv(&rev_list.env, opt->env);
	rev_list.in = -1;
	rev_list.out = -1;
	if (vs->err_fd) {
		int fd = dup(vs->err_fd);
		if (fd < 0)
			return error_errno(_("could not duplicate error fd"));
		rev_list.err = fd;
	} else {
		rev_list.no_stderr = opt->quiet;
	}

	if (start_command(&rev_list))
		return error(_("could not run 'git rev-list'"));

	sigchain_push(SIGPIPE, SIG_IGN);

	/*
	 * rev-list --stdin consumes all input revisions before starting
	 * the revision walk, so it cannot fill stdout while we feed
	 * stdin.  Write-then-read is safe here despite both pipes.
	 */
	rev_list_in = xfdopen(rev_list.in, "w");

	for (i = 0; i < commit_tips->nr; i++) {
		if (fprintf(rev_list_in, "%s\n",
			    oid_to_hex(&commit_tips->oid[i])) < 0)
			break;
	}

	if (ferror(rev_list_in) || fflush(rev_list_in)) {
		if (errno != EPIPE && errno != EINVAL)
			error_errno(_("failed write to rev-list"));
		err = -1;
	}
	if (fclose(rev_list_in))
		err = error_errno(_("failed to close rev-list's stdin"));

	rev_list_out = xfdopen(rev_list.out, "r");

	while (!err && strbuf_getline(&line, rev_list_out) != EOF) {
		struct object_id commit_oid;
		struct commit *commit;
		const char *end;

		if (parse_oid_hex(line.buf, &commit_oid, &end) || *end) {
			verify_error(vs,
				     _("bad rev-list output: %s"), line.buf);
			err = -1;
			break;
		}

		commit = lookup_commit(the_repository, &commit_oid);
		if (!commit || repo_parse_commit_gently(the_repository,
							commit, 1)) {
			verify_error(vs, _("unable to parse commit %s"),
				     oid_to_hex(&commit_oid));
			err = -1;
			break;
		}

		commit_list_insert(commit, new_commits);
	}

	strbuf_release(&line);
	fclose(rev_list_out);
	sigchain_pop(SIGPIPE);

	if (finish_command(&rev_list))
		err = -1;

	if (err) {
		commit_list_free(*new_commits);
		*new_commits = NULL;
	}

	return err;
}

/*
 * Collect tips, find the connectivity boundary via rev-list,
 * then verify new commits' trees.
 */
static int check_connected_incremental(oid_iterate_fn fn, void *cb_data,
				       struct check_connected_options *opt,
				       const struct object_id *oid)
{
	struct verify_state vs = { 0 };
	struct commit_list *new_commits = NULL;
	struct oidset shallow_commits = OIDSET_INIT;
	struct oid_array commit_tips = OID_ARRAY_INIT;
	int err = 0;

	vs.quiet = opt->quiet;
	vs.err_fd = opt->err_fd;

	trace2_region_enter("connectivity", "incremental", the_repository);

	if (opt->shallow_file && *opt->shallow_file) {
		err = parse_shallow_file_gently(opt->shallow_file,
						&shallow_commits, &vs);
		if (err)
			goto done;
	}

	trace2_region_enter("connectivity", "collect-tips", the_repository);
	err = collect_and_peel_tips(oid, fn, cb_data, opt->transport,
				    &commit_tips, &vs);
	trace2_region_leave("connectivity", "collect-tips", the_repository);

	trace2_region_enter("connectivity", "find-boundary", the_repository);
	if (!err && find_boundary_from_commit_graph(&commit_tips, &opt->old_tips,
						    &new_commits))
		err = find_connectivity_boundary(opt, &commit_tips,
						 &new_commits, &vs);
	trace2_region_leave("connectivity", "find-boundary", the_repository);

	trace2_region_enter("connectivity", "verify-new-commits", the_repository);
	if (!err)
		err = verify_new_commits(&new_commits, &shallow_commits, &vs);
	trace2_region_leave("connectivity", "verify-new-commits", the_repository);

done:
	if (vs.err_fd)
		close(vs.err_fd);
	commit_list_free(new_commits);
	oidset_clear(&shallow_commits);
	oid_array_clear(&commit_tips);
	oidset_clear(&vs.verified_trees);
	oidset_clear(&vs.verified_blobs);
	trace2_data_intmax("connectivity", the_repository,
			   "trees_walked", vs.trees_walked);
	trace2_data_intmax("connectivity", the_repository,
			   "blobs_checked", vs.blobs_checked);
	trace2_region_leave("connectivity", "incremental", the_repository);

	return err;
}

static int incremental_check_applicable(struct check_connected_options *opt)
{
	const char *algorithm = NULL;

	if (repo_config_get_string_tmp(the_repository,
				       "transfer.connectivitycheck",
				       &algorithm))
		return 0;
	if (!strcasecmp(algorithm, "rev-list"))
		return 0;
	if (strcasecmp(algorithm, "incremental"))
		die(_("unknown transfer.connectivityCheck algorithm '%s'"),
		    algorithm);

	if (opt->is_deepening_fetch)
		return 0;
	if (replace_refs_enabled(the_repository)) {
		prepare_replace_object(the_repository);
		if (oidmap_get_size(&the_repository->objects->replace_map))
			return 0;
	}

	return 1;
}

/*
 * If we feed all the commits we want to verify to this command
 *
 *  $ git rev-list --objects --stdin --not --all
 *
 * and if it does not error out, that means everything reachable from
 * these commits locally exists and is connected to our existing refs.
 * Note that this does _not_ validate the individual objects.
 *
 * Returns 0 if everything is connected, non-zero otherwise.
 */
int check_connected(oid_iterate_fn fn, void *cb_data,
		    struct check_connected_options *opt)
{
	struct child_process rev_list = CHILD_PROCESS_INIT;
	FILE *rev_list_in;
	struct check_connected_options defaults = CHECK_CONNECTED_INIT;
	const struct object_id *oid;
	int err = 0;
	struct packed_git *new_pack = NULL;
	struct transport *transport;

	if (!opt)
		opt = &defaults;
	transport = opt->transport;

	oid = fn(cb_data);
	if (!oid) {
		if (opt->err_fd)
			close(opt->err_fd);
		return err;
	}

	if (repo_has_promisor_remote(the_repository)) {
		err = check_connected_promisor(fn, cb_data, &oid);
		if (err) {
			if (opt->err_fd)
				close(opt->err_fd);
			if (err > 0)
				err = 0;
			return err;
		}
	}

	if (incremental_check_applicable(opt))
		return check_connected_incremental(fn, cb_data, opt, oid);

	if (opt->shallow_file) {
		strvec_push(&rev_list.args, "--shallow-file");
		strvec_push(&rev_list.args, opt->shallow_file);
	}
	strvec_push(&rev_list.args,"rev-list");
	strvec_push(&rev_list.args, "--objects");
	strvec_push(&rev_list.args, "--stdin");
	if (repo_has_promisor_remote(the_repository))
		strvec_push(&rev_list.args, "--exclude-promisor-objects");
	if (!opt->is_deepening_fetch) {
		strvec_push(&rev_list.args, "--not");
		if (opt->exclude_hidden_refs_section)
			strvec_pushf(&rev_list.args, "--exclude-hidden=%s",
				     opt->exclude_hidden_refs_section);
		strvec_push(&rev_list.args, "--all");
	}
	strvec_push(&rev_list.args, "--quiet");
	strvec_push(&rev_list.args, "--alternate-refs");
	if (opt->progress)
		strvec_pushf(&rev_list.args, "--progress=%s",
			     _("Checking connectivity"));

	rev_list.git_cmd = 1;
	if (opt->env)
		strvec_pushv(&rev_list.env, opt->env);
	rev_list.in = -1;
	rev_list.no_stdout = 1;
	if (opt->err_fd)
		rev_list.err = opt->err_fd;
	else
		rev_list.no_stderr = opt->quiet;

	if (start_command(&rev_list))
		return error(_("Could not run 'git rev-list'"));

	sigchain_push(SIGPIPE, SIG_IGN);

	rev_list_in = xfdopen(rev_list.in, "w");

	new_pack = get_self_contained_pack(transport);

	do {
		/*
		 * If index-pack already checked that:
		 * - there are no dangling pointers in the new pack
		 * - the pack is self contained
		 * Then if the updated ref is in the new pack, then we
		 * are sure the ref is good and not sending it to
		 * rev-list for verification.
		 */
		if (new_pack && find_pack_entry_one(oid, new_pack))
			continue;

		if (fprintf(rev_list_in, "%s\n", oid_to_hex(oid)) < 0)
			break;
	} while ((oid = fn(cb_data)) != NULL);

	if (ferror(rev_list_in) || fflush(rev_list_in)) {
		if (errno != EPIPE && errno != EINVAL)
			error_errno(_("failed write to rev-list"));
		err = -1;
	}

	if (fclose(rev_list_in))
		err = error_errno(_("failed to close rev-list's stdin"));

	sigchain_pop(SIGPIPE);
	if (new_pack) {
		close_pack(new_pack);
		free(new_pack);
	}
	return finish_command(&rev_list) || err;
}
