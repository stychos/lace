/*
 * Lace
 * Connecting behind the progress dialog, with password prompts
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONNECT_OPEN_H
#define LACE_CONNECT_OPEN_H

#include "../db_compat.h"
#include "../tui.h"
#include "../config/connections.h"

/* Connect to model (a saved connection, or one parsed from a URL; may be
 * NULL), first with connstr; ask for a database or SSH password when the
 * server wants one and try again (rebuilt with lace_saved_conn_build_connstr,
 * so a tunnel is kept). On success *final_connstr is the string that worked
 * (passwords given here in it). */
DbConnection *tui_open_connection(TuiState *state, const SavedConnection *model,
                                  const char *connstr, char **final_connstr,
                                  char **error, bool *cancelled);

/* Connect with connstr (NULL: model's saved string) and disconnect; *msg
 * says how it went. True if it worked. */
bool tui_test_connection(TuiState *state, const SavedConnection *model,
                         const char *connstr, char **msg);

/* A saved connection's string, with passwords typed this session in */
char *tui_saved_connstr(const SavedConnection *c);

/* "Connection failed: <why>", unless why already says it failed */
char *tui_connect_failure_message(const char *why);

/* Open connstr (model and saved_id: the saved connection it is, or NULL):
 * an open connection with the same key is used, else a new one (password
 * prompts as above); a URL goes to Recent; then it is placed
 * (tui_place_connection(kind, ws_idx, tab_idx)). False when it did not
 * open: *message says why (caller frees; NULL and *cancelled when the user
 * cancelled). */
bool tui_connect_and_place(TuiState *state, const SavedConnection *model, const char *saved_id,
                           const char *connstr, int kind, size_t ws_idx, size_t tab_idx, char **message,
                           bool *cancelled);

#endif /* LACE_CONNECT_OPEN_H */
