/*
 * lace - Database Viewer and Manager
 * Core Workspace Management (platform-independent)
 *
 * This module provides workspace/tab management that operates purely on
 * AppState without any UI dependencies.
 */

#ifndef LACE_CORE_WORKSPACE_H
#define LACE_CORE_WORKSPACE_H

#include "app_state.h"
#include <stdbool.h>

/* ============================================================================
 * Workspace Lifecycle
 * ============================================================================
 */

/* Initialize a workspace struct (zeros and inits filters) */
void workspace_init(Workspace *ws);

/* Free all data owned by a workspace (but not the struct itself) */
void workspace_free_data(Workspace *ws);

/* ============================================================================
 * Workspace Operations (pure AppState, no UI)
 * ============================================================================
 */

/* Switch to a different workspace index
 * Returns the new workspace pointer, or NULL if invalid index
 * Caller is responsible for saving/restoring UI state */
Workspace *app_workspace_switch(AppState *app, size_t index);

/* Create a new workspace for a table
 * Returns the new workspace pointer, or NULL on failure
 * Does NOT load data - caller must do that after */
Workspace *app_workspace_create(AppState *app, size_t table_index,
                                const char *table_name);

/* Create a new query workspace
 * Returns the new workspace pointer, or NULL on failure */
Workspace *app_workspace_create_query(AppState *app);

/* Close a workspace by index
 * Returns true if closed, false if invalid or last workspace behavior needed
 * Updates app->current_workspace if needed */
bool app_workspace_close(AppState *app, size_t index);

/* Get workspace at index (NULL if invalid) */
Workspace *app_workspace_at(AppState *app, size_t index);

/* ============================================================================
 * Navigation Operations (UI-agnostic)
 * ============================================================================
 */

/* Move cursor within a workspace
 * Returns true if cursor actually moved
 * visible_rows: number of visible data rows in UI (for scroll adjustment) */
bool workspace_move_cursor(Workspace *ws, int row_delta, int col_delta,
                           int visible_rows);

/* Page up/down within a workspace
 * visible_rows: number of visible data rows in UI */
void workspace_page_up(Workspace *ws, int visible_rows);
void workspace_page_down(Workspace *ws, int visible_rows);

/* Go to first/last row */
void workspace_home(Workspace *ws);
void workspace_end(Workspace *ws, int visible_rows);

/* Go to first/last column */
void workspace_column_first(Workspace *ws);
void workspace_column_last(Workspace *ws);

/* ============================================================================
 * Pagination State (tracking only, no I/O)
 * ============================================================================
 */

/* Check if cursor is near edge of loaded data
 * Returns: -1 = near start, 0 = in middle, 1 = near end */
int workspace_check_data_edge(Workspace *ws, size_t threshold);

/* Check if more data exists beyond loaded range */
bool workspace_has_more_data_forward(Workspace *ws);
bool workspace_has_more_data_backward(Workspace *ws);

/* Update pagination tracking after data load
 * Call this after loading data to update workspace state */
void workspace_update_pagination(Workspace *ws, size_t loaded_offset,
                                 size_t loaded_count, size_t total_rows);

#endif /* LACE_CORE_WORKSPACE_H */
