#ifndef FSM_SETTINGS_UNIX_H
#define FSM_SETTINGS_UNIX_H

#ifdef HAVE_FSMONITOR_OS_SETTINGS
/*
 * Check for compatibility on unix-like systems (e.g. Darwin and Linux)
 */
enum fsmonitor_reason fsm_os__incompatible_unix(struct repository *r, int ipc);
#endif /* HAVE_FSMONITOR_OS_SETTINGS */

#endif /* FSM_SETTINGS_UNIX_H */
