/*
 * Lace
 * Core Actions Implementation
 *
 * This module implements the action dispatch system, translating UI-agnostic
 * actions into state mutations. UI-specific operations are delegated to
 * callbacks provided by the frontend (TUI, GTK, Cocoa, etc.)
 *
 * Hierarchy: AppState → Connection → Workspace → Tab
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "actions.h"
#include "app_state.h"
#include "workspace.h"
#include <stdlib.h>

/* ============================================================================
 * Helper Macros
 * ============================================================================
 */

/* Safe callback invocation */
#define UI_CALL(ui, func, ...)                                                 \
  do {                                                                         \
    if ((ui) && (ui)->func)                                                    \
      (ui)->func((ui)->ctx, ##__VA_ARGS__);                                    \
  } while (0)

#define UI_CALL_RET(ui, func, default_val, ...)                                \
  (((ui) && (ui)->func) ? (ui)->func((ui)->ctx, ##__VA_ARGS__) : (default_val))

/* ============================================================================
 * Navigation Actions (operate on Tab)
 * ============================================================================
 */

static ChangeFlags handle_cursor_move(AppState *app, const Action *action,
                                      const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab || !tab->data)
    return CHANGED_NONE;

  UI_CALL(ui, move_cursor, action->cursor_move.row_delta,
          action->cursor_move.col_delta);
  return CHANGED_CURSOR | CHANGED_SCROLL;
}

static ChangeFlags handle_page_up(AppState *app, const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab || !tab->data)
    return CHANGED_NONE;

  UI_CALL(ui, page_up);
  return CHANGED_CURSOR | CHANGED_SCROLL;
}

static ChangeFlags handle_page_down(AppState *app, const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab || !tab->data)
    return CHANGED_NONE;

  UI_CALL(ui, page_down);
  return CHANGED_CURSOR | CHANGED_SCROLL;
}

static ChangeFlags handle_home(AppState *app, const UICallbacks *ui) {
  if (!app)
    return CHANGED_NONE;

  UI_CALL(ui, home);
  return CHANGED_CURSOR | CHANGED_SCROLL | CHANGED_DATA;
}

static ChangeFlags handle_end(AppState *app, const UICallbacks *ui) {
  if (!app)
    return CHANGED_NONE;

  UI_CALL(ui, end);
  return CHANGED_CURSOR | CHANGED_SCROLL | CHANGED_DATA;
}

static ChangeFlags handle_column_first(AppState *app) {
  Tab *tab = app_current_tab(app);
  if (!tab)
    return CHANGED_NONE;

  tab_column_first(tab);
  return CHANGED_CURSOR | CHANGED_SCROLL;
}

static ChangeFlags handle_column_last(AppState *app) {
  Tab *tab = app_current_tab(app);
  if (!tab || !tab->data)
    return CHANGED_NONE;

  tab_column_last(tab);
  return CHANGED_CURSOR | CHANGED_SCROLL;
}

/* ============================================================================
 * Edit Actions (operate on Tab)
 * ============================================================================
 */

static ChangeFlags handle_edit_start(AppState *app, const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab || !tab->data || tab->data->num_rows == 0)
    return CHANGED_NONE;
  if (UI_CALL_RET(ui, is_sidebar_focused, false))
    return CHANGED_NONE;

  UI_CALL(ui, start_edit);
  return CHANGED_EDIT;
}

static ChangeFlags handle_edit_start_modal(AppState *app,
                                           const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab || !tab->data || tab->data->num_rows == 0)
    return CHANGED_NONE;
  if (UI_CALL_RET(ui, is_sidebar_focused, false))
    return CHANGED_NONE;

  UI_CALL(ui, start_modal_edit);
  return CHANGED_EDIT | CHANGED_DATA;
}

static ChangeFlags handle_edit_cancel(AppState *app, const UICallbacks *ui) {
  (void)app;
  UI_CALL(ui, cancel_edit);
  return CHANGED_EDIT;
}

static ChangeFlags handle_cell_set_null(AppState *app, const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab || !tab->data || tab->data->num_rows == 0)
    return CHANGED_NONE;
  if (UI_CALL_RET(ui, is_sidebar_focused, false))
    return CHANGED_NONE;

  UI_CALL(ui, set_cell_null);
  return CHANGED_DATA | CHANGED_STATUS;
}

static ChangeFlags handle_cell_set_empty(AppState *app, const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab || !tab->data || tab->data->num_rows == 0)
    return CHANGED_NONE;
  if (UI_CALL_RET(ui, is_sidebar_focused, false))
    return CHANGED_NONE;

  UI_CALL(ui, set_cell_empty);
  return CHANGED_DATA | CHANGED_STATUS;
}

