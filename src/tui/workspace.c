/*
 * lace - Database Viewer and Manager
 * Workspace/Tab management
 */

#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>

/* Save current TUI state to workspace */
void workspace_save(TuiState *state) {
  if (!state || state->num_workspaces == 0)
    return;

  Workspace *ws = &state->workspaces[state->current_workspace];

  /* Save cursor and scroll positions */
  ws->cursor_row = state->cursor_row;
  ws->cursor_col = state->cursor_col;
  ws->scroll_row = state->scroll_row;
  ws->scroll_col = state->scroll_col;

  /* Save pagination state */
  ws->total_rows = state->total_rows;
  ws->loaded_offset = state->loaded_offset;
  ws->loaded_count = state->loaded_count;

  /* Data and schema pointers are already stored in workspace */
  ws->data = state->data;
  ws->schema = state->schema;

  /* Save column widths */
  ws->col_widths = state->col_widths;
  ws->num_col_widths = state->num_col_widths;

  /* Save filters panel state */
  ws->filters_visible = state->filters_visible;
  ws->filters_focused = state->filters_focused;
  ws->filters_cursor_row = state->filters_cursor_row;
  ws->filters_cursor_col = state->filters_cursor_col;
  ws->filters_scroll = state->filters_scroll;

  /* Save sidebar state */
  ws->sidebar_visible = state->sidebar_visible;
  ws->sidebar_focused = state->sidebar_focused;
  ws->sidebar_highlight = state->sidebar_highlight;
  ws->sidebar_scroll = state->sidebar_scroll;
  memcpy(ws->sidebar_filter, state->sidebar_filter, sizeof(ws->sidebar_filter));
  ws->sidebar_filter_len = state->sidebar_filter_len;
}

/* Restore TUI state from workspace */
void workspace_restore(TuiState *state) {
  if (!state || state->num_workspaces == 0)
    return;

  Workspace *ws = &state->workspaces[state->current_workspace];

  /* Restore cursor and scroll positions */
  state->cursor_row = ws->cursor_row;
  state->cursor_col = ws->cursor_col;
  state->scroll_row = ws->scroll_row;
  state->scroll_col = ws->scroll_col;

  /* Restore pagination state */
  state->total_rows = ws->total_rows;
  state->loaded_offset = ws->loaded_offset;
  state->loaded_count = ws->loaded_count;

  /* Restore data and schema */
  state->data = ws->data;
  state->schema = ws->schema;
  state->current_table = ws->table_index;

  /* Restore column widths */
  state->col_widths = ws->col_widths;
  state->num_col_widths = ws->num_col_widths;

  /* Restore filters panel state */
  state->filters_visible = ws->filters_visible;
  state->filters_focused = ws->filters_focused;
  state->filters_cursor_row = ws->filters_cursor_row;
  state->filters_cursor_col = ws->filters_cursor_col;
  state->filters_scroll = ws->filters_scroll;
  state->filters_editing = false; /* Always reset editing state */

  /* Restore sidebar state */
  bool sidebar_was_visible = state->sidebar_visible;
  state->sidebar_visible = ws->sidebar_visible;
  state->sidebar_focused = ws->sidebar_focused;
  state->sidebar_highlight = ws->sidebar_highlight;
  state->sidebar_scroll = ws->sidebar_scroll;
  memcpy(state->sidebar_filter, ws->sidebar_filter, sizeof(state->sidebar_filter));
  state->sidebar_filter_len = ws->sidebar_filter_len;
  state->sidebar_filter_active = false; /* Always reset filter input mode */

  /* Recreate windows if sidebar visibility changed */
  if (sidebar_was_visible != state->sidebar_visible) {
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
  memset(ws, 0, sizeof(Workspace));

  ws->active = true;
  ws->table_index = table_index;
  ws->table_name = str_dup(state->tables[table_index]);
  filters_init(&ws->filters);

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
    /* Failed - remove the workspace and clear all its fields */
    free(ws->table_name);
    filters_free(&ws->filters);
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

  /* Validate current_workspace is within bounds */
  if (app->current_workspace >= app->num_workspaces)
    return;

  Workspace *ws = &app->workspaces[app->current_workspace];

  /* Free workspace data */
  free(ws->table_name);
  db_result_free(ws->data);
  db_schema_free(ws->schema);
  free(ws->col_widths);
  filters_free(&ws->filters);

  /* Free query-specific data */
  free(ws->query_text);
  db_result_free(ws->query_results);
  free(ws->query_error);
  free(ws->query_result_col_widths);
  free(ws->query_result_edit_buf);
  free(ws->query_source_table);
  db_schema_free(ws->query_source_schema);
  free(ws->query_base_sql);

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
