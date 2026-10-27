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

/* How the Workspaces window closed */
typedef enum {
  CONNECT_MODE_CANCELLED, /* Closed (anything it did is done) */
  CONNECT_MODE_QUIT       /* The user wants to quit the app */
} ConnectMode;

typedef struct {
  ConnectMode mode;
} ConnectResult;

/* Show the Workspaces window (the workspaces manager) */
ConnectResult connect_view_show(TuiState *state);

#endif /* LACE_CONNECT_VIEW_H */