static ChangeFlags handle_row_delete(AppState *app, const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab || !tab->data || tab->data->num_rows == 0)
    return CHANGED_NONE;
  if (UI_CALL_RET(ui, is_sidebar_focused, false))
    return CHANGED_NONE;

  UI_CALL(ui, delete_row);
  return CHANGED_DATA | CHANGED_CURSOR | CHANGED_STATUS;
}

/* ============================================================================
 * Tab Actions (switch tabs within current workspace)
 * ============================================================================
 */

static ChangeFlags handle_tab_next(AppState *app) {
  Workspace *ws = app_current_workspace(app);
  if (!ws || ws->num_tabs <= 1)
    return CHANGED_NONE;

  size_t next = (ws->current_tab + 1) % ws->num_tabs;
  workspace_switch_tab(ws, next);
  return CHANGED_WORKSPACE;
}

static ChangeFlags handle_tab_prev(AppState *app) {
  Workspace *ws = app_current_workspace(app);
  if (!ws || ws->num_tabs <= 1)
    return CHANGED_NONE;

  size_t prev = ws->current_tab > 0 ? ws->current_tab - 1 : ws->num_tabs - 1;
  workspace_switch_tab(ws, prev);
  return CHANGED_WORKSPACE;
}

static ChangeFlags handle_tab_switch(AppState *app, const Action *action) {
  Workspace *ws = app_current_workspace(app);
  if (!ws || action->workspace_switch.index >= ws->num_tabs)
    return CHANGED_NONE;

  workspace_switch_tab(ws, action->workspace_switch.index);
  return CHANGED_WORKSPACE;
}

/* ============================================================================
 * Workspace Actions (switch workspaces - now independent from connections)
 * ============================================================================
 */

static ChangeFlags handle_workspace_next(AppState *app) {
  if (!app || app->num_workspaces <= 1)
    return CHANGED_NONE;

  size_t next = (app->current_workspace + 1) % app->num_workspaces;
  app_switch_workspace(app, next);
  return CHANGED_WORKSPACE | CHANGED_SIDEBAR;
}

static ChangeFlags handle_workspace_prev(AppState *app) {
  if (!app || app->num_workspaces <= 1)
    return CHANGED_NONE;

  size_t prev = app->current_workspace > 0 ? app->current_workspace - 1
                                           : app->num_workspaces - 1;
  app_switch_workspace(app, prev);
  return CHANGED_WORKSPACE | CHANGED_SIDEBAR;
}

/* ============================================================================
 * Sidebar Actions (operate on Tab)
 * ============================================================================
 */

static ChangeFlags handle_sidebar_toggle(AppState *app, const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab)
    return CHANGED_NONE;

  if (UI_CALL_RET(ui, is_sidebar_visible, false)) {
    UI_CALL(ui, set_sidebar_visible, false);
    UI_CALL(ui, set_sidebar_focused, false);
  } else {
    UI_CALL(ui, set_sidebar_visible, true);
    UI_CALL(ui, set_sidebar_focused, true);
    size_t highlight =
        UI_CALL_RET(ui, get_sidebar_highlight_for_table, 0, tab->table_index);
    UI_CALL(ui, set_sidebar_highlight, highlight);
    UI_CALL(ui, set_sidebar_scroll, 0);
  }

  UI_CALL(ui, recreate_layout);
  UI_CALL(ui, recalculate_widths);
  return CHANGED_SIDEBAR | CHANGED_LAYOUT | CHANGED_FOCUS;
}

static ChangeFlags handle_sidebar_focus(AppState *app, const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab || !UI_CALL_RET(ui, is_sidebar_visible, false))
    return CHANGED_NONE;

  UI_CALL(ui, set_filters_was_focused,
          UI_CALL_RET(ui, is_filters_focused, false));
  UI_CALL(ui, set_filters_focused, false);
  UI_CALL(ui, set_sidebar_focused, true);
  UI_CALL(ui, set_sidebar_highlight,
          UI_CALL_RET(ui, get_sidebar_last_position, 0));
  return CHANGED_FOCUS | CHANGED_SIDEBAR;
}

static ChangeFlags handle_sidebar_unfocus(AppState *app,
                                          const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab)
    return CHANGED_NONE;

  UI_CALL(ui, set_sidebar_focused, false);
  /* Restore filters focus if it was focused before */
  if (UI_CALL_RET(ui, get_filters_was_focused, false) &&
      UI_CALL_RET(ui, is_filters_visible, false)) {
    UI_CALL(ui, set_filters_focused, true);
  }
  return CHANGED_FOCUS;
}

/* ============================================================================
 * Filter Panel Actions (operate on Tab)
 * ============================================================================
 */

