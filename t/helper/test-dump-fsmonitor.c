#include "cache.h"
#include "config.h"

int cmd_main(int ac, const char **av)
{
	struct index_state *istate = &the_index;
	uint64_t now = getnanotime();
	int i, valid = 0;

	git_config_push_parameter("core.fsmonitor=keep");
	setup_git_directory();
	if (read_index_from(istate, get_index_file(), get_git_dir()) < 0)
		die("unable to read index file");
	if (!istate->fsmonitor_last_update) {
		printf("no fsmonitor\n");
		return 0;
	}

	printf("fsmonitor last update %"PRIuMAX", (%.2f seconds ago)\n",
	       (uintmax_t)istate->fsmonitor_last_update,
	       (now - istate->fsmonitor_last_update)/1.0e9);

	for (i = 0; i < istate->cache_nr; i++)
		if (istate->cache[i]->ce_flags & CE_FSMONITOR_VALID)
			valid++;

	printf("  valid: %d\n", valid);
	printf("  invalid: %d\n", istate->cache_nr - valid);

	return 0;
}
