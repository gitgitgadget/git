#include <git-compat-util.h>
#include <list-objects-filter-extensions.h>
#include <object.h>
#include <hash.h>
#include <hex.h>
#include <trace.h>


static struct trace_key trace_filter = TRACE_KEY_INIT(FILTER);

struct rand_context {
	int percentageMatch;
	int matchCount;
	int blobCount;
	int treeCount;
	uint64_t started_at;
};

static int rand_init(
	const struct repository *r,
	const char *filter_arg,
	void **context)
{
	struct rand_context *ctx = calloc(1, sizeof(struct rand_context));

	ctx->percentageMatch = atoi(filter_arg);
	if (ctx->percentageMatch > 100 || ctx->percentageMatch < 0) {
	fprintf(stderr, "filter-rand: warning: invalid match %%: %s\n",
		filter_arg);
	ctx->percentageMatch = 1;  // default 1%
	}
	fprintf(stderr, "filter-rand: matching %d%%\n", ctx->percentageMatch);
	ctx->started_at = getnanotime();
	(*context) = ctx;

	return 0;
}

static enum list_objects_filter_result rand_filter_object(
	const struct repository *r,
	const enum list_objects_filter_situation filter_situation,
	struct object *obj,
	const char *pathname,
	const char *filename,
	enum list_objects_filter_omit *omit,
	void *context)
{
	struct rand_context *ctx = (struct rand_context*)(context);

	if ((ctx->blobCount + ctx->treeCount + 1) % 100000 == 0) {
		fprintf(stderr, "filter-rand: %d...\n",
			(ctx->blobCount + ctx->treeCount + 1));
	}

	switch (filter_situation) {
	default:
		die("filter-rand: unknown filter_situation: %d", filter_situation);

	case LOFS_BEGIN_TREE:
		ctx->treeCount++;
		/* always include all tree objects */
		return LOFR_MARK_SEEN | LOFR_DO_SHOW;

	case LOFS_END_TREE:
		return LOFR_ZERO;

	case LOFS_BLOB:
		ctx->blobCount++;

		if ((rand() % 100) < ctx->percentageMatch) {
			ctx->matchCount++;
			trace_printf_key(&trace_filter,
				"match: %s %s\n",
				oid_to_hex(&obj->oid),
				pathname
			);
			return LOFR_MARK_SEEN | LOFR_DO_SHOW;
		} else {
			*omit = LOFO_OMIT;
			return LOFR_MARK_SEEN; /* hard omit */
		}
	}
}

static void rand_free(const struct repository *r, void *context)
{
	struct rand_context *ctx = (struct rand_context*)(context);
	double elapsed = (getnanotime() - ctx->started_at)/1E9;
	int count = ctx->blobCount + ctx->treeCount;

	fprintf(stderr, "filter-rand: done: count=%d (blob=%d tree=%d) "
		"matched=%d elapsed=%fs rate=%0.1f/s average=%0.1fus\n",
		count, ctx->blobCount, ctx->treeCount, ctx->matchCount,
		elapsed, count/elapsed, elapsed/count*1E6);

	free(ctx);
}

const struct filter_extension filter_extension_rand = {
	"rand",
	&rand_init,
	&rand_filter_object,
	&rand_free,
};