static ChangeFlags handle_filters_toggle(AppState *app, const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab || tab->type != TAB_TYPE_TABLE || !tab->schema)
    return CHANGED_NONE;
  if (UI_CALL_RET(ui, is_sidebar_focused, false))
    return CHANGED_NONE;

  bool visible = !UI_CALL_RET(ui, is_filters_visible, false);
  UI_CALL(ui, set_filters_visible, visible);
  UI_CALL(ui, set_filters_focused, visible);
  UI_CALL(ui, set_filters_editing, false);

  return CHANGED_FILTERS | CHANGED_FOCUS | CHANGED_LAYOUT;
}

static ChangeFlags handle_filters_focus(AppState *app, const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab || !UI_CALL_RET(ui, is_filters_visible, false))
    return CHANGED_NONE;

  UI_CALL(ui, set_filters_focused, true);
  return CHANGED_FOCUS;
}

static ChangeFlags handle_filters_unfocus(AppState *app,
                                          const UICallbacks *ui) {
  Tab *tab = app_current_tab(app);
  if (!tab)
    return CHANGED_NONE;

  UI_CALL(ui, set_filters_focused, false);
  return CHANGED_FOCUS;
}

/* ============================================================================
 * UI Toggle Actions (operate on AppState - global)
 * ============================================================================
 */

static ChangeFlags handle_toggle_header(AppState *app, const UICallbacks *ui) {
  if (!app)
    return CHANGED_NONE;

  app->header_visible = !app->header_visible;
  UI_CALL(ui, recreate_layout);
  return CHANGED_LAYOUT;
}

static ChangeFlags handle_toggle_status(AppState *app, const UICallbacks *ui) {
  if (!app)
    return CHANGED_NONE;

  app->status_visible = !app->status_visible;
  UI_CALL(ui, recreate_layout);
  return CHANGED_LAYOUT;
}

/* ============================================================================
 * Data Loading Actions
 * ============================================================================
 */

static ChangeFlags handle_load_more_rows(AppState *app, const UICallbacks *ui) {
  (void)app;
  if (UI_CALL_RET(ui, load_more_rows, false)) {
    return CHANGED_DATA;
  }
  return CHANGED_NONE;
}

static ChangeFlags handle_load_prev_rows(AppState *app, const UICallbacks *ui) {
  (void)app;
  if (UI_CALL_RET(ui, load_prev_rows, false)) {
    return CHANGED_DATA | CHANGED_CURSOR;
  }
  return CHANGED_NONE;
}

static ChangeFlags handle_disconnect(AppState *app, const UICallbacks *ui) {
  (void)app;
  UI_CALL(ui, disconnect);
  return CHANGED_CONNECTION | CHANGED_DATA | CHANGED_TABLES | CHANGED_SIDEBAR;
}

/* ============================================================================
 * Application Actions
 * ============================================================================
 */

static ChangeFlags handle_quit(AppState *app) {
  if (!app)
    return CHANGED_NONE;

  /* If no connections, quit immediately */
  if (app->num_connections == 0) {
    app->running = false;
    return CHANGED_NONE;
  }

  /* Otherwise, UI should show confirmation dialog */
  return CHANGED_NONE;
}

static ChangeFlags handle_quit_force(AppState *app) {
  if (!app)
    return CHANGED_NONE;

  app->running = false;
  return CHANGED_NONE;
}

/* ============================================================================
 * Main Dispatch Function
 * ============================================================================
 */

