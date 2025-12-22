/*
 * lace - Database Viewer and Manager
 * TUI Workspace/Tab management
 *
 * Core workspace lifecycle (workspace_init, workspace_free_data) is in
 * core/workspace.c. This file contains TUI-specific workspace operations
 * that need to sync UI state or call TUI functions.
 */

#include "tui_internal.h"
#include "../core/workspace.h"
#include <stdlib.h>
#include <string.h>

/* Workspace state field mappings - used by save/restore */
#define WS_COPY_TO_WS(field) ws->field = state->field
#define WS_COPY_TO_STATE(field) state->field = ws->field

/* Save current TUI state to workspace */
void workspace_save(TuiState *state) {
  if (!state || state->num_workspaces == 0)
    return;

  Workspace *ws = &state->workspaces[state->current_workspace];

  /* Cursor and scroll */
  WS_COPY_TO_WS(cursor_row);
  WS_COPY_TO_WS(cursor_col);
  WS_COPY_TO_WS(scroll_row);
  WS_COPY_TO_WS(scroll_col);

  /* Pagination */
  WS_COPY_TO_WS(total_rows);
  WS_COPY_TO_WS(loaded_offset);
  WS_COPY_TO_WS(loaded_count);

  /* Data pointers */
  WS_COPY_TO_WS(data);
  WS_COPY_TO_WS(schema);
  WS_COPY_TO_WS(col_widths);
  WS_COPY_TO_WS(num_col_widths);

  /* Filters panel */
  WS_COPY_TO_WS(filters_visible);
  WS_COPY_TO_WS(filters_focused);
  WS_COPY_TO_WS(filters_cursor_row);
  WS_COPY_TO_WS(filters_cursor_col);
  WS_COPY_TO_WS(filters_scroll);

  /* Sidebar */
  WS_COPY_TO_WS(sidebar_visible);
  WS_COPY_TO_WS(sidebar_focused);
  WS_COPY_TO_WS(sidebar_highlight);
  WS_COPY_TO_WS(sidebar_scroll);
  WS_COPY_TO_WS(sidebar_filter_len);
  memcpy(ws->sidebar_filter, state->sidebar_filter, sizeof(ws->sidebar_filter));

  /* Layout visibility */
  WS_COPY_TO_WS(header_visible);
  WS_COPY_TO_WS(status_visible);
}

/* Restore TUI state from workspace */
void workspace_restore(TuiState *state) {
  if (!state || state->num_workspaces == 0)
    return;

  Workspace *ws = &state->workspaces[state->current_workspace];

  /* Track layout state changes for window recreation */
  bool sidebar_was_visible = state->sidebar_visible;
  bool header_was_visible = state->header_visible;
  bool status_was_visible = state->status_visible;

  /* Cursor and scroll */
  WS_COPY_TO_STATE(cursor_row);
  WS_COPY_TO_STATE(cursor_col);
  WS_COPY_TO_STATE(scroll_row);
  WS_COPY_TO_STATE(scroll_col);

  /* Pagination */
  WS_COPY_TO_STATE(total_rows);
  WS_COPY_TO_STATE(loaded_offset);
  WS_COPY_TO_STATE(loaded_count);

  /* Data pointers */
  WS_COPY_TO_STATE(data);
  WS_COPY_TO_STATE(schema);
  WS_COPY_TO_STATE(col_widths);
  WS_COPY_TO_STATE(num_col_widths);
  state->current_table = ws->table_index;

  /* Filters panel */
  WS_COPY_TO_STATE(filters_visible);
  WS_COPY_TO_STATE(filters_focused);
  WS_COPY_TO_STATE(filters_cursor_row);
  WS_COPY_TO_STATE(filters_cursor_col);
  WS_COPY_TO_STATE(filters_scroll);
  state->filters_editing = false;

  /* Sidebar */
  WS_COPY_TO_STATE(sidebar_visible);
  WS_COPY_TO_STATE(sidebar_focused);
  WS_COPY_TO_STATE(sidebar_highlight);
  WS_COPY_TO_STATE(sidebar_scroll);
  WS_COPY_TO_STATE(sidebar_filter_len);
  memcpy(state->sidebar_filter, ws->sidebar_filter, sizeof(state->sidebar_filter));
  state->sidebar_filter_active = false;

  /* Layout visibility */
  WS_COPY_TO_STATE(header_visible);
  WS_COPY_TO_STATE(status_visible);

  /* Recreate windows if layout changed */
  if (sidebar_was_visible != state->sidebar_visible ||
      header_was_visible != state->header_visible ||
      status_was_visible != state->status_visible) {
    tui_recreate_windows(state);
  }
}

/* Switch to a different workspace */
void workspace_switch(TuiState *state, size_t index) {
  if (!state || !state->app || index >= state->app->num_workspaces)
    return;
  if (index == state->app->current_workspace)
    return;

  AppState *app = state->app;

  /* Cancel any pending background load */
  tui_cancel_background_load(state);

  /* Save current workspace state */
  workspace_save(state);

  /* Switch to new workspace in AppState */
  app->current_workspace = index;
  state->current_workspace = index;

  /* Restore new workspace state */
  workspace_restore(state);

  /* Clear status message */
  free(state->status_msg);
  state->status_msg = NULL;
  state->status_is_error = false;
}

