#define USE_THE_REPOSITORY_VARIABLE

#include "builtin.h"
#include "environment.h"
#include "parse-options.h"
#include "path-walk.h"
#include "quote.h"
#include "ref-filter.h"
#include "refs.h"
#include "revision.h"
#include "strbuf.h"
#include "string-list.h"
#include "shallow.h"

static const char *const repo_usage[] = {
	"git repo info [--format=(keyvalue|nul)] [-z] [<key>...]",
	"git repo stats [--format=(table|keyvalue|nul)]",
	NULL
};

typedef int get_value_fn(struct repository *repo, struct strbuf *buf);

enum output_format {
	FORMAT_TABLE,
	FORMAT_KEYVALUE,
	FORMAT_NUL_TERMINATED,
};

struct field {
	const char *key;
	get_value_fn *get_value;
};

static int get_layout_bare(struct repository *repo UNUSED, struct strbuf *buf)
{
	strbuf_addstr(buf, is_bare_repository() ? "true" : "false");
	return 0;
}

static int get_layout_shallow(struct repository *repo, struct strbuf *buf)
{
	strbuf_addstr(buf,
		      is_repository_shallow(repo) ? "true" : "false");
	return 0;
}

static int get_object_format(struct repository *repo, struct strbuf *buf)
{
	strbuf_addstr(buf, repo->hash_algo->name);
	return 0;
}

static int get_references_format(struct repository *repo, struct strbuf *buf)
{
	strbuf_addstr(buf,
		      ref_storage_format_to_name(repo->ref_storage_format));
	return 0;
}

/* repo_info_fields keys must be in lexicographical order */
static const struct field repo_info_fields[] = {
	{ "layout.bare", get_layout_bare },
	{ "layout.shallow", get_layout_shallow },
	{ "object.format", get_object_format },
	{ "references.format", get_references_format },
};

static int repo_info_fields_cmp(const void *va, const void *vb)
{
	const struct field *a = va;
	const struct field *b = vb;

	return strcmp(a->key, b->key);
}

static get_value_fn *get_value_fn_for_key(const char *key)
{
	const struct field search_key = { key, NULL };
	const struct field *found = bsearch(&search_key, repo_info_fields,
					    ARRAY_SIZE(repo_info_fields),
					    sizeof(*found),
					    repo_info_fields_cmp);
	return found ? found->get_value : NULL;
}

static int print_fields(int argc, const char **argv,
			struct repository *repo,
			enum output_format format)
{
	int ret = 0;
	struct strbuf valbuf = STRBUF_INIT;
	struct strbuf quotbuf = STRBUF_INIT;

	for (int i = 0; i < argc; i++) {
		get_value_fn *get_value;
		const char *key = argv[i];

		get_value = get_value_fn_for_key(key);

		if (!get_value) {
			ret = error(_("key '%s' not found"), key);
			continue;
		}

		strbuf_reset(&valbuf);
		strbuf_reset(&quotbuf);

		get_value(repo, &valbuf);

		switch (format) {
		case FORMAT_KEYVALUE:
			quote_c_style(valbuf.buf, &quotbuf, NULL, 0);
			printf("%s=%s\n", key, quotbuf.buf);
			break;
		case FORMAT_NUL_TERMINATED:
			printf("%s\n%s%c", key, valbuf.buf, '\0');
			break;
		default:
			BUG("not a valid output format: %d", format);
		}
	}

	strbuf_release(&valbuf);
	strbuf_release(&quotbuf);
	return ret;
}

static int parse_format_cb(const struct option *opt,
			   const char *arg, int unset UNUSED)
{
	enum output_format *format = opt->value;

	if (opt->short_name == 'z')
		*format = FORMAT_NUL_TERMINATED;
	else if (!strcmp(arg, "nul"))
		*format = FORMAT_NUL_TERMINATED;
	else if (!strcmp(arg, "keyvalue"))
		*format = FORMAT_KEYVALUE;
	else if (!strcmp(arg, "table"))
		*format = FORMAT_TABLE;
	else
		die(_("invalid format '%s'"), arg);

	return 0;
}

static int cmd_repo_info(int argc, const char **argv, const char *prefix,
			 struct repository *repo)
{
	enum output_format format = FORMAT_KEYVALUE;
	struct option options[] = {
		OPT_CALLBACK_F(0, "format", &format, N_("format"),
			       N_("output format"),
			       PARSE_OPT_NONEG, parse_format_cb),
		OPT_CALLBACK_F('z', NULL, &format, NULL,
			       N_("synonym for --format=nul"),
			       PARSE_OPT_NONEG | PARSE_OPT_NOARG,
			       parse_format_cb),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options, repo_usage, 0);
	if (format != FORMAT_KEYVALUE && format != FORMAT_NUL_TERMINATED)
		die(_("unsupported output format"));

	return print_fields(argc, argv, repo, format);
}

struct ref_stats {
	size_t branches;
	size_t remotes;
	size_t tags;
	size_t others;
};

struct object_stats {
	size_t tags;
	size_t commits;
	size_t trees;
	size_t blobs;
};

