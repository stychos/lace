/*
 * lace - Database Viewer and Manager
 * Core Actions Implementation
 *
 * This module implements the action dispatch system, translating UI-agnostic
 * actions into state mutations. All business logic for user interactions
 * should live here or in functions called from here.
 *
 * ARCHITECTURE NOTE:
 * This file currently has dependencies on TUI for operations that need:
 * - Window dimensions (visible_rows calculation)
 * - Modal dialogs (pagination loading, edit modal)
 * - Window recreation (sidebar toggle)
 *
 * Future refactoring will introduce a callback/delegate pattern to allow
 * different frontends (GTK, Cocoa) to provide these UI-specific operations.
 * For now, simple state-only operations use core/workspace.h functions.
 */

#include "app_state.h"
#include "workspace.h"
#include "../tui/tui.h"
#include "../tui/tui_internal.h"
#include "actions.h"
#include <stdlib.h>

/* ============================================================================
 * Helper Macros
 * ============================================================================
 */

/* Get current workspace or return CHANGED_NONE */
#define GET_WORKSPACE_OR_RETURN(state, ws)                                     \
  Workspace *ws = NULL;                                                        \
  do {                                                                         \
    if (!state || state->num_workspaces == 0)                                  \
      return CHANGED_NONE;                                                     \
    ws = &state->workspaces[state->current_workspace];                         \
  } while (0)

/* ============================================================================
 * Navigation Actions
 * ============================================================================
 */

static ChangeFlags handle_cursor_move(TuiState *state, const Action *action) {
  if (!state || !state->data)
    return CHANGED_NONE;

  tui_move_cursor(state, action->cursor_move.row_delta,
                  action->cursor_move.col_delta);
  return CHANGED_CURSOR | CHANGED_SCROLL;
}

static ChangeFlags handle_page_up(TuiState *state) {
  if (!state || !state->data)
    return CHANGED_NONE;

  tui_page_up(state);
  return CHANGED_CURSOR | CHANGED_SCROLL;
}

static ChangeFlags handle_page_down(TuiState *state) {
  if (!state || !state->data)
    return CHANGED_NONE;

  tui_page_down(state);
  return CHANGED_CURSOR | CHANGED_SCROLL;
}

static ChangeFlags handle_home(TuiState *state) {
  if (!state)
    return CHANGED_NONE;

  tui_home(state);
  return CHANGED_CURSOR | CHANGED_SCROLL | CHANGED_DATA;
}

static ChangeFlags handle_end(TuiState *state) {
  if (!state)
    return CHANGED_NONE;

  tui_end(state);
  return CHANGED_CURSOR | CHANGED_SCROLL | CHANGED_DATA;
}

static ChangeFlags handle_column_first(TuiState *state) {
  if (!state || !state->app)
    return CHANGED_NONE;

  Workspace *ws = app_current_workspace(state->app);
  if (!ws)
    return CHANGED_NONE;

  /* Use core function */
  workspace_column_first(ws);

  /* Sync to TUI cache */
  state->cursor_col = ws->cursor_col;
  state->scroll_col = ws->scroll_col;

  return CHANGED_CURSOR | CHANGED_SCROLL;
}

static ChangeFlags handle_column_last(TuiState *state) {
  if (!state || !state->app)
    return CHANGED_NONE;

  Workspace *ws = app_current_workspace(state->app);
  if (!ws || !ws->data)
    return CHANGED_NONE;

  /* Use core function */
  workspace_column_last(ws);

  /* Sync to TUI cache */
  state->cursor_col = ws->cursor_col;

  return CHANGED_CURSOR | CHANGED_SCROLL;
}

/* ============================================================================
 * Edit Actions
 * ============================================================================
 */

static ChangeFlags handle_edit_start(TuiState *state) {
  if (!state || !state->data || state->data->num_rows == 0)
    return CHANGED_NONE;
  if (state->sidebar_focused)
    return CHANGED_NONE;

  tui_start_edit(state);
  return CHANGED_EDIT;
}

static ChangeFlags handle_edit_start_modal(TuiState *state) {
  if (!state || !state->data || state->data->num_rows == 0)
    return CHANGED_NONE;
  if (state->sidebar_focused)
    return CHANGED_NONE;

  tui_start_modal_edit(state);
  return CHANGED_EDIT | CHANGED_DATA;
}

static ChangeFlags handle_cell_set_null(TuiState *state) {
  if (!state || !state->data || state->data->num_rows == 0)
    return CHANGED_NONE;
  if (state->sidebar_focused)
    return CHANGED_NONE;

  tui_set_cell_direct(state, true);
  return CHANGED_DATA | CHANGED_STATUS;
}

