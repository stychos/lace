/*
 * Lace ncurses frontend
 * Sidebar - table list with filtering
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FRONTEND_SIDEBAR_H
#define LACE_FRONTEND_SIDEBAR_H

#include "tui.h"

/* ==========================================================================
 * Sidebar State
 * ========================================================================== */

typedef struct {
  bool filtering;        /* Currently in filter mode */
  char filter[128];      /* Filter string */
  size_t filter_len;     /* Filter string length */
} SidebarState;

/* ==========================================================================
 * Sidebar Functions
 * ========================================================================== */

/*
 * Draw sidebar.
 *
 * @param tui      TUI state
 * @param ss       Sidebar state
 * @param win      Sidebar window
 */
void sidebar_draw(TuiState *tui, SidebarState *ss, WINDOW *win);

/*
 * Handle sidebar input.
 *
 * @param tui      TUI state
 * @param ss       Sidebar state
 * @param ch       Input character
 * @return         true if input was handled
 */
bool sidebar_handle_input(TuiState *tui, SidebarState *ss, int ch);

/*
 * Count tables matching filter.
 *
 * @param tui      TUI state
 * @param ss       Sidebar state
 * @return         Number of matching tables
 */
size_t sidebar_count_filtered(TuiState *tui, SidebarState *ss);

/*
 * Get table name at filtered index.
 *
 * @param tui      TUI state
 * @param ss       Sidebar state
 * @param index    Filtered index
 * @return         Table name or NULL
 */
const char *sidebar_get_table(TuiState *tui, SidebarState *ss, size_t index);

/*
 * Start filter mode.
 *
 * @param tui      TUI state
 * @param ss       Sidebar state
 */
void sidebar_start_filter(TuiState *tui, SidebarState *ss);

/*
 * Clear filter.
 *
 * @param tui      TUI state
 * @param ss       Sidebar state
 */
void sidebar_clear_filter(TuiState *tui, SidebarState *ss);

#endif /* LACE_FRONTEND_SIDEBAR_H */
