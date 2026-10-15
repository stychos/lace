/*
 * Lace
 * Connection dialog view
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONNECT_VIEW_H
#define LACE_CONNECT_VIEW_H

#include "../db_compat.h"
#include "../tui.h"

/* Connection open mode */
typedef enum {
  CONNECT_MODE_CANCELLED,     /* User cancelled */
  CONNECT_MODE_NEW_TAB,       /* Open in new tab of current workspace */
  CONNECT_MODE_NEW_WORKSPACE, /* Open in new workspace */
  CONNECT_MODE_QUIT           /* User wants to quit the app */
} ConnectMode;

/* Connection dialog result */
typedef struct {
  char *connstr;       /* Connection string (caller must str_secure_free) */
  char *saved_conn_id; /* Saved connection ID (caller must free, may be NULL) */
  ConnectMode mode;    /* How to open the connection */
  DbConnection *conn;  /* Already open (the dialog connected): the caller
                          adds it, never connects a second time */
} ConnectResult;

/* Show connection dialog and return connection result */
ConnectResult connect_view_show(TuiState *state);

#endif /* LACE_CONNECT_VIEW_H */
