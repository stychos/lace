/*
 * Lace
 * TUI Tab management
 *
 * Core tab/workspace/connection lifecycle is in core/app_state.c.
 * This file contains TUI-specific operations that sync UI state.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "core/workspace.h"
#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>

/* Save current TUI state to tab */
void tab_save(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;

  /* UI state to UITabState (source of truth) */
  UITabState *ui = TUI_TAB_UI(state);

  /* Sync widget state to Tab before saving (widget may have been modified) */
  if (ui && ui->table_widget && tab->type == TAB_TYPE_TABLE) {
    table_widget_sync_to_tab(ui->table_widget);
  }
  /* TODO: Add query_widget_sync_to_tab when QueryWidget is migrated */

  /* Sync TUI-specific UI state to UITabState */
  if (ui) {
    ui->filters_visible = state->filters_visible;
    ui->filters_focused = state->filters_focused;
    ui->filters_was_focused = state->filters_was_focused;
    ui->filters_cursor_row = state->filters_cursor_row;
    ui->filters_cursor_col = state->filters_cursor_col;
    ui->filters_scroll = state->filters_scroll;
    ui->sidebar_visible = state->sidebar_visible;
    ui->sidebar_focused = state->sidebar_focused;
    ui->sidebar_highlight = state->sidebar_highlight;
    ui->sidebar_scroll = state->sidebar_scroll;
    ui->sidebar_last_position = state->sidebar_last_position;
    ui->sidebar_filter_len = state->sidebar_filter_len;
    memcpy(ui->sidebar_filter, state->sidebar_filter,
           sizeof(ui->sidebar_filter));
  }
}

/* Restore TUI state from tab */
void tab_restore(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;

  /* Track layout state changes for window recreation */
  bool sidebar_was_visible = state->sidebar_visible;
  bool header_was_visible = state->header_visible;
  bool status_was_visible = state->status_visible;

  /* Rebind VmTable to new Tab so cursor/scroll reads work correctly */
  if (state->vm_table) {
    vm_table_bind(state->vm_table, tab);
  }

  /* UI state from UITabState (source of truth) */
  UITabState *ui = TUI_TAB_UI(state);
  if (ui) {
    state->filters_visible = ui->filters_visible;
    state->filters_focused = ui->filters_focused;
    state->filters_was_focused = ui->filters_was_focused;
    state->filters_cursor_row = ui->filters_cursor_row;
    state->filters_cursor_col = ui->filters_cursor_col;
    state->filters_scroll = ui->filters_scroll;
    state->sidebar_visible = ui->sidebar_visible;
    state->sidebar_focused = ui->sidebar_focused;
    state->sidebar_highlight = ui->sidebar_highlight;
    state->sidebar_scroll = ui->sidebar_scroll;
    state->sidebar_last_position = ui->sidebar_last_position;
    state->sidebar_filter_len = ui->sidebar_filter_len;
    memcpy(state->sidebar_filter, ui->sidebar_filter,
           sizeof(state->sidebar_filter));

    /* Initialize widgets if not already done (lazy initialization) */
    if (tab->type == TAB_TYPE_TABLE && !ui->table_widget) {
      tui_init_table_tab_widgets(state, ui, tab);
      /* New widget synced via table_widget_bind -> sync_from_tab */
    } else if (tab->type == TAB_TYPE_TABLE && ui->table_widget) {
      /* Existing widget - sync from Tab */
      table_widget_sync_from_tab(ui->table_widget);
    } else if (tab->type == TAB_TYPE_QUERY && !ui->query_widget) {
      tui_init_query_tab_widgets(state, ui, tab);
    }
    /* TODO: Add query_widget_sync_from_tab when QueryWidget is migrated */

    /* Sync sidebar widget state from UITabState */
    if (ui->sidebar_widget) {
      ui->sidebar_widget->base.state.visible = ui->sidebar_visible;
      ui->sidebar_widget->base.state.focused = ui->sidebar_focused;
      ui->sidebar_widget->base.state.cursor_row = ui->sidebar_highlight;
      ui->sidebar_widget->base.state.scroll_row = ui->sidebar_scroll;
    }

    /* Sync filters widget state from UITabState */
    if (ui->filters_widget) {
      ui->filters_widget->base.state.visible = ui->filters_visible;
      ui->filters_widget->base.state.focused = ui->filters_focused;
    }
  } else {
    /* No UITabState - use defaults */
    state->filters_visible = false;
    state->filters_focused = false;
    state->filters_was_focused = false;
    state->filters_cursor_row = 0;
    state->filters_cursor_col = 0;
    state->filters_scroll = 0;
    state->sidebar_visible = true;
    state->sidebar_focused = false;
    state->sidebar_highlight = 0;
    state->sidebar_scroll = 0;
    state->sidebar_last_position = 0;
    state->sidebar_filter[0] = '\0';
    state->sidebar_filter_len = 0;
  }
  state->filters_editing = false;
  state->sidebar_filter_active = false;

  /* Layout visibility from app */
  if (state->app) {
    state->header_visible = state->app->header_visible;
    state->status_visible = state->app->status_visible;
  }

  /* Recreate windows if layout changed */
  if (sidebar_was_visible != state->sidebar_visible ||
      header_was_visible != state->header_visible ||
      status_was_visible != state->status_visible) {
    tui_recreate_windows(state);
  }

  /* Check if tab needs refresh due to changes in another tab */
  if (tab->needs_refresh && tab->type == TAB_TYPE_TABLE && tab->table_name) {
    tab->needs_refresh = false;
    tui_refresh_table(state);
  }
}

