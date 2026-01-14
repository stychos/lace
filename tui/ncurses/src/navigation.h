/*
 * Lace ncurses frontend
 * Navigation - cursor and page movement
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FRONTEND_NAVIGATION_H
#define LACE_FRONTEND_NAVIGATION_H

#include "tui.h"

/* ==========================================================================
 * Cursor Movement
 * ========================================================================== */

/*
 * Move cursor by delta.
 * Handles bounds checking and auto-loading of more data.
 */
void nav_move_cursor(TuiState *tui, int row_delta, int col_delta);

/*
 * Page up/down navigation.
 * Loads more data if needed.
 */
void nav_page_up(TuiState *tui);
void nav_page_down(TuiState *tui);

/*
 * Jump to first/last row.
 * Loads data at target position if needed.
 */
void nav_home(TuiState *tui);
void nav_end(TuiState *tui);

/*
 * Jump to first/last column.
 */
void nav_column_first(TuiState *tui);
void nav_column_last(TuiState *tui);

/* ==========================================================================
 * Sidebar Navigation
 * ========================================================================== */

/*
 * Move sidebar selection.
 */
void nav_sidebar_up(TuiState *tui);
void nav_sidebar_down(TuiState *tui);

/*
 * Open selected table from sidebar.
 */
void nav_sidebar_open_table(TuiState *tui);

/* ==========================================================================
 * Scroll Adjustment
 * ========================================================================== */

/*
 * Ensure cursor is visible by adjusting scroll.
 */
void nav_ensure_cursor_visible(TuiState *tui);

/*
 * Get the number of visible data rows.
 */
int nav_get_visible_rows(TuiState *tui);

/*
 * Get column width for a given column index.
 */
int nav_get_column_width(TuiState *tui, size_t col);

/*
 * Calculate column widths based on data.
 */
void nav_calculate_column_widths(TuiState *tui);

#endif /* LACE_FRONTEND_NAVIGATION_H */
