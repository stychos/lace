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

/* Restore TUI state from tab */
void tab_restore(TuiState *state) {
  /* Add-row mode belongs to the tab being left (its arrays follow that
   * tab's schema); switching by mouse bypasses the add-row keys */
  if (state && state->adding_row) {
    tui_cancel_add_row(state);
  }
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;

  /* Track layout state changes for window recreation */
  bool sidebar_was_visible = state->sidebar_visible;
  bool header_was_visible = state->header_visible;
  bool status_was_visible = state->status_visible;

  /* The new tab may use a different connection */
  tui_bind_connection(state);

  /* Rebind VmTable to new Tab so cursor/scroll reads work correctly */
  if (state->vm_table) {
    table_vm_bind(state->vm_table, tab);
  }

  /* Reset editing state when switching tabs */
  state->filters_editing = false;
  state->filters_cursor_row = 0;
  state->filters_cursor_col = 0;
  state->filters_scroll = 0;
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

/* Switch to a different tab */
void tab_switch(TuiState *state, size_t index) {
  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws || index >= ws->num_tabs)
    return;
  if (index == ws->current_tab)
    return;

  /* Cancel any pending background load */
  tui_cancel_background_load(state);

  /* Switch to new tab */
  workspace_switch_tab(ws, index);

  /* Restore new tab state */
  tab_restore(state);

  /* Clear status message */
  free(state->status_msg);
  state->status_msg = NULL;
  state->status_is_error = false;
}

/* Create a new table tab */
bool tab_create(TuiState *state, size_t table_index) {
  if (!state || !state->app || table_index >= TUI_NUM_TABLES(state))
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

  /* Create new tab with connection reference */
  Tab *tab = workspace_create_table_tab(ws, connection_index, TUI_TABLES(state)[table_index]);
  if (!tab) {
    tui_set_error(state, "Failed to create tab");
    return false;
  }

  /* Ensure UITabState capacity for new tab */
  tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                             ws->current_tab);

  /* Initialize UITabState for new tab */
  UITabState *ui = TUI_TAB_UI(state);
  if (ui) {
    /* New tab starts with filters closed */
    ui->filters_visible = false;
    ui->filters_focused = false;
  }

  /* Update sidebar last position (global state) */
  state->sidebar_last_position = table_index;

  /* Reset TUI state for new tab - all panels start unfocused */
  state->sidebar_focused = false;
  tui_set_filters_visible(state, false);
  tui_set_filters_focused(state, false);
  state->filters_was_focused = false;
  state->filters_editing = false;
  state->filters_cursor_row = 0;
  state->filters_cursor_col = 0;
  state->filters_scroll = 0;

  /* Tab cursor/scroll/data is initialized by tui_load_table_data */

  /* Load the table data (updates Tab directly) */
  if (!tui_load_table_data(state, TUI_TABLES(state)[table_index])) {
    /* Failed - remove the tab */
    workspace_close_tab(ws, ws->current_tab);

    /* Restore previous tab if any */
    if (ws->num_tabs > 0) {
      tab_restore(state);
    }
    return false;
  }


  return true;
}

/* Longest tab label shown, in columns (longer names are clipped) */
#define TAB_LABEL_MAX_COLS 30

int tui_tab_width(const Tab *tab) {
  const char *name = tab->table_name ? tab->table_name : "?";
  int cols = utf8_width(name, strlen(name));
  if (cols > TAB_LABEL_MAX_COLS)
    cols = TAB_LABEL_MAX_COLS;
  return cols + 4; /* " name " plus the separator column */
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
    int tab_width = tui_tab_width(tab); /* Same widths as the mouse hit test */

    if (x + tab_width > state->term_cols)
      break;

    /* " name " in display columns, clipped to the label width */
    attr_t attr = i == ws->current_tab ? COLOR_PAIR(COLOR_SELECTED) | A_BOLD
                                       : A_NORMAL;
    wattron(state->tab_win, attr);
    mvwaddch(state->tab_win, 0, x, ' ');
    tui_print_fit(state->tab_win, 0, x + 1, tab_width - 3, name);
    waddch(state->tab_win, ' ');
    wattroff(state->tab_win, attr);

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
    /* Free resources */
    free(ui->query_result_edit_buf);
    ui->query_result_edit_buf = NULL;
  }

  /* If closing a CONNECTION tab, check if other tabs use this connection */
  if (closing_type == TAB_TYPE_CONNECTION) {
    /* Close the tab first */
    workspace_close_tab(ws, tab_idx);

    /* Count the tabs still using this connection in every workspace:
     * app_close_connection closes all of them, including tabs (and their
     * query text) in workspaces the user is not looking at. */
    size_t other_tabs_same_conn = 0;
    for (size_t w = 0; w < state->app->num_workspaces; w++) {
      Workspace *other = &state->app->workspaces[w];
      for (size_t i = 0; i < other->num_tabs; i++) {
        if (other->tabs[i].connection_index == conn_idx) {
          other_tabs_same_conn++;
        }
      }
    }

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
      } else {
        workspace_close(state);
      }
      return;
    }

    /* No other tabs use this connection - close it */
    state->sidebar_tables_key = NULL;

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
        tui_set_filters_visible(state, false);
        tui_set_filters_focused(state, false);
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
      /* Free resources */
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
  tui_set_filters_visible(state, false);
  tui_set_filters_focused(state, false);
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
