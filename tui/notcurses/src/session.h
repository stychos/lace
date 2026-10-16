/* session.json (version 2): the open workspaces, shared with the other
 * frontends. Members this frontend does not own are kept on save. */
#ifndef NC_SESSION_H
#define NC_SESSION_H

#include "app.h"
#include <cjson/cJSON.h>
#include <stdbool.h>

bool nc_session_save(NcApp *app, char **error); /* keeps other top-level members */
/* v2 root (converted when v1); NULL when absent (no error) or broken (error set) */
cJSON *nc_session_load(char **error);
char *nc_session_path(void);

/* Connect every connection of every workspace of a v2 root, one after another
 * (app_connect_url; a missing password is asked for, a failure is logged and
 * skipped), open its tabs, then switch to the root's current workspace. The
 * first workspace takes over the current one when that has no tabs. root is
 * only read. done (may be NULL) runs after the last connection. */
void nc_session_restore(NcApp *app, const cJSON *root, void (*done)(NcApp *app));
void nc_session_restore_free(NcApp *app); /* app_free: restores cut short by quitting */

#endif