static ChangeFlags handle_cell_set_empty(TuiState *state) {
  if (!state || !state->data || state->data->num_rows == 0)
    return CHANGED_NONE;
  if (state->sidebar_focused)
    return CHANGED_NONE;

  tui_set_cell_direct(state, false);
  return CHANGED_DATA | CHANGED_STATUS;
}

static ChangeFlags handle_row_delete(TuiState *state) {
  if (!state || !state->data || state->data->num_rows == 0)
    return CHANGED_NONE;
  if (state->sidebar_focused)
    return CHANGED_NONE;

  tui_delete_row(state);
  return CHANGED_DATA | CHANGED_CURSOR | CHANGED_STATUS;
}

/* ============================================================================
 * Workspace Actions
 * ============================================================================
 */

static ChangeFlags handle_workspace_next(TuiState *state) {
  if (!state || state->num_workspaces <= 1)
    return CHANGED_NONE;

  size_t next = (state->current_workspace + 1) % state->num_workspaces;
  workspace_switch(state, next);
  return CHANGED_WORKSPACE | CHANGED_DATA | CHANGED_CURSOR | CHANGED_SCROLL;
}

static ChangeFlags handle_workspace_prev(TuiState *state) {
  if (!state || state->num_workspaces <= 1)
    return CHANGED_NONE;

  size_t prev = state->current_workspace > 0 ? state->current_workspace - 1
                                             : state->num_workspaces - 1;
  workspace_switch(state, prev);
  return CHANGED_WORKSPACE | CHANGED_DATA | CHANGED_CURSOR | CHANGED_SCROLL;
}

static ChangeFlags handle_workspace_switch(TuiState *state,
                                           const Action *action) {
  if (!state || action->workspace_switch.index >= state->num_workspaces)
    return CHANGED_NONE;

  workspace_switch(state, action->workspace_switch.index);
  return CHANGED_WORKSPACE | CHANGED_DATA | CHANGED_CURSOR | CHANGED_SCROLL;
}

static ChangeFlags handle_workspace_create(TuiState *state,
                                           const Action *action) {
  if (!state)
    return CHANGED_NONE;

  if (workspace_create(state, action->workspace_create.table_index)) {
    return CHANGED_WORKSPACES | CHANGED_WORKSPACE | CHANGED_DATA;
  }
  return CHANGED_STATUS;
}

static ChangeFlags handle_workspace_create_query(TuiState *state) {
  if (!state)
    return CHANGED_NONE;

  workspace_create_query(state);
  return CHANGED_WORKSPACES | CHANGED_WORKSPACE;
}

static ChangeFlags handle_workspace_close(TuiState *state) {
  if (!state || state->num_workspaces == 0)
    return CHANGED_NONE;

  workspace_close(state);
  return CHANGED_WORKSPACES | CHANGED_WORKSPACE | CHANGED_DATA;
}

/* ============================================================================
 * Sidebar Actions
 * ============================================================================
 */

static ChangeFlags handle_sidebar_toggle(TuiState *state) {
  if (!state)
    return CHANGED_NONE;

  if (state->sidebar_visible) {
    state->sidebar_visible = false;
    state->sidebar_focused = false;
  } else {
    state->sidebar_visible = true;
    state->sidebar_focused = true;
    state->sidebar_highlight =
        tui_get_sidebar_highlight_for_table(state, state->current_table);
    state->sidebar_scroll = 0;
  }

  tui_recreate_windows(state);
  tui_calculate_column_widths(state);
  return CHANGED_SIDEBAR | CHANGED_LAYOUT | CHANGED_FOCUS;
}

static ChangeFlags handle_sidebar_focus(TuiState *state) {
  if (!state || !state->sidebar_visible)
    return CHANGED_NONE;

  state->filters_was_focused = false;
  state->sidebar_focused = true;
  state->sidebar_highlight = state->sidebar_last_position;
  return CHANGED_FOCUS | CHANGED_SIDEBAR;
}

static ChangeFlags handle_sidebar_unfocus(TuiState *state) {
  if (!state)
    return CHANGED_NONE;

  state->sidebar_focused = false;
  return CHANGED_FOCUS;
}

/* ============================================================================
 * Filter Panel Actions
 * ============================================================================
 */

