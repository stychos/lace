/*
 * Lace ncurses frontend
 * Session save/restore
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FRONTEND_SESSION_H
#define LACE_FRONTEND_SESSION_H

#include "tui.h"

#define SESSION_FILE "session.json"

/* ==========================================================================
 * Session Functions
 * ========================================================================== */

/*
 * Save current session to file.
 */
bool session_save(TuiState *tui);

/*
 * Restore session from file.
 * Returns true if session was restored.
 */
bool session_restore(TuiState *tui);

/*
 * Get session file path (caller must free).
 */
char *session_get_path(void);

/*
 * Clear saved session.
 */
void session_clear(void);

#endif /* LACE_FRONTEND_SESSION_H */