/* Legacy wrapper for compatibility */
void workspace_save(TuiState *state) { tab_save(state); }

/* Legacy wrapper for compatibility */
void workspace_restore(TuiState *state) { tab_restore(state); }

/* Sync focus and panel state from TuiState to UITabState.
 * Call this when input handlers modify focus state so it persists across tab
 * switches. */
void tab_sync_focus(TuiState *state) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!ui)
    return;

  /* Sync focus-related state to UITabState (source of truth) */
  ui->sidebar_visible = state->sidebar_visible;
  ui->sidebar_focused = state->sidebar_focused;
  ui->sidebar_highlight = state->sidebar_highlight;
  ui->sidebar_scroll = state->sidebar_scroll;
  ui->sidebar_last_position = state->sidebar_last_position;
  ui->sidebar_filter_len = state->sidebar_filter_len;
  memcpy(ui->sidebar_filter, state->sidebar_filter, sizeof(ui->sidebar_filter));

  ui->filters_visible = state->filters_visible;
  ui->filters_focused = state->filters_focused;
  ui->filters_was_focused = state->filters_was_focused;
  ui->filters_cursor_row = state->filters_cursor_row;
  ui->filters_cursor_col = state->filters_cursor_col;
  ui->filters_scroll = state->filters_scroll;
}

/* Switch to a different tab */
void tab_switch(TuiState *state, size_t index) {
  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws || index >= ws->num_tabs)
    return;
  if (index == ws->current_tab)
    return;

  /* Cancel any pending background load */
  tui_cancel_background_load(state);

  /* Save current tab state */
  tab_save(state);

  /* Switch to new tab */
  workspace_switch_tab(ws, index);

  /* Restore new tab state */
  tab_restore(state);

  /* Clear status message */
  free(state->status_msg);
  state->status_msg = NULL;
  state->status_is_error = false;
}

/* Legacy wrapper */
void workspace_switch(TuiState *state, size_t index) {
  tab_switch(state, index);
}

