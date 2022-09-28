#include "config.h"
#include "fsmonitor.h"
#include "fsmonitor-ipc.h"
#include "fsmonitor-path-utils.h"
#include "fsmonitor-settings.h"
#include "fsm-settings-unix.h"

enum fsmonitor_reason fsm_os__incompatible(struct repository *r, int ipc)
{
    return fsm_os__incompatible_unix(r, ipc);
}
