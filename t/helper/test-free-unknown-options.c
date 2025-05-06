#include "git-compat-util.h"
#include "parse-options.h"
#include "setup.h"
#include "strvec.h"

static const char *const free_unknown_options_usage[] = {
    "test-tool free-unknown-options",
    NULL
};

int cmd__free_unknown_options(int argc, const char **argv) {
    struct strvec *unknown_opts = xmalloc(sizeof(struct strvec));
    strvec_init(unknown_opts);
    const char *prefix = setup_git_directory();

    bool a, b;
	struct option options[] = {
		OPT_BOOL('a', "test-a", &a, N_("option a, only for test use")),
        OPT_BOOL('b', "test-b", &b, N_("option b, only for test use")),
        OPT_UNKNOWN(unknown_opts, (parse_opt_strdup_fn *)&strvec_push),
	};

    parse_options(argc, argv, prefix, options,
        free_unknown_options_usage, PARSE_OPT_KEEP_UNKNOWN_OPT);

    printf("a = %s\n", a? "true": "false");
    printf("b = %s\n", b? "true": "false");

    int i;
    for (i = 0; i < unknown_opts->nr; i++) {
        printf("free unknown option: %s\n", unknown_opts->v[i]);
    }
    strvec_clear(unknown_opts);
    free(unknown_opts);
}