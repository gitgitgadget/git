#include "builtin.h"
#include "ref-filter.h"
#include "parse-options.h"
#include "metacommit.h"
#include "config.h"
#include "refs.h"

static const char *const builtin_change_usage[] = {
	N_("git change list [<pattern>...]"),
	N_("git change update [--force] [--replace <treeish>...] "
	   "[--origin <treeish>...] [--content <newtreeish>]"),
	N_("git change delete <change-name>..."),
	NULL
};

static const char *const builtin_list_usage[] = {
	N_("git change list [<pattern>...]"), NULL
};

static const char *const builtin_update_usage[] = {
	N_("git change update [--force] [--replace <treeish>...] "
	   "[--origin <treeish>...] [--content <newtreeish>]"),
	NULL
};

static const char *const builtin_delete_usage[] = {
	N_("git change delete [<pattern>...]"),
	NULL
};

static int change_list(int argc, const char **argv, const char *prefix)
{
	struct option options[] = { OPT_END() };
	struct ref_filter filter = { 0 };
	struct ref_sorting *sorting;
	struct string_list sorting_options = STRING_LIST_INIT_DUP;
	struct ref_format format = REF_FORMAT_INIT;
	struct ref_array array = { 0 };
	size_t i;

	argc = parse_options(argc, argv, prefix, options, builtin_list_usage,
			     0);

	setup_ref_filter_porcelain_msg();

	filter.kind = FILTER_REFS_CHANGES;
	filter.name_patterns = argv;

	filter_refs(&array, &filter, FILTER_REFS_CHANGES);

	if (!format.format)
		format.format = "%(refname:lstrip=1)";

	if (verify_ref_format(&format))
		die(_("unable to parse format string"));

	sorting = ref_sorting_options(&sorting_options);
	ref_array_sort(sorting, &array);

	for (i = 0; i < array.nr; i++) {
		struct strbuf output = STRBUF_INIT;
		struct strbuf err = STRBUF_INIT;
		if (format_ref_array_item(array.items[i], &format, &output,
					  &err))
			die("%s", err.buf);
		fwrite(output.buf, 1, output.len, stdout);
		putchar('\n');

		strbuf_release(&err);
		strbuf_release(&output);
	}

	ref_array_clear(&array);
	ref_sorting_release(sorting);

	return 0;
}

struct update_state {
	int options;
	const char *change;
	const char *content;
	struct string_list replace;
	struct string_list origin;
};

#define UPDATE_STATE_INIT                                             \
	{                                                             \
		.content = "HEAD", .replace = STRING_LIST_INIT_NODUP, \
		.origin = STRING_LIST_INIT_NODUP                      \
	}

static void clear_update_state(struct update_state *state)
{
	string_list_clear(&state->replace, 0);
	string_list_clear(&state->origin, 0);
}

static int update_option_parse_replace(const struct option *opt,
				       const char *arg, int unset)
{
	struct update_state *state = opt->value;
	string_list_append(&state->replace, arg);
	return 0;
}

static int update_option_parse_origin(const struct option *opt, const char *arg,
				      int unset)
{
	struct update_state *state = opt->value;
	string_list_append(&state->origin, arg);
	return 0;
}

static int resolve_commit(const char *committish, struct object_id *result)
{
	struct commit *commit;
	if (get_oid_committish(committish, result))
		die(_("failed to resolve '%s' as a valid revision."),
		    committish);
	commit = lookup_commit_reference(the_repository, result);
	if (!commit)
		die(_("could not parse object '%s'."), committish);
	oidcpy(result, &commit->object.oid);
	return 0;
}

static void resolve_commit_list(const struct string_list *commitsish_list,
				struct oid_array *result)
{
	struct string_list_item *item;

	for_each_string_list_item (item, commitsish_list) {
		struct object_id next;
		resolve_commit(item->string, &next);
		oid_array_append(result, &next);
	}
}

/*
 * Given the command-line options for the update command, fills in a
 * metacommit_data with the corresponding changes.
 */
static void
get_metacommit_from_command_line(const struct update_state *commands,
				 struct metacommit_data *result)
{
	resolve_commit(commands->content, &(result->content));
	resolve_commit_list(&(commands->replace), &(result->replace));
	resolve_commit_list(&(commands->origin), &(result->origin));
}

static int perform_update(struct repository *repo,
			  const struct update_state *state, struct strbuf *err)
{
	struct metacommit_data metacommit = METACOMMIT_DATA_INIT;
	struct string_list changes = STRING_LIST_INIT_DUP;
	int ret;
	struct string_list_item *item;

	get_metacommit_from_command_line(state, &metacommit);

	ret = record_metacommit(repo, &metacommit, state->change,
				state->options, err, &changes);

	for_each_string_list_item (item, &changes) {
		const char *name = lstrip_ref_components(item->string, 1);
		if (!name)
			die(_("failed to remove `refs/` from %s"),
			    item->string);

		if (item->util)
			fprintf(stdout, _("Updated change %s"), name);
		else
			fprintf(stdout, _("Created change %s"), name);
		putchar('\n');
	}

	string_list_clear(&changes, 0);
	clear_metacommit_data(&metacommit);

	return ret;
}

static int change_update(int argc, const char **argv, const char *prefix)
{
	int result;
	struct strbuf err = STRBUF_INIT;
	struct update_state state = UPDATE_STATE_INIT;
	struct option options[] = {
		{ OPTION_CALLBACK, 'r', "replace", &state, N_("commit"),
		  N_("marks the given commit as being obsolete"), 0,
		  update_option_parse_replace },
		{ OPTION_CALLBACK, 'o', "origin", &state, N_("commit"),
		  N_("marks the given commit as being the origin of this commit"),
		  0, update_option_parse_origin },

		OPT_STRING(
			'c', "content", &state.content, N_("commit"),
			N_("identifies the new content commit for the change")),
		OPT_STRING('g', "change", &state.change, N_("commit"),
			   N_("name of the change to update")),
		OPT_SET_INT_F(
			'n', "new", &state.options,
			N_("create a new change - do not append to any existing change"),
			UPDATE_OPTION_NOAPPEND, 0),
		OPT_SET_INT_F(
			'F', "force", &state.options,
			N_("overwrite an existing change of the same name"),
			UPDATE_OPTION_FORCE, 0),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options, builtin_update_usage,
			     0);
	result = perform_update(the_repository, &state, &err);

	if (result < 0) {
		error("%s", err.buf);
		strbuf_release(&err);
	}

	clear_update_state(&state);

	return result;
}

typedef int (*each_change_name_fn)(const char *name, const char *ref,
				   const struct object_id *oid, void *cb_data);

static int for_each_change_name(const char **argv, each_change_name_fn fn,
				void *cb_data)
{
	const char **p;
	struct strbuf ref = STRBUF_INIT;
	int had_error = 0;
	struct object_id oid;

	for (p = argv; *p; p++) {
		strbuf_reset(&ref);
		/* Convenience functionality to avoid having to type `metas/` */
		if (strncmp("metas/", *p, 5)) {
			strbuf_addf(&ref, "refs/metas/%s", *p);
		} else {
			strbuf_addf(&ref, "refs/%s", *p);
		}
		if (read_ref(ref.buf, &oid)) {
			error(_("change '%s' not found."), *p);
			had_error = 1;
			continue;
		}
		if (fn(*p, ref.buf, &oid, cb_data))
			had_error = 1;
	}
	strbuf_release(&ref);
	return had_error;
}

static int collect_changes(const char *name, const char *ref,
			   const struct object_id *oid, void *cb_data)
{
	struct string_list *ref_list = cb_data;

	string_list_append(ref_list, ref);
	ref_list->items[ref_list->nr - 1].util = oiddup(oid);
	return 0;
}

static int change_delete(int argc, const char **argv, const char* prefix) {
	int result = 0;
	struct string_list refs_to_delete = STRING_LIST_INIT_DUP;
	struct string_list_item *item;
	struct option options[] = {
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options, builtin_delete_usage, 0);

	result = for_each_change_name(argv, collect_changes, (void *)&refs_to_delete);
	if (delete_refs(NULL, &refs_to_delete, REF_NO_DEREF))
		result = 1;

	for_each_string_list_item(item, &refs_to_delete) {
		const char *name = item->string;
		struct object_id *oid = item->util;
		if (!ref_exists(name))
			printf(_("Deleted change '%s' (was %s)\n"),
				item->string + 5,
				find_unique_abbrev(oid, DEFAULT_ABBREV));

		free(oid);
	}
	string_list_clear(&refs_to_delete, 0);
	return result;
}

int cmd_change(int argc, const char **argv, const char *prefix)
{
	parse_opt_subcommand_fn *fn = NULL;
	/* No options permitted before subcommand currently */
	struct option options[] = {
		OPT_SUBCOMMAND("list", &fn, change_list),
		OPT_SUBCOMMAND("update", &fn, change_update),
		OPT_SUBCOMMAND("delete", &fn, change_delete),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options, builtin_change_usage,
			     PARSE_OPT_SUBCOMMAND_OPTIONAL);

	if (!fn) {
		if (argc) {
			error(_("unknown subcommand: `%s'"), argv[0]);
			usage_with_options(builtin_change_usage, options);
		}
		fn = change_list;
	}

	return !!fn(argc, argv, prefix);
}
