/*
 * Lace ncurses frontend
 * Modal dialogs
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FRONTEND_DIALOGS_H
#define LACE_FRONTEND_DIALOGS_H

#include "tui.h"

/* ==========================================================================
 * Confirmation Dialog
 * ========================================================================== */

/*
 * Show a yes/no confirmation dialog.
 *
 * @param tui      TUI state
 * @param title    Dialog title
 * @param message  Message to display
 * @return         true if user selected Yes
 */
bool dialog_confirm(TuiState *tui, const char *title, const char *message);

/* ==========================================================================
 * Input Dialogs
 * ========================================================================== */

/*
 * Show a dialog to input a row number.
 *
 * @param tui      TUI state
 * @param max_row  Maximum valid row number
 * @param result   Output: selected row number (0-based)
 * @return         true if user entered a valid number
 */
bool dialog_goto_row(TuiState *tui, size_t max_row, size_t *result);

/*
 * Show a text input dialog.
 *
 * @param tui      TUI state
 * @param title    Dialog title
 * @param prompt   Input prompt
 * @param initial  Initial text (can be NULL)
 * @param result   Output: entered text (caller frees)
 * @return         true if user confirmed input
 */
bool dialog_input(TuiState *tui, const char *title, const char *prompt,
                  const char *initial, char **result);

/* ==========================================================================
 * Schema Dialog
 * ========================================================================== */

/*
 * Show table schema information.
 *
 * @param tui      TUI state
 */
void dialog_schema(TuiState *tui);

/* ==========================================================================
 * Help Dialog
 * ========================================================================== */

/*
 * Show help/keyboard shortcuts dialog.
 *
 * @param tui      TUI state
 */
void dialog_help(TuiState *tui);

/* ==========================================================================
 * Message Dialog
 * ========================================================================== */

/*
 * Show a simple message dialog.
 *
 * @param tui      TUI state
 * @param title    Dialog title
 * @param message  Message to display
 */
void dialog_message(TuiState *tui, const char *title, const char *message);

#endif /* LACE_FRONTEND_DIALOGS_H */
