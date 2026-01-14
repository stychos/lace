/*
 * Lace
 * Core Workspace/Tab Management (platform-independent)
 *
 * This module provides tab management and navigation that operates purely on
 * core state without any UI dependencies.
 *
 * Hierarchy: AppState → Connection → Workspace → Tab
 *
 * Note: Most lifecycle functions are declared in app_state.h.
 * This file contains navigation and pagination operations.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CORE_WORKSPACE_H
#define LACE_CORE_WORKSPACE_H

#include "app_state.h"
#include <stdbool.h>

/* ============================================================================
 * Tab Navigation Operations (UI-agnostic)
 * ============================================================================
 */

/* Move cursor within a tab
 * Returns true if cursor actually moved
 * visible_rows: number of visible data rows in UI (for scroll adjustment) */
bool tab_move_cursor(Tab *tab, int row_delta, int col_delta, int visible_rows);

/* Page up/down within a tab
 * visible_rows: number of visible data rows in UI */
void tab_page_up(Tab *tab, int visible_rows);
void tab_page_down(Tab *tab, int visible_rows);

/* Go to first/last row */
void tab_home(Tab *tab);
void tab_end(Tab *tab, int visible_rows);

/* Go to first/last column */
void tab_column_first(Tab *tab);
void tab_column_last(Tab *tab);

/* ============================================================================
 * Tab Pagination State (tracking only, no I/O)
 * ============================================================================
 */

/* Check if cursor is near edge of loaded data
 * Returns: -1 = near start, 0 = in middle, 1 = near end */
int tab_check_data_edge(Tab *tab, size_t threshold);

/* Check if more data exists beyond loaded range */
bool tab_has_more_data_forward(Tab *tab);
bool tab_has_more_data_backward(Tab *tab);

/* Update pagination tracking after data load
 * Call this after loading data to update tab state */
void tab_update_pagination(Tab *tab, size_t loaded_offset, size_t loaded_count,
                           size_t total_rows);

/* ============================================================================
 * Compatibility Aliases (for gradual migration)
 * ============================================================================
 * These will be removed after all code is updated to use Tab-based functions.
 */

/* Old workspace_* functions now operate on Tab */
#define workspace_move_cursor(ws, rd, cd, vr)                                  \
  tab_move_cursor((ws), (rd), (cd), (vr))
#define workspace_page_up(ws, vr) tab_page_up((ws), (vr))
#define workspace_page_down(ws, vr) tab_page_down((ws), (vr))
#define workspace_home(ws) tab_home((ws))
#define workspace_end(ws, vr) tab_end((ws), (vr))
#define workspace_column_first(ws) tab_column_first((ws))
#define workspace_column_last(ws) tab_column_last((ws))
#define workspace_check_data_edge(ws, t) tab_check_data_edge((ws), (t))
#define workspace_has_more_data_forward(ws) tab_has_more_data_forward((ws))
#define workspace_has_more_data_backward(ws) tab_has_more_data_backward((ws))
#define workspace_update_pagination(ws, o, c, t)                               \
  tab_update_pagination((ws), (o), (c), (t))

#endif /* LACE_CORE_WORKSPACE_H */