/* Create a new table tab */
bool tab_create(TuiState *state, size_t table_index) {
  if (!state || !state->app || table_index >= state->num_tables)
    return false;

  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws) {
    /* No workspace - need to create one first */
    ws = app_create_workspace(state->app);
    if (!ws)
      return false;
  }

  /* Get connection index - use current tab's connection if available */
  size_t connection_index = 0;
  Tab *current_tab = TUI_TAB(state);
  if (current_tab) {
    connection_index = current_tab->connection_index;
  }

  /* Save current tab first */
  if (ws->num_tabs > 0) {
    tab_save(state);
  }

  /* Create new tab with connection reference */
  Tab *tab = workspace_create_table_tab(ws, connection_index, table_index,
                                        state->tables[table_index]);
  if (!tab) {
    tui_set_error(state, "Failed to create tab");
    return false;
  }

  /* Ensure UITabState capacity for new tab */
  tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                             ws->current_tab);

  /* Initialize UITabState for new tab (now source of truth) */
  UITabState *ui = TUI_TAB_UI(state);
  if (ui) {
    /* Inherit sidebar visibility but reset focus for new tab */
    ui->sidebar_visible = state->sidebar_visible;
    ui->sidebar_focused = false; /* New tab: table has focus, not sidebar */
    ui->sidebar_highlight = state->sidebar_highlight;
    ui->sidebar_scroll = state->sidebar_scroll;
    ui->sidebar_filter_len = state->sidebar_filter_len;
    memcpy(ui->sidebar_filter, state->sidebar_filter,
           sizeof(ui->sidebar_filter));
    ui->sidebar_last_position = table_index;

    /* New tab starts with filters closed */
    ui->filters_visible = false;
    ui->filters_focused = false;
    ui->filters_was_focused = false;
    ui->filters_editing = false;
    ui->filters_cursor_row = 0;
    ui->filters_cursor_col = 0;
    ui->filters_scroll = 0;
  }

  /* Reset TUI state for new tab - all panels start unfocused */
  state->sidebar_focused = false;
  state->filters_visible = false;
  state->filters_focused = false;
  state->filters_was_focused = false;
  state->filters_editing = false;
  state->filters_cursor_row = 0;
  state->filters_cursor_col = 0;
  state->filters_scroll = 0;

  /* Tab cursor/scroll/data is initialized by tui_load_table_data */

  /* Load the table data (updates Tab directly) */
  if (!tui_load_table_data(state, state->tables[table_index])) {
    /* Failed - remove the tab */
    workspace_close_tab(ws, ws->current_tab);

    /* Restore previous tab if any */
    if (ws->num_tabs > 0) {
      tab_restore(state);
    }
    return false;
  }

  /* Tab stores table_index - no cache sync needed */

  /* Initialize widgets for the new tab */
  if (ui) {
    tui_init_table_tab_widgets(state, ui, tab);
  }

  return true;
}

/* Legacy wrapper */
bool workspace_create(TuiState *state, size_t table_index) {
  return tab_create(state, table_index);
}

