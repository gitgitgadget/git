#include "builtin.h"
#include "gettext.h"
#include <stdio.h>

int cmd_psuh(int argc UNUSED, const char **argv UNUSED,
	     const char *prefix UNUSED, struct repository *repo UNUSED)
{
	printf(_("Pony saying hello goes here.\n"));
	return 0;
}