ChangeFlags app_dispatch(AppState *app, const Action *action,
                         const UICallbacks *ui) {
  if (!app || !action)
    return CHANGED_NONE;

  switch (action->type) {
  /* Navigation */
  case ACTION_CURSOR_MOVE:
    return handle_cursor_move(app, action, ui);
  case ACTION_CURSOR_GOTO:
    /* Handled by UI dialog */
    return CHANGED_NONE;
  case ACTION_PAGE_UP:
    return handle_page_up(app, ui);
  case ACTION_PAGE_DOWN:
    return handle_page_down(app, ui);
  case ACTION_HOME:
    return handle_home(app, ui);
  case ACTION_END:
    return handle_end(app, ui);
  case ACTION_COLUMN_FIRST:
    return handle_column_first(app);
  case ACTION_COLUMN_LAST:
    return handle_column_last(app);

  /* Editing */
  case ACTION_EDIT_START:
    return handle_edit_start(app, ui);
  case ACTION_EDIT_START_MODAL:
    return handle_edit_start_modal(app, ui);
  case ACTION_EDIT_CONFIRM:
    /* Handled by UI edit handler */
    return CHANGED_NONE;
  case ACTION_EDIT_CANCEL:
    return handle_edit_cancel(app, ui);
  case ACTION_EDIT_INPUT:
  case ACTION_EDIT_BACKSPACE:
  case ACTION_EDIT_DELETE:
  case ACTION_EDIT_CURSOR_LEFT:
  case ACTION_EDIT_CURSOR_RIGHT:
  case ACTION_EDIT_CURSOR_HOME:
  case ACTION_EDIT_CURSOR_END:
    /* Handled by UI edit handler */
    return CHANGED_NONE;

  /* Cell operations */
  case ACTION_CELL_SET_NULL:
    return handle_cell_set_null(app, ui);
  case ACTION_CELL_SET_EMPTY:
    return handle_cell_set_empty(app, ui);
  case ACTION_ROW_DELETE:
    return handle_row_delete(app, ui);

  /* Tabs - switch within current workspace */
  case ACTION_TAB_NEXT:
    return handle_tab_next(app);
  case ACTION_TAB_PREV:
    return handle_tab_prev(app);
  case ACTION_TAB_SWITCH:
    return handle_tab_switch(app, action);
  case ACTION_TAB_CREATE:
  case ACTION_TAB_CREATE_QUERY:
  case ACTION_TAB_CLOSE:
    /* These need UI for table loading - handled by UI layer */
    return CHANGED_NONE;

  /* Workspaces - switch within current connection */
  case ACTION_WORKSPACE_NEXT:
    return handle_workspace_next(app);
  case ACTION_WORKSPACE_PREV:
    return handle_workspace_prev(app);
  case ACTION_WORKSPACE_SWITCH:
  case ACTION_WORKSPACE_CREATE:
  case ACTION_WORKSPACE_CREATE_QUERY:
  case ACTION_WORKSPACE_CLOSE:
    /* These need UI for workspace management - handled by UI layer */
    return CHANGED_NONE;

  /* Sidebar */
  case ACTION_SIDEBAR_TOGGLE:
    return handle_sidebar_toggle(app, ui);
  case ACTION_SIDEBAR_FOCUS:
    return handle_sidebar_focus(app, ui);
  case ACTION_SIDEBAR_UNFOCUS:
    return handle_sidebar_unfocus(app, ui);
  case ACTION_SIDEBAR_MOVE:
  case ACTION_SIDEBAR_SELECT:
  case ACTION_SIDEBAR_SELECT_NEW_TAB:
  case ACTION_SIDEBAR_FILTER_START:
  case ACTION_SIDEBAR_FILTER_INPUT:
  case ACTION_SIDEBAR_FILTER_CLEAR:
  case ACTION_SIDEBAR_FILTER_STOP:
    /* Handled by UI sidebar handler */
    return CHANGED_NONE;

  /* Filters */
  case ACTION_FILTERS_TOGGLE:
    return handle_filters_toggle(app, ui);
  case ACTION_FILTERS_FOCUS:
    return handle_filters_focus(app, ui);
  case ACTION_FILTERS_UNFOCUS:
    return handle_filters_unfocus(app, ui);
  case ACTION_FILTERS_MOVE:
  case ACTION_FILTERS_ADD:
  case ACTION_FILTERS_REMOVE:
  case ACTION_FILTERS_CLEAR:
  case ACTION_FILTERS_EDIT_START:
  case ACTION_FILTERS_EDIT_INPUT:
  case ACTION_FILTERS_EDIT_CONFIRM:
  case ACTION_FILTERS_EDIT_CANCEL:
  case ACTION_FILTERS_APPLY:
    /* Handled by UI filters handler */
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
    /* Handled by UI query handler */
    return CHANGED_NONE;

  /* Connection */
  case ACTION_CONNECT:
    /* UI handles connection dialog */
    return CHANGED_NONE;
  case ACTION_DISCONNECT:
    return handle_disconnect(app, ui);

  /* Data loading */
  case ACTION_TABLE_LOAD:
    /* Handled by UI */
    return CHANGED_NONE;
  case ACTION_TABLE_REFRESH:
    /* TODO: Implement refresh */
    return CHANGED_NONE;
  case ACTION_DATA_LOAD_MORE:
    return handle_load_more_rows(app, ui);
  case ACTION_DATA_LOAD_PREV:
    return handle_load_prev_rows(app, ui);

  /* UI toggles */
  case ACTION_TOGGLE_HEADER:
    return handle_toggle_header(app, ui);
  case ACTION_TOGGLE_STATUS:
    return handle_toggle_status(app, ui);

  /* Dialogs - UI handles these directly */
  case ACTION_SHOW_SCHEMA:
  case ACTION_SHOW_GOTO:
  case ACTION_SHOW_CONNECT:
  case ACTION_SHOW_HELP:
    return CHANGED_NONE;

  /* Application */
  case ACTION_QUIT:
    return handle_quit(app);
  case ACTION_QUIT_FORCE:
    return handle_quit_force(app);

  case ACTION_NONE:
  default:
    return CHANGED_NONE;
  }
}