/* Draw tab bar */
void tui_draw_tabs(TuiState *state) {
  if (!state || !state->tab_win)
    return;

  werase(state->tab_win);
  wbkgd(state->tab_win, COLOR_PAIR(COLOR_BORDER));

  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws) {
    wrefresh(state->tab_win);
    return;
  }

  int x = 0;

  for (size_t i = 0; i < ws->num_tabs; i++) {
    Tab *tab = &ws->tabs[i];
    if (!tab->active)
      continue;

    const char *name = tab->table_name ? tab->table_name : "?";
    int tab_width = (int)strlen(name) + 4; /* " name  " with padding */

    if (x + tab_width > state->term_cols)
      break;

    if (i == ws->current_tab) {
      /* Current tab - highlighted */
      wattron(state->tab_win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
      mvwprintw(state->tab_win, 0, x, " %s ", name);
      wattroff(state->tab_win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    } else {
      /* Inactive tab */
      mvwprintw(state->tab_win, 0, x, " %s ", name);
    }

    x += tab_width;

    /* Tab separator */
    if (i < ws->num_tabs - 1 && x < state->term_cols) {
      wattron(state->tab_win, COLOR_PAIR(COLOR_BORDER));
      mvwaddch(state->tab_win, 0, x - 1, ACS_VLINE);
      wattroff(state->tab_win, COLOR_PAIR(COLOR_BORDER));
    }
  }

  /* Show hint for new tab if sidebar visible */
  if (state->sidebar_focused) {
    const char *hint = "[+] New tab";
    int hint_len = (int)strlen(hint);
    if (state->term_cols - x > hint_len + 2) {
      wattron(state->tab_win, A_DIM);
      mvwprintw(state->tab_win, 0, state->term_cols - hint_len - 1, "%s", hint);
      wattroff(state->tab_win, A_DIM);
    }
  }

  wrefresh(state->tab_win);
}

/* Close current tab */
void tab_close(TuiState *state) {
  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws || ws->num_tabs == 0)
    return;

  /* Cancel any pending background load */
  tui_cancel_background_load(state);

  /* Get indices and tab info before close */
  size_t ws_idx = state->app->current_workspace;
  size_t tab_idx = ws->current_tab;
  size_t old_num_tabs = ws->num_tabs;
  Tab *tab = &ws->tabs[tab_idx];
  TabType closing_type = tab->type;
  size_t conn_idx = tab->connection_index;

  /* Free UITabState resources for the closing tab */
  UITabState *ui = tui_get_tab_ui(state, ws_idx, tab_idx);
  if (ui) {
    /* Clean up widgets */
    tui_cleanup_tab_widgets(ui);

    /* Free legacy resources */
    free(ui->query_result_edit_buf);
    ui->query_result_edit_buf = NULL;
  }

  /* If closing a CONNECTION tab, check if other tabs use this connection */
  if (closing_type == TAB_TYPE_CONNECTION) {
    /* Count other tabs using this connection */
    size_t other_tabs_same_conn = 0;
    for (size_t i = 0; i < ws->num_tabs; i++) {
      if (i == tab_idx)
        continue;
      if (ws->tabs[i].connection_index == conn_idx) {
        other_tabs_same_conn++;
      }
    }

    /* Close the tab first */
    workspace_close_tab(ws, tab_idx);

    /* Shift UITabState entries (with bounds checking) */
    if (old_num_tabs > 0 && state->tab_ui &&
        ws_idx < state->tab_ui_ws_capacity && state->tab_ui[ws_idx] &&
        old_num_tabs <= state->tab_ui_capacity[ws_idx]) {
      for (size_t i = tab_idx; i < old_num_tabs - 1; i++) {
        state->tab_ui[ws_idx][i] = state->tab_ui[ws_idx][i + 1];
      }
      memset(&state->tab_ui[ws_idx][old_num_tabs - 1], 0, sizeof(UITabState));
    }

    if (other_tabs_same_conn > 0) {
      /* Other tabs still use this connection - just restore the next tab */
      if (ws->num_tabs > 0) {
        tab_restore(state);
      }
      return;
    }

    /* No other tabs use this connection - close it */
    /*
     * Clear TuiState pointers BEFORE calling app_close_connection.
     * These point to the same memory as Connection->conn/tables,
     * which will be freed by app_close_connection. Don't free here!
     */
    state->conn = NULL;
    state->tables = NULL;
    state->num_tables = 0;

    /* Unbind sidebar widget from connection before closing to prevent dangling
     * pointer */
    UITabState *ui = TUI_TAB_UI(state);
    if (ui && ui->sidebar_widget) {
      sidebar_widget_bind(ui->sidebar_widget, NULL);
    }

    /* Close the connection in app state - this frees everything */
    app_close_connection(state->app, conn_idx);

    /* Tab data fields are managed per-tab, cleared by workspace_close_tab */
    /* Hide sidebar since there's no connection */
    state->sidebar_visible = false;
    state->sidebar_focused = false;
    tui_recreate_windows(state);

    /* If there are other tabs, restore them */
    if (ws->num_tabs > 0) {
      tab_restore(state);
    } else {
      /* No tabs left - close the workspace */
      workspace_close(state);
    }
    return;
  }

  /* Closing a TABLE or QUERY tab */
  /* Count remaining content tabs FOR THIS CONNECTION (excluding the one being
   * closed) */
  size_t remaining_conn_content_tabs = 0;
  bool has_conn_connection_tab = false;
  for (size_t i = 0; i < ws->num_tabs; i++) {
    if (i == tab_idx)
      continue;
    if (ws->tabs[i].connection_index != conn_idx)
      continue; /* Different connection, skip */
    if (ws->tabs[i].type == TAB_TYPE_CONNECTION) {
      has_conn_connection_tab = true;
    } else {
      remaining_conn_content_tabs++;
    }
  }

  /* Close the tab */
  workspace_close_tab(ws, tab_idx);

  /* Shift UITabState entries to match (with bounds checking) */
  if (old_num_tabs > 0 && state->tab_ui && ws_idx < state->tab_ui_ws_capacity &&
      state->tab_ui[ws_idx] && old_num_tabs <= state->tab_ui_capacity[ws_idx]) {
    for (size_t i = tab_idx; i < old_num_tabs - 1; i++) {
      state->tab_ui[ws_idx][i] = state->tab_ui[ws_idx][i + 1];
    }
    memset(&state->tab_ui[ws_idx][old_num_tabs - 1], 0, sizeof(UITabState));
  }

  /* If this was the last content tab for this connection, create a connection
   * tab (unless close_conn_on_last_tab is enabled, in which case close the
   * connection) */
  if (remaining_conn_content_tabs == 0 && !has_conn_connection_tab) {
    /* Check if we should close connection instead of creating connection tab */
    bool close_conn = state->app->config &&
                      state->app->config->general.close_conn_on_last_tab;

    if (!close_conn) {
      /* Get connection string for display */
      Connection *conn = app_get_connection(state->app, conn_idx);
      const char *connstr = conn ? conn->connstr : NULL;

      /* Create connection tab */
      Tab *conn_tab = workspace_create_connection_tab(ws, conn_idx, connstr);
      if (conn_tab) {
        /* Switch to the new connection tab */
        ws->current_tab = ws->num_tabs - 1;

        /* Connection tab has no table data - Tab fields are clean */

        /* Focus sidebar to select a new table */
        state->sidebar_visible = true;
        state->sidebar_focused = true;
        state->sidebar_highlight = 0;
        state->sidebar_scroll = 0;
        state->sidebar_filter[0] = '\0';
        state->sidebar_filter_len = 0;
        state->sidebar_filter_active = false;
        tui_recreate_windows(state);

        /* Reset filters state */
        state->filters_visible = false;
        state->filters_focused = false;
        state->filters_was_focused = false;
        state->filters_editing = false;
        state->filters_cursor_row = 0;
        state->filters_cursor_col = 0;
        state->filters_scroll = 0;
        return;
      }
    }
    /* If close_conn is true, fall through to close workspace/connection */
  }

  /* If no tabs remain, close the workspace */
  if (ws->num_tabs == 0) {
    workspace_close(state);
  } else {
    /* Restore the now-current tab */
    tab_restore(state);
  }
}

