/*
 * Lace ncurses frontend
 * Filter panel UI
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FRONTEND_FILTERS_H
#define LACE_FRONTEND_FILTERS_H

#include "tui.h"

/* Maximum visible filter rows */
#define MAX_VISIBLE_FILTERS 5

/* FilterPanelState is defined in tui.h */

/* ==========================================================================
 * Filter Panel Functions
 * ========================================================================== */

/*
 * Toggle filter panel visibility.
 */
void filters_toggle(TuiState *tui, FilterPanelState *fp);

/*
 * Get filter panel height.
 */
int filters_get_height(TuiState *tui, FilterPanelState *fp);

/*
 * Draw filter panel.
 */
void filters_draw(TuiState *tui, FilterPanelState *fp, WINDOW *win, int y);

/*
 * Handle filter panel input.
 * Returns true if input was handled.
 */
bool filters_handle_input(TuiState *tui, FilterPanelState *fp, int ch);

/*
 * Add a new filter.
 */
void filters_add(TuiState *tui, FilterPanelState *fp);

/*
 * Remove current filter.
 */
void filters_remove(TuiState *tui, FilterPanelState *fp);

/*
 * Clear all filters.
 */
void filters_clear(TuiState *tui, FilterPanelState *fp);

/*
 * Apply filters and reload data.
 */
void filters_apply(TuiState *tui);

#endif /* LACE_FRONTEND_FILTERS_H */
