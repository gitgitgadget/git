#include "cache.h"
#include "fsm-ipc-unix.h"
#include "fsmonitor-ipc.h"

const char *fsmonitor_ipc__get_path(struct repository *r)
{
    return fsmonitor_ipc__get_path_unix(r);
}