static ChangeFlags handle_filters_toggle(TuiState *state) {
  if (!state || state->sidebar_focused)
    return CHANGED_NONE;
  if (state->num_workspaces == 0)
    return CHANGED_NONE;

  Workspace *ws = &state->workspaces[state->current_workspace];
  if (ws->type != WORKSPACE_TYPE_TABLE || !state->schema)
    return CHANGED_NONE;

  state->filters_visible = !state->filters_visible;
  state->filters_focused = state->filters_visible;
  state->filters_editing = false;

  /* Restore cursor position from workspace when opening */
  if (state->filters_visible) {
    state->filters_cursor_row = ws->filters_cursor_row;
    state->filters_cursor_col = ws->filters_cursor_col;
  }

  return CHANGED_FILTERS | CHANGED_FOCUS | CHANGED_LAYOUT;
}

static ChangeFlags handle_filters_focus(TuiState *state) {
  if (!state || !state->filters_visible)
    return CHANGED_NONE;

  state->filters_focused = true;
  return CHANGED_FOCUS;
}

static ChangeFlags handle_filters_unfocus(TuiState *state) {
  if (!state)
    return CHANGED_NONE;

  state->filters_focused = false;
  return CHANGED_FOCUS;
}

/* ============================================================================
 * UI Toggle Actions
 * ============================================================================
 */

static ChangeFlags handle_toggle_header(TuiState *state) {
  if (!state)
    return CHANGED_NONE;

  state->header_visible = !state->header_visible;
  tui_recreate_windows(state);
  return CHANGED_LAYOUT;
}

static ChangeFlags handle_toggle_status(TuiState *state) {
  if (!state)
    return CHANGED_NONE;

  state->status_visible = !state->status_visible;
  tui_recreate_windows(state);
  return CHANGED_LAYOUT;
}

/* ============================================================================
 * Application Actions
 * ============================================================================
 */

static ChangeFlags handle_quit(TuiState *state) {
  if (!state)
    return CHANGED_NONE;

  /* If no connection, quit immediately */
  if (!state->conn) {
    state->running = false;
    return CHANGED_NONE;
  }

  /* Otherwise, UI should show confirmation dialog */
  /* Return special flag to indicate dialog needed */
  return CHANGED_NONE; /* UI handles confirmation */
}

static ChangeFlags handle_quit_force(TuiState *state) {
  if (!state)
    return CHANGED_NONE;

  state->running = false;
  return CHANGED_NONE;
}

/* ============================================================================
 * Main Dispatch Function
 * ============================================================================
 */

