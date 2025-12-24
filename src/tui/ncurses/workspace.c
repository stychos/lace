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

#include "../../core/workspace.h"
#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>

/* Tab state field mappings - used by save/restore */
#define TAB_COPY_TO_TAB(field) tab->field = state->field
#define TAB_COPY_TO_STATE(field) state->field = tab->field

/* Save current TUI state to tab */
void tab_save(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;

  /* Cursor and scroll */
  TAB_COPY_TO_TAB(cursor_row);
  TAB_COPY_TO_TAB(cursor_col);
  TAB_COPY_TO_TAB(scroll_row);
  TAB_COPY_TO_TAB(scroll_col);

  /* Pagination */
  TAB_COPY_TO_TAB(total_rows);
  TAB_COPY_TO_TAB(loaded_offset);
  TAB_COPY_TO_TAB(loaded_count);
  TAB_COPY_TO_TAB(row_count_approximate);
  TAB_COPY_TO_TAB(unfiltered_total_rows);

  /* Data pointers */
  TAB_COPY_TO_TAB(data);
  TAB_COPY_TO_TAB(schema);
  TAB_COPY_TO_TAB(col_widths);
  TAB_COPY_TO_TAB(num_col_widths);

  /* UI state to UITabState (source of truth) */
  UITabState *ui = TUI_TAB_UI(state);
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

  /* Update connection state from tab's connection_index */
  if (state->app) {
    Connection *conn = app_get_connection(state->app, tab->connection_index);
    if (conn && conn->active) {
      state->conn = conn->conn;
      state->tables = conn->tables;
      state->num_tables = conn->num_tables;
    }
  }

  /* Cursor and scroll */
  TAB_COPY_TO_STATE(cursor_row);
  TAB_COPY_TO_STATE(cursor_col);
  TAB_COPY_TO_STATE(scroll_row);
  TAB_COPY_TO_STATE(scroll_col);

  /* Pagination */
  TAB_COPY_TO_STATE(total_rows);
  TAB_COPY_TO_STATE(loaded_offset);
  TAB_COPY_TO_STATE(loaded_count);
  TAB_COPY_TO_STATE(row_count_approximate);
  TAB_COPY_TO_STATE(unfiltered_total_rows);

  /* Data pointers */
  TAB_COPY_TO_STATE(data);
  TAB_COPY_TO_STATE(schema);
  TAB_COPY_TO_STATE(col_widths);
  TAB_COPY_TO_STATE(num_col_widths);
  state->current_table = tab->table_index;

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

  if (ws->num_tabs >= MAX_TABS) {
    tui_set_error(state, "Maximum %d tabs reached", MAX_TABS);
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

  /* Reset TUI state cache for new tab - all panels start unfocused */
  state->sidebar_focused = false;
  state->filters_visible = false;
  state->filters_focused = false;
  state->filters_was_focused = false;
  state->filters_editing = false;
  state->filters_cursor_row = 0;
  state->filters_cursor_col = 0;
  state->filters_scroll = 0;

  /* Clear data state for new tab */
  state->data = NULL;
  state->schema = NULL;
  state->col_widths = NULL;
  state->num_col_widths = 0;
  state->cursor_row = 0;
  state->cursor_col = 0;
  state->scroll_row = 0;
  state->scroll_col = 0;

  /* Load the table data */
  if (!tui_load_table_data(state, state->tables[table_index])) {
    /* Failed - remove the tab */
    workspace_close_tab(ws, ws->current_tab);

    /* Restore previous tab if any */
    if (ws->num_tabs > 0) {
      tab_restore(state);
    }
    return false;
  }

  /* Save the loaded data to tab */
  tab->data = state->data;
  tab->schema = state->schema;
  tab->col_widths = state->col_widths;
  tab->num_col_widths = state->num_col_widths;
  tab->total_rows = state->total_rows;
  tab->loaded_offset = state->loaded_offset;
  tab->loaded_count = state->loaded_count;

  state->current_table = table_index;

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
      mvwaddch(state->tab_win, 0, x - 1, ACS_VLINE);
    }
  }

  /* Show hint for new tab if space and sidebar visible */
  if (ws->num_tabs < MAX_TABS && state->sidebar_focused) {
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

  /* Get indices before close */
  size_t ws_idx = state->app->current_workspace;
  size_t tab_idx = ws->current_tab;
  size_t old_num_tabs = ws->num_tabs;

  /* Free UITabState resources for the closing tab */
  UITabState *ui = tui_get_tab_ui(state, ws_idx, tab_idx);
  if (ui) {
    free(ui->query_result_edit_buf);
    ui->query_result_edit_buf = NULL;
  }

  /* Close the tab (shifts tabs array) */
  workspace_close_tab(ws, tab_idx);

  /* Shift UITabState entries to match (workspace_close_tab shifts tabs) */
  for (size_t i = tab_idx; i < old_num_tabs - 1; i++) {
    state->tab_ui[ws_idx][i] = state->tab_ui[ws_idx][i + 1];
  }
  /* Clear the last entry */
  memset(&state->tab_ui[ws_idx][old_num_tabs - 1], 0, sizeof(UITabState));

  if (ws->num_tabs == 0) {
    /* Last tab closed - clear all state */
    state->data = NULL;
    state->schema = NULL;
    state->col_widths = NULL;
    state->num_col_widths = 0;
    state->cursor_row = 0;
    state->cursor_col = 0;
    state->scroll_row = 0;
    state->scroll_col = 0;
    state->total_rows = 0;
    state->loaded_offset = 0;
    state->loaded_count = 0;
    state->current_table = 0;

    /*
     * Note: We keep connection state (conn, tables, num_tables) intact
     * because the sidebar still needs to display tables for opening new tabs.
     * Connection state is only cleared when the connection itself is closed.
     */

    /* Reset sidebar state and focus it */
    state->sidebar_focused = true;
    state->sidebar_highlight = 0;
    state->sidebar_scroll = 0;
    state->sidebar_filter[0] = '\0';
    state->sidebar_filter_len = 0;
    state->sidebar_filter_active = false;

    /* Reset filters state */
    state->filters_visible = false;
    state->filters_focused = false;
    state->filters_was_focused = false;
    state->filters_editing = false;
    state->filters_cursor_row = 0;
    state->filters_cursor_col = 0;
    state->filters_scroll = 0;
  } else {
    /* Restore the now-current tab */
    tab_restore(state);
  }
}

/* Legacy wrapper */
void workspace_close(TuiState *state) { tab_close(state); }