/* Create a new workspace for a table */
bool workspace_create(TuiState *state, size_t table_index) {
  if (!state || !state->app || table_index >= state->num_tables)
    return false;

  AppState *app = state->app;

  if (app->num_workspaces >= MAX_WORKSPACES) {
    tui_set_error(state, "Maximum %d tabs reached", MAX_WORKSPACES);
    return false;
  }

  /* Save current workspace first */
  if (app->num_workspaces > 0) {
    workspace_save(state);
  }

  /* Create new workspace in AppState */
  size_t new_idx = app->num_workspaces;
  Workspace *ws = &app->workspaces[new_idx];
  workspace_init(ws);

  ws->active = true;
  ws->table_index = table_index;
  ws->table_name = str_dup(state->tables[table_index]);

  /* Initialize sidebar state - inherit current state including scroll position */
  ws->sidebar_visible = state->sidebar_visible;
  ws->sidebar_focused = false; /* New workspace starts with table focused */
  ws->sidebar_highlight = state->sidebar_highlight; /* Keep current highlight position */
  ws->sidebar_scroll = state->sidebar_scroll; /* Keep current scroll position */
  memcpy(ws->sidebar_filter, state->sidebar_filter, sizeof(ws->sidebar_filter));
  ws->sidebar_filter_len = state->sidebar_filter_len;

  /* Initialize sidebar last position for navigation restoration */
  state->sidebar_last_position = table_index;

  /* Update AppState */
  app->num_workspaces++;
  app->current_workspace = new_idx;

  /* Sync to TuiState cache */
  state->workspaces = app->workspaces;
  state->num_workspaces = app->num_workspaces;
  state->current_workspace = app->current_workspace;

  /* Clear TUI state for new workspace */
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
    /* Failed - remove the workspace using core function */
    workspace_free_data(ws);
    memset(ws, 0, sizeof(Workspace)); /* Clear all pointers to prevent dangling refs */

    /* Update AppState */
    app->num_workspaces--;

    /* Sync to TuiState cache */
    state->num_workspaces = app->num_workspaces;

    /* Restore previous workspace */
    if (app->num_workspaces > 0) {
      app->current_workspace = app->num_workspaces - 1;
      state->current_workspace = app->current_workspace;
      workspace_restore(state);
    }
    return false;
  }

  /* Save the loaded data to workspace */
  ws->data = state->data;
  ws->schema = state->schema;
  ws->col_widths = state->col_widths;
  ws->num_col_widths = state->num_col_widths;
  ws->total_rows = state->total_rows;
  ws->loaded_offset = state->loaded_offset;
  ws->loaded_count = state->loaded_count;

  state->current_table = table_index;

  return true;
}

/* Draw tab bar */
void tui_draw_tabs(TuiState *state) {
  if (!state || !state->tab_win)
    return;

  werase(state->tab_win);
  wbkgd(state->tab_win, COLOR_PAIR(COLOR_BORDER));

  int x = 0;

  for (size_t i = 0; i < state->num_workspaces; i++) {
    Workspace *ws = &state->workspaces[i];
    if (!ws->active)
      continue;

    const char *name = ws->table_name ? ws->table_name : "?";
    int tab_width = (int)strlen(name) + 4; /* " name  " with padding */

    if (x + tab_width > state->term_cols)
      break;

    if (i == state->current_workspace) {
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
    if (i < state->num_workspaces - 1 && x < state->term_cols) {
      mvwaddch(state->tab_win, 0, x - 1, ACS_VLINE);
    }
  }

  /* Show hint for new tab if space and sidebar visible */
  if (state->num_workspaces < MAX_WORKSPACES && state->sidebar_focused) {
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

/* Close current workspace */
void workspace_close(TuiState *state) {
  if (!state || !state->app || state->app->num_workspaces == 0)
    return;

  AppState *app = state->app;

  /* Cancel any pending background load */
  tui_cancel_background_load(state);

  /* Validate current_workspace is within bounds */
  if (app->current_workspace >= app->num_workspaces)
    return;

  Workspace *ws = &app->workspaces[app->current_workspace];

  /* Free workspace data using core function */
  workspace_free_data(ws);
  memset(ws, 0, sizeof(Workspace));

  /* Shift remaining workspaces down */
  for (size_t i = app->current_workspace; i < app->num_workspaces - 1; i++) {
    app->workspaces[i] = app->workspaces[i + 1];
  }
  memset(&app->workspaces[app->num_workspaces - 1], 0, sizeof(Workspace));

  app->num_workspaces--;

  /* Sync to TuiState cache */
  state->workspaces = app->workspaces;
  state->num_workspaces = app->num_workspaces;

  if (app->num_workspaces == 0) {
    /* Last tab closed - clear state and focus sidebar */
    app->current_workspace = 0;
    state->current_workspace = 0;
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

    /* Reset sidebar state and focus it */
    state->sidebar_focused = true;
    state->sidebar_highlight = 0;
    state->sidebar_scroll = 0;
    state->sidebar_filter[0] = '\0';
    state->sidebar_filter_len = 0;
    state->sidebar_filter_active = false;
  } else {
    /* Adjust current workspace index */
    if (app->current_workspace >= app->num_workspaces) {
      app->current_workspace = app->num_workspaces - 1;
    }
    state->current_workspace = app->current_workspace;

    /* Restore the now-current workspace */
    workspace_restore(state);
  }
}