/* Close the current workspace and switch to another or show connect dialog */
void workspace_close(TuiState *state) {
  if (!state || !state->app)
    return;

  size_t ws_idx = state->app->current_workspace;

  /* Cancel any pending background load */
  tui_cancel_background_load(state);

  /* Free UITabState resources for all tabs in this workspace */
  if (ws_idx < state->tab_ui_ws_capacity && state->tab_ui[ws_idx]) {
    for (size_t i = 0; i < state->tab_ui_capacity[ws_idx]; i++) {
      /* Clean up widgets for this tab */
      tui_cleanup_tab_widgets(&state->tab_ui[ws_idx][i]);

      /* Free legacy resources */
      free(state->tab_ui[ws_idx][i].query_result_edit_buf);
      state->tab_ui[ws_idx][i].query_result_edit_buf = NULL;
    }
    free(state->tab_ui[ws_idx]);
    state->tab_ui[ws_idx] = NULL;
    state->tab_ui_capacity[ws_idx] = 0;

    /* Shift UITabState workspace entries down */
    for (size_t i = ws_idx; i < state->tab_ui_ws_capacity - 1; i++) {
      state->tab_ui[i] = state->tab_ui[i + 1];
      state->tab_ui_capacity[i] = state->tab_ui_capacity[i + 1];
    }
    if (state->tab_ui_ws_capacity > 0) {
      state->tab_ui[state->tab_ui_ws_capacity - 1] = NULL;
      state->tab_ui_capacity[state->tab_ui_ws_capacity - 1] = 0;
    }
  }

  /* Close workspace in app state (frees tabs and data) */
  app_close_workspace(state->app, ws_idx);

  /* Tab data fields are freed by app_close_workspace */

  /* Reset UI state */
  state->sidebar_visible = false;
  state->sidebar_focused = false;
  state->sidebar_highlight = 0;
  state->sidebar_scroll = 0;
  state->sidebar_filter[0] = '\0';
  state->sidebar_filter_len = 0;
  state->sidebar_filter_active = false;
  state->filters_visible = false;
  state->filters_focused = false;
  state->filters_was_focused = false;
  state->filters_editing = false;
  state->filters_cursor_row = 0;
  state->filters_cursor_col = 0;
  state->filters_scroll = 0;

  /* Check if there are other workspaces */
  if (state->app->num_workspaces > 0) {
    /* Switch to the new current workspace (adjusted by app_close_workspace) */
    tui_recreate_windows(state);
    tab_restore(state);
  } else {
    /* No workspaces left - show connect dialog */
    tui_recreate_windows(state);
    if (state->sidebar_win) {
      werase(state->sidebar_win);
      wrefresh(state->sidebar_win);
    }
    werase(state->main_win);
    wrefresh(state->main_win);
    tui_refresh(state);
    tui_show_connect_dialog(state);
  }
}
