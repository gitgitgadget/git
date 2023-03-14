#include "cache.h"
#include "config.h"
#include "strbuf.h"
#include "fsmonitor-ipc.h"

const char *fsmonitor_ipc__get_path(struct repository *r)
{
	static const char *ipc_path = NULL;
	git_SHA_CTX sha1ctx;
	struct strbuf pipe_name = STRBUF_INIT;
	unsigned char hash[GIT_MAX_RAWSZ];

	if (ipc_path)
		return ipc_path;

	git_SHA1_Init(&sha1ctx);
	git_SHA1_Update(&sha1ctx, r->worktree, strlen(r->worktree));
	git_SHA1_Final(hash, &sha1ctx);

	strbuf_addf(&pipe_name, "git-fsmonitor-%s", hash_to_hex(hash));
	ipc_path = strbuf_detach(&pipe_name, NULL);
	return ipc_path;
}
