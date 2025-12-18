/*
 * lace - Database Viewer and Manager
 * Connection dialog view
 */

#ifndef LACE_CONNECT_VIEW_H
#define LACE_CONNECT_VIEW_H

#include "../tui.h"

/* Show connection dialog and return connection string */
char *connect_view_show(TuiState *state);

/* Show recent connections picker */
char *connect_view_recent(TuiState *state);

#endif /* LACE_CONNECT_VIEW_H */
