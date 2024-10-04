#include <stdio.h>
#include "builtin.h"
#include "gettext.h"

int cmd_psuh(int argc, const char **argv, const char *prefix, struct repository *repo) {
    printf("%d\n, %s\n, %s\n", argc, argv[1], prefix);
    printf("%d\n", repo->different_commondir);
    printf(_("Pony saying hello goes here.\n"));
    return 0;
}