struct repo_stats {
	struct ref_stats refs;
	struct object_stats objects;
};

struct stats_table {
	struct string_list rows;

	size_t name_col_width;
	size_t value_col_width;
};

/*
 * Holds column data that gets stored for each row.
 */
struct stats_table_entry {
	char *value;
};

static void stats_table_add(struct stats_table *table, const char *format,
			    const char *name, struct stats_table_entry *entry)
{
	struct strbuf buf = STRBUF_INIT;
	struct string_list_item *item;
	char *formatted_name;
	size_t name_width;

	strbuf_addf(&buf, format, name);
	formatted_name = strbuf_detach(&buf, &name_width);

	item = string_list_append_nodup(&table->rows, formatted_name);
	item->util = entry;

	if (name_width > table->name_col_width)
		table->name_col_width = name_width;
	if (entry) {
		size_t value_width = strlen(entry->value);
		if (value_width > table->value_col_width)
			table->value_col_width = value_width;
	}
}

static void stats_table_add_count(struct stats_table *table, const char *format,
				  const char *name, size_t value)
{
	struct stats_table_entry *entry;

	CALLOC_ARRAY(entry, 1);
	entry->value = xstrfmt("%" PRIuMAX, (uintmax_t)value);
	stats_table_add(table, format, name, entry);
}

static void stats_table_setup(struct stats_table *table, struct repo_stats *stats)
{
	struct object_stats *objects = &stats->objects;
	struct ref_stats *refs = &stats->refs;
	size_t object_total;
	size_t ref_total;

	ref_total = refs->branches + refs->remotes + refs->tags + refs->others;
	stats_table_add(table, "* %s", _("References"), NULL);
	stats_table_add_count(table, "  * %s", _("Count"), ref_total);
	stats_table_add_count(table, "    * %s", _("Branches"), refs->branches);
	stats_table_add_count(table, "    * %s", _("Tags"), refs->tags);
	stats_table_add_count(table, "    * %s", _("Remotes"), refs->remotes);
	stats_table_add_count(table, "    * %s", _("Others"), refs->others);

	object_total = objects->commits + objects->trees + objects->blobs + objects->tags;
	stats_table_add(table, "%s", "", NULL);
	stats_table_add(table, "* %s", _("Reachable objects"), NULL);
	stats_table_add_count(table, "  * %s", _("Count"), object_total);
	stats_table_add_count(table, "    * %s", _("Commits"), objects->commits);
	stats_table_add_count(table, "    * %s", _("Trees"), objects->trees);
	stats_table_add_count(table, "    * %s", _("Blobs"), objects->blobs);
	stats_table_add_count(table, "    * %s", _("Tags"), objects->tags);
}

static inline size_t max_size_t(size_t a, size_t b)
{
	return (a > b) ? a : b;
}

static void stats_table_print(struct stats_table *table)
{
	const char *name_col_title = _("Repository stats");
	const char *value_col_title = _("Value");
	size_t name_title_len = strlen(name_col_title);
	size_t value_title_len = strlen(value_col_title);
	struct strbuf buf = STRBUF_INIT;
	struct string_list_item *item;
	int name_col_width;
	int value_col_width;

	name_col_width = cast_size_t_to_int(
		max_size_t(table->name_col_width, name_title_len));
	value_col_width = cast_size_t_to_int(
		max_size_t(table->value_col_width, value_title_len));

	strbuf_addf(&buf, "| %-*s | %-*s |\n", name_col_width, name_col_title,
		    value_col_width, value_col_title);
	strbuf_addstr(&buf, "| ");
	strbuf_addchars(&buf, '-', name_col_width);
	strbuf_addstr(&buf, " | ");
	strbuf_addchars(&buf, '-', value_col_width);
	strbuf_addstr(&buf, " |\n");

	for_each_string_list_item(item, &table->rows) {
		struct stats_table_entry *entry = item->util;
		const char *value = "";

		if (entry) {
			struct stats_table_entry *entry = item->util;
			value = entry->value;
		}

		strbuf_addf(&buf, "| %-*s | %*s |\n", name_col_width,
			    item->string, value_col_width, value);
	}

	fputs(buf.buf, stdout);
	strbuf_release(&buf);
}

static void stats_table_clear(struct stats_table *table)
{
	struct stats_table_entry *entry;
	struct string_list_item *item;

	for_each_string_list_item(item, &table->rows) {
		entry = item->util;
		if (entry)
			free(entry->value);
	}

	string_list_clear(&table->rows, 1);
}

