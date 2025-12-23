/*
 * lace - Database Viewer and Manager
 * Connection dialog view
 */

#ifndef LACE_CONNECT_VIEW_H
#define LACE_CONNECT_VIEW_H

#include "../tui.h"

/* Connection open mode */
typedef enum {
  CONNECT_MODE_CANCELLED,    /* User cancelled */
  CONNECT_MODE_NEW_TAB,      /* Open in new tab of current workspace */
  CONNECT_MODE_NEW_WORKSPACE /* Open in new workspace */
} ConnectMode;

/* Connection dialog result */
typedef struct {
  char *connstr;    /* Connection string (caller must free) */
  ConnectMode mode; /* How to open the connection */
} ConnectResult;

/* Show connection dialog and return connection result */
ConnectResult connect_view_show(TuiState *state);

/* Show recent connections picker */
char *connect_view_recent(TuiState *state);

#endif /* LACE_CONNECT_VIEW_H */
