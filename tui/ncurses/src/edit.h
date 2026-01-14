/*
 * Lace ncurses frontend
 * Cell editing
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FRONTEND_EDIT_H
#define LACE_FRONTEND_EDIT_H

#include "tui.h"

/* EditState is defined in tui.h */

/* ==========================================================================
 * Edit Functions
 * ========================================================================== */

/*
 * Start inline editing of current cell.
 *
 * @param tui    TUI state
 * @param edit   Edit state to initialize
 * @return       true if editing started
 */
bool edit_start(TuiState *tui, EditState *edit);

/*
 * Cancel current edit.
 *
 * @param edit   Edit state
 */
void edit_cancel(EditState *edit);

/*
 * Confirm edit and save to database.
 *
 * @param tui    TUI state
 * @param edit   Edit state
 * @return       true on success
 */
bool edit_confirm(TuiState *tui, EditState *edit);

/*
 * Handle input while in edit mode.
 *
 * @param tui    TUI state
 * @param edit   Edit state
 * @param ch     Input character
 * @return       true if input was handled
 */
bool edit_handle_input(TuiState *tui, EditState *edit, int ch);

/*
 * Draw the edit cell.
 *
 * @param tui    TUI state
 * @param edit   Edit state
 * @param win    Window to draw in
 * @param y      Y position
 * @param x      X position
 * @param width  Available width
 */
void edit_draw(TuiState *tui, EditState *edit, WINDOW *win, int y, int x, int width);

/*
 * Set current cell to NULL.
 *
 * @param tui    TUI state
 * @return       true on success
 */
bool edit_set_null(TuiState *tui);

/*
 * Set current cell to empty string.
 *
 * @param tui    TUI state
 * @return       true on success
 */
bool edit_set_empty(TuiState *tui);

/*
 * Delete current row.
 *
 * @param tui    TUI state
 * @return       true on success
 */
bool edit_delete_row(TuiState *tui);

/*
 * Free edit state resources.
 *
 * @param edit   Edit state
 */
void edit_free(EditState *edit);

#endif /* LACE_FRONTEND_EDIT_H */