static void stats_keyvalue_print(struct repo_stats *stats, char key_delim,
				 char value_delim)
{
	struct strbuf buf = STRBUF_INIT;

	strbuf_addf(&buf, "references.branches.count%c%" PRIuMAX "%c",
		    key_delim, (uintmax_t)stats->refs.branches, value_delim);
	strbuf_addf(&buf, "references.tags.count%c%" PRIuMAX "%c",
		    key_delim, (uintmax_t)stats->refs.tags, value_delim);
	strbuf_addf(&buf, "references.remotes.count%c%" PRIuMAX "%c",
		    key_delim, (uintmax_t)stats->refs.remotes, value_delim);
	strbuf_addf(&buf, "references.others.count%c%" PRIuMAX "%c",
		    key_delim, (uintmax_t)stats->refs.others, value_delim);

	strbuf_addf(&buf, "objects.commits.count%c%" PRIuMAX "%c",
		    key_delim, (uintmax_t)stats->objects.commits, value_delim);
	strbuf_addf(&buf, "objects.trees.count%c%" PRIuMAX "%c",
		    key_delim, (uintmax_t)stats->objects.trees, value_delim);
	strbuf_addf(&buf, "objects.blobs.count%c%" PRIuMAX "%c",
		    key_delim, (uintmax_t)stats->objects.blobs, value_delim);
	strbuf_addf(&buf, "objects.tags.count%c%" PRIuMAX "%c",
		    key_delim, (uintmax_t)stats->objects.tags, value_delim);

	fwrite(buf.buf, sizeof(char), buf.len, stdout);
	strbuf_release(&buf);
}

static void stats_count_references(struct ref_stats *stats, struct ref_array *refs)
{
	for (int i = 0; i < refs->nr; i++) {
		struct ref_array_item *ref = refs->items[i];

		switch (ref->kind) {
		case FILTER_REFS_BRANCHES:
			stats->branches++;
			break;
		case FILTER_REFS_REMOTES:
			stats->remotes++;
			break;
		case FILTER_REFS_TAGS:
			stats->tags++;
			break;
		case FILTER_REFS_OTHERS:
			stats->others++;
			break;
		default:
			BUG("unexpected reference type");
		}
	}
}

static int count_objects(const char *path UNUSED, struct oid_array *oids,
			 enum object_type type, void *cb_data)
{
	struct object_stats *stats = cb_data;

	switch (type) {
	case OBJ_TAG:
		stats->tags += oids->nr;
		break;
	case OBJ_COMMIT:
		stats->commits += oids->nr;
		break;
	case OBJ_TREE:
		stats->trees += oids->nr;
		break;
	case OBJ_BLOB:
		stats->blobs += oids->nr;
		break;
	default:
		BUG("invalid object type");
	}

	return 0;
}

static void stats_count_objects(struct object_stats *stats,
				struct ref_array *refs, struct rev_info *revs)
{
	struct path_walk_info info = PATH_WALK_INFO_INIT;

	info.revs = revs;
	info.path_fn = count_objects;
	info.path_fn_data = stats;

	for (int i = 0; i < refs->nr; i++) {
		struct ref_array_item *ref = refs->items[i];

		switch (ref->kind) {
		case FILTER_REFS_BRANCHES:
		case FILTER_REFS_TAGS:
		case FILTER_REFS_REMOTES:
		case FILTER_REFS_OTHERS:
			add_pending_oid(revs, NULL, &ref->objectname, 0);
			break;
		default:
			BUG("unexpected reference type");
		}
	}

	walk_objects_by_path(&info);
	path_walk_info_clear(&info);
}

static int cmd_repo_stats(int argc, const char **argv, const char *prefix,
			  struct repository *repo)
{
	struct ref_filter filter = REF_FILTER_INIT;
	struct stats_table table = {
		.rows = STRING_LIST_INIT_DUP,
	};
	enum output_format format = FORMAT_TABLE;
	struct repo_stats stats = { 0 };
	struct ref_array refs = { 0 };
	struct rev_info revs;
	struct option options[] = {
		OPT_CALLBACK_F(0, "format", &format, N_("format"),
			       N_("output format"),
			       PARSE_OPT_NONEG, parse_format_cb),
		OPT_END()
	};

	parse_options(argc, argv, prefix, options, repo_usage, 0);
	repo_init_revisions(repo, &revs, prefix);
	if (filter_refs(&refs, &filter, FILTER_REFS_REGULAR))
		die(_("unable to filter refs"));

	stats_count_references(&stats.refs, &refs);
	stats_count_objects(&stats.objects, &refs, &revs);

	switch (format) {
	case FORMAT_TABLE:
		stats_table_setup(&table, &stats);
		stats_table_print(&table);
		break;
	case FORMAT_KEYVALUE:
		stats_keyvalue_print(&stats, '=', '\n');
		break;
	case FORMAT_NUL_TERMINATED:
		stats_keyvalue_print(&stats, '\n', '\0');
		break;
	default:
		BUG("invalid output format");
	}

	stats_table_clear(&table);
	release_revisions(&revs);
	ref_array_clear(&refs);

	return 0;
}

int cmd_repo(int argc, const char **argv, const char *prefix,
	     struct repository *repo)
{
	parse_opt_subcommand_fn *fn = NULL;
	struct option options[] = {
		OPT_SUBCOMMAND("info", &fn, cmd_repo_info),
		OPT_SUBCOMMAND("stats", &fn, cmd_repo_stats),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options, repo_usage, 0);

	return fn(argc, argv, prefix, repo);
}
