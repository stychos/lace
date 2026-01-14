/*
 * Lace ncurses frontend
 * Connection dialog
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FRONTEND_CONNECT_H
#define LACE_FRONTEND_CONNECT_H

#include "tui.h"

/* ==========================================================================
 * Connection Dialog Functions
 * ========================================================================== */

/*
 * Show connection dialog.
 * Returns true if a connection was made.
 */
bool connect_dialog(TuiState *tui);

/*
 * Show quick connect dialog (just connection string).
 * Returns true if a connection was made.
 */
bool connect_quick_dialog(TuiState *tui);

#endif /* LACE_FRONTEND_CONNECT_H */