ChangeFlags app_dispatch(struct TuiState *state, const Action *action) {
  /* Cast to TuiState* for internal use (struct TuiState and TuiState are same) */
  TuiState *s = (TuiState *)state;

  if (!s || !action)
    return CHANGED_NONE;

  switch (action->type) {
  /* Navigation */
  case ACTION_CURSOR_MOVE:
    return handle_cursor_move(s, action);
  case ACTION_CURSOR_GOTO:
    /* Handled by UI dialog, then calls tui_load_rows_at_with_dialog */
    return CHANGED_NONE;
  case ACTION_PAGE_UP:
    return handle_page_up(s);
  case ACTION_PAGE_DOWN:
    return handle_page_down(s);
  case ACTION_HOME:
    return handle_home(s);
  case ACTION_END:
    return handle_end(s);
  case ACTION_COLUMN_FIRST:
    return handle_column_first(s);
  case ACTION_COLUMN_LAST:
    return handle_column_last(s);

  /* Editing */
  case ACTION_EDIT_START:
    return handle_edit_start(s);
  case ACTION_EDIT_START_MODAL:
    return handle_edit_start_modal(s);
  case ACTION_EDIT_CONFIRM:
    /* Handled by edit.c via tui_handle_edit_input */
    return CHANGED_NONE;
  case ACTION_EDIT_CANCEL:
    tui_cancel_edit(s);
    return CHANGED_EDIT;
  case ACTION_EDIT_INPUT:
  case ACTION_EDIT_BACKSPACE:
  case ACTION_EDIT_DELETE:
  case ACTION_EDIT_CURSOR_LEFT:
  case ACTION_EDIT_CURSOR_RIGHT:
  case ACTION_EDIT_CURSOR_HOME:
  case ACTION_EDIT_CURSOR_END:
    /* Handled by edit.c via tui_handle_edit_input */
    return CHANGED_NONE;

  /* Cell operations */
  case ACTION_CELL_SET_NULL:
    return handle_cell_set_null(s);
  case ACTION_CELL_SET_EMPTY:
    return handle_cell_set_empty(s);
  case ACTION_ROW_DELETE:
    return handle_row_delete(s);

  /* Workspaces */
  case ACTION_WORKSPACE_NEXT:
    return handle_workspace_next(s);
  case ACTION_WORKSPACE_PREV:
    return handle_workspace_prev(s);
  case ACTION_WORKSPACE_SWITCH:
    return handle_workspace_switch(s, action);
  case ACTION_WORKSPACE_CREATE:
    return handle_workspace_create(s, action);
  case ACTION_WORKSPACE_CREATE_QUERY:
    return handle_workspace_create_query(s);
  case ACTION_WORKSPACE_CLOSE:
    return handle_workspace_close(s);

  /* Sidebar */
  case ACTION_SIDEBAR_TOGGLE:
    return handle_sidebar_toggle(s);
  case ACTION_SIDEBAR_FOCUS:
    return handle_sidebar_focus(s);
  case ACTION_SIDEBAR_UNFOCUS:
    return handle_sidebar_unfocus(s);
  case ACTION_SIDEBAR_MOVE:
  case ACTION_SIDEBAR_SELECT:
  case ACTION_SIDEBAR_SELECT_NEW_TAB:
  case ACTION_SIDEBAR_FILTER_START:
  case ACTION_SIDEBAR_FILTER_INPUT:
  case ACTION_SIDEBAR_FILTER_CLEAR:
  case ACTION_SIDEBAR_FILTER_STOP:
    /* Handled by sidebar.c via tui_handle_sidebar_input */
    return CHANGED_NONE;

  /* Filters */
  case ACTION_FILTERS_TOGGLE:
    return handle_filters_toggle(s);
  case ACTION_FILTERS_FOCUS:
    return handle_filters_focus(s);
  case ACTION_FILTERS_UNFOCUS:
    return handle_filters_unfocus(s);
  case ACTION_FILTERS_MOVE:
  case ACTION_FILTERS_ADD:
  case ACTION_FILTERS_REMOVE:
  case ACTION_FILTERS_CLEAR:
  case ACTION_FILTERS_EDIT_START:
  case ACTION_FILTERS_EDIT_INPUT:
  case ACTION_FILTERS_EDIT_CONFIRM:
  case ACTION_FILTERS_EDIT_CANCEL:
  case ACTION_FILTERS_APPLY:
    /* Handled by filters.c via tui_handle_filters_input */
    return CHANGED_NONE;

  /* Query */
  case ACTION_QUERY_INPUT:
  case ACTION_QUERY_BACKSPACE:
  case ACTION_QUERY_DELETE:
  case ACTION_QUERY_NEWLINE:
  case ACTION_QUERY_CURSOR_MOVE:
  case ACTION_QUERY_EXECUTE:
  case ACTION_QUERY_EXECUTE_ALL:
  case ACTION_QUERY_EXECUTE_TXN:
  case ACTION_QUERY_FOCUS_RESULTS:
  case ACTION_QUERY_FOCUS_EDITOR:
    /* Handled by query.c via tui_handle_query_input */
    return CHANGED_NONE;

  /* Connection */
  case ACTION_CONNECT:
    /* UI handles connection dialog */
    return CHANGED_NONE;
  case ACTION_DISCONNECT:
    tui_disconnect(s);
    return CHANGED_CONNECTION | CHANGED_DATA | CHANGED_TABLES | CHANGED_SIDEBAR;

  /* Data loading */
  case ACTION_TABLE_LOAD:
    /* Handled by tui_load_table_data */
    return CHANGED_NONE;
  case ACTION_TABLE_REFRESH:
    /* TODO: Implement refresh */
    return CHANGED_NONE;
  case ACTION_DATA_LOAD_MORE:
    if (tui_load_more_rows(s)) {
      return CHANGED_DATA;
    }
    return CHANGED_NONE;
  case ACTION_DATA_LOAD_PREV:
    if (tui_load_prev_rows(s)) {
      return CHANGED_DATA | CHANGED_CURSOR;
    }
    return CHANGED_NONE;

  /* UI toggles */
  case ACTION_TOGGLE_HEADER:
    return handle_toggle_header(s);
  case ACTION_TOGGLE_STATUS:
    return handle_toggle_status(s);

  /* Dialogs - UI handles these directly */
  case ACTION_SHOW_SCHEMA:
  case ACTION_SHOW_GOTO:
  case ACTION_SHOW_CONNECT:
  case ACTION_SHOW_HELP:
    return CHANGED_NONE;

  /* Application */
  case ACTION_QUIT:
    return handle_quit(s);
  case ACTION_QUIT_FORCE:
    return handle_quit_force(s);

  default:
    return CHANGED_NONE;
  }
}
