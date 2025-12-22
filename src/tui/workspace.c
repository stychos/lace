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
}

/* Switch to a different workspace */
void workspace_switch(TuiState *state, size_t index) {
  if (!state || index >= state->num_workspaces)
    return;
  if (index == state->current_workspace)
    return;

  /* Save current workspace state */
  workspace_save(state);

  /* Switch to new workspace */
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
  if (!state || table_index >= state->num_tables)
    return false;
  if (state->num_workspaces >= MAX_WORKSPACES) {
    tui_set_error(state, "Maximum %d tabs reached", MAX_WORKSPACES);
    return false;
  }

  /* Save current workspace first */
  if (state->num_workspaces > 0) {
    workspace_save(state);
  }

  /* Create new workspace */
  size_t new_idx = state->num_workspaces;
  Workspace *ws = &state->workspaces[new_idx];
  memset(ws, 0, sizeof(Workspace));

  ws->active = true;
  ws->table_index = table_index;
  ws->table_name = str_dup(state->tables[table_index]);
  filters_init(&ws->filters);

  state->num_workspaces++;
  state->current_workspace = new_idx;

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
    memset(ws, 0, sizeof(Workspace)); /* Clear all pointers to prevent dangling refs */
    state->num_workspaces--;

    /* Restore previous workspace */
    if (state->num_workspaces > 0) {
      state->current_workspace = state->num_workspaces - 1;
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
  if (!state || state->num_workspaces == 0)
    return;

  /* Validate current_workspace is within bounds */
  if (state->current_workspace >= state->num_workspaces)
    return;

  Workspace *ws = &state->workspaces[state->current_workspace];

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
  for (size_t i = state->current_workspace; i < state->num_workspaces - 1;
       i++) {
    state->workspaces[i] = state->workspaces[i + 1];
  }
  memset(&state->workspaces[state->num_workspaces - 1], 0, sizeof(Workspace));

  state->num_workspaces--;

  if (state->num_workspaces == 0) {
    /* Last tab closed - clear state and focus sidebar */
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
    state->sidebar_focused = true;
    state->sidebar_highlight = 0;
  } else {
    /* Adjust current workspace index */
    if (state->current_workspace >= state->num_workspaces) {
      state->current_workspace = state->num_workspaces - 1;
    }

    /* Restore the now-current workspace */
    workspace_restore(state);
  }
}
