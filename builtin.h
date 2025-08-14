#ifndef BUILTIN_H
#define BUILTIN_H

#include "git-compat-util.h"
#include "repository.h"



extern const char git_usage_string[];
extern const char git_more_info_string[];

void setup_auto_pager(const char *cmd, int def);

int is_builtin(const char *s);


#define BUG_ON_NON_EMPTY_PREFIX(prefix) do { \
	if ((prefix)) \
		BUG("unexpected prefix in builtin: %s", (prefix)); \
} while (0)

int cmd_add(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_am(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_annotate(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_apply(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_archive(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_backfill(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_bisect(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_blame(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_branch(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_bugreport(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_bundle(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_cat_file(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_checkout(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_checkout__worker(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_checkout_index(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_check_attr(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_check_ignore(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_check_mailmap(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_check_ref_format(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_cherry(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_cherry_pick(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_clone(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_clean(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_column(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_commit(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_commit_graph(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_commit_tree(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_config(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_count_objects(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_credential(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_credential_cache(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_credential_cache_daemon(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_credential_store(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_describe(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_diagnose(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_diff_files(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_diff_index(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_diff(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_diff_pairs(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_diff_tree(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_difftool(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_env__helper(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_fast_export(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_fast_import(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_fetch(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_fetch_pack(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_fmt_merge_msg(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_for_each_ref(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_for_each_repo(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_format_patch(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_fsck(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_fsmonitor__daemon(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_gc(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_get_tar_commit_id(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_grep(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_hash_object(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_help(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_hook(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_index_pack(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_init_db(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_interpret_trailers(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_log_reflog(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_log(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_ls_files(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_ls_tree(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_ls_remote(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_mailinfo(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_mailsplit(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_maintenance(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_merge(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_merge_base(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_merge_index(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_merge_ours(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_merge_file(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_merge_recursive(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_merge_tree(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_mktag(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_mktree(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_multi_pack_index(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_mv(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_name_rev(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_notes(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_pack_objects(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_pack_redundant(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_patch_id(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_prune(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_prune_packed(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_psuh(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_pull(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_push(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_range_diff(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_read_tree(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_rebase(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_rebase__interactive(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_receive_pack(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_reflog(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_refs(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_remote(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_remote_ext(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_remote_fd(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_repack(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_replay(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_rerere(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_reset(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_restore(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_rev_list(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_rev_parse(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_revert(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_rm(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_send_pack(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_shortlog(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_show(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_show_branch(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_show_index(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_sparse_checkout(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_status(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_stash(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_stripspace(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_submodule__helper(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_switch(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_symbolic_ref(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_tag(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_unpack_file(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_unpack_objects(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_update_index(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_update_ref(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_update_server_info(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_upload_archive(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_upload_archive_writer(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_upload_pack(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_var(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_verify_commit(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_verify_tag(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_version(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_whatchanged(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_worktree(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_write_tree(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_verify_pack(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_show_ref(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_pack_refs(int argc, const char **argv, const char *prefix, struct repository *repo);
int cmd_replace(int argc, const char **argv, const char *prefix, struct repository *repo);

#endif
