/*
 * lace - Database Viewer and Manager
 * Cell editing and row deletion
 */

#include "tui_internal.h"
#include "views/editor_view.h"
#include <stdlib.h>
#include <string.h>

/* Find all primary key column indices */
size_t tui_find_pk_columns(TuiState *state, size_t *pk_indices,
                           size_t max_pks) {
  if (!state || !state->schema || !pk_indices || max_pks == 0)
    return 0;

  size_t count = 0;
  for (size_t i = 0; i < state->schema->num_columns && count < max_pks; i++) {
    if (state->schema->columns[i].primary_key) {
      pk_indices[count++] = i;
    }
  }
  return count;
}

/* Start inline editing */
void tui_start_edit(TuiState *state) {
  if (!state || !state->data || state->editing)
    return;
  if (state->cursor_row >= state->data->num_rows)
    return;
  if (state->cursor_col >= state->data->num_columns)
    return;

  /* Get current cell value */
  DbValue *val = &state->data->rows[state->cursor_row].cells[state->cursor_col];

  /* Convert value to string */
  char *content = NULL;
  if (val->is_null) {
    content = str_dup("");
  } else {
    content = db_value_to_string(val);
    if (!content)
      content = str_dup("");
  }

  /* Check if content is truncated (exceeds column width) */
  int col_width = tui_get_column_width(state, state->cursor_col);
  size_t content_len = content ? strlen(content) : 0;
  bool is_truncated = content_len > (size_t)col_width;

  /* Also check if content has newlines (always use modal for multi-line) */
  bool has_newlines = content && strchr(content, '\n') != NULL;

  if (is_truncated || has_newlines) {
    /* Use modal editor for truncated or multi-line content */
    const char *col_name = state->data->columns[state->cursor_col].name;
    char *title = str_printf("Edit: %s", col_name);

    EditorResult result =
        editor_view_show(state, title ? title : "Edit Cell", content, false);
    free(title);

    if (result.saved) {
      /* Update the cell with new content (or NULL) */
      free(state->edit_buffer);
      state->edit_buffer = result.set_null ? NULL : result.content;
      state->editing = true; /* Required for tui_confirm_edit */
      tui_confirm_edit(state);
    } else {
      free(result.content);
    }

    free(content);
  } else {
    /* Use inline editing for short content */
    free(state->edit_buffer);
    state->edit_buffer = content;
    state->edit_pos = state->edit_buffer ? strlen(state->edit_buffer) : 0;
    state->editing = true;
    curs_set(1); /* Show cursor */
  }
}

/* Start modal editing */
void tui_start_modal_edit(TuiState *state) {
  if (!state || !state->data || state->editing)
    return;
  if (state->cursor_row >= state->data->num_rows)
    return;
  if (state->cursor_col >= state->data->num_columns)
    return;

  /* Get current cell value */
  DbValue *val = &state->data->rows[state->cursor_row].cells[state->cursor_col];

  /* Convert value to string */
  char *content = NULL;
  if (val->is_null) {
    content = str_dup("");
  } else {
    content = db_value_to_string(val);
    if (!content)
      content = str_dup("");
  }

  /* Always use modal editor */
  const char *col_name = state->data->columns[state->cursor_col].name;
  char *title = str_printf("Edit: %s", col_name);

  EditorResult result =
      editor_view_show(state, title ? title : "Edit Cell", content, false);
  free(title);

  if (result.saved) {
    /* Update the cell with new content (or NULL) */
    free(state->edit_buffer);
    state->edit_buffer = result.set_null ? NULL : result.content;
    state->editing = true; /* Required for tui_confirm_edit */
    tui_confirm_edit(state);
  } else {
    free(result.content);
  }

  free(content);
}

/* Cancel editing */
void tui_cancel_edit(TuiState *state) {
  if (!state)
    return;

  free(state->edit_buffer);
  state->edit_buffer = NULL;
  state->edit_pos = 0;
  state->editing = false;
  curs_set(0); /* Hide cursor */
}

/* Confirm edit and update database */
void tui_confirm_edit(TuiState *state) {
  if (!state || !state->editing || !state->data || !state->conn) {
    tui_cancel_edit(state);
    return;
  }
  if (!state->tables || state->num_tables == 0) {
    tui_cancel_edit(state);
    return;
  }

  /* Find all primary key columns */
  size_t pk_indices[16]; /* Support up to 16 PK columns */
  size_t num_pk = tui_find_pk_columns(state, pk_indices, 16);
  if (num_pk == 0) {
    tui_set_error(state, "Cannot update: no primary key found");
    tui_cancel_edit(state);
    return;
  }

  /* Verify all PK indices are within data bounds */
  for (size_t i = 0; i < num_pk; i++) {
    if (pk_indices[i] >= state->data->num_columns ||
        pk_indices[i] >= state->data->rows[state->cursor_row].num_cells) {
      tui_set_error(state, "Primary key column index out of bounds");
      tui_cancel_edit(state);
      return;
    }
  }

  /* Get the current table name */
  const char *table = state->tables[state->current_table];

  /* Build arrays of primary key column names and values */
  const char **pk_col_names = malloc(sizeof(char *) * num_pk);
  DbValue *pk_vals = malloc(sizeof(DbValue) * num_pk);
  if (!pk_col_names || !pk_vals) {
    free(pk_col_names);
    free(pk_vals);
    tui_set_error(state, "Memory allocation failed");
    tui_cancel_edit(state);
    return;
  }

  for (size_t i = 0; i < num_pk; i++) {
    pk_col_names[i] = state->data->columns[pk_indices[i]].name;
    /* Deep copy the value to avoid issues with shared pointers */
    pk_vals[i] = db_value_copy(
        &state->data->rows[state->cursor_row].cells[pk_indices[i]]);
  }

  /* Get the column being edited */
  const char *col_name = state->data->columns[state->cursor_col].name;

  /* Create new value from edit buffer */
  DbValue new_val;
  if (state->edit_buffer == NULL || state->edit_buffer[0] == '\0') {
    /* Empty string = NULL */
    new_val = db_value_null();
  } else {
    new_val = db_value_text(state->edit_buffer);
  }

  /* Attempt to update */
  char *err = NULL;
  bool success = db_update_cell(state->conn, table, pk_col_names, pk_vals,
                                num_pk, col_name, &new_val, &err);

  free(pk_col_names);
  for (size_t i = 0; i < num_pk; i++) {
    db_value_free(&pk_vals[i]);
  }
  free(pk_vals);

  if (success) {
    /* Update the local data */
    DbValue *cell =
        &state->data->rows[state->cursor_row].cells[state->cursor_col];
    db_value_free(cell);
    *cell = new_val;
    tui_set_status(state, "Cell updated");
  } else {
    db_value_free(&new_val);
    tui_set_error(state, "Update failed: %s", err ? err : "unknown error");
    free(err);
  }

  tui_cancel_edit(state);
}

/* Set cell value directly (NULL or empty string) */
void tui_set_cell_direct(TuiState *state, bool set_null) {
  if (!state || !state->data || !state->conn)
    return;
  if (!state->tables || state->num_tables == 0)
    return;
  if (state->cursor_row >= state->data->num_rows)
    return;
  if (state->cursor_col >= state->data->num_columns)
    return;

  /* Find all primary key columns */
  size_t pk_indices[16]; /* Support up to 16 PK columns */
  size_t num_pk = tui_find_pk_columns(state, pk_indices, 16);
  if (num_pk == 0) {
    tui_set_error(state, "Cannot update: no primary key found");
    return;
  }

  /* Get the current table name */
  const char *table = state->tables[state->current_table];

  /* Verify all PK indices are within data bounds */
  for (size_t i = 0; i < num_pk; i++) {
    if (pk_indices[i] >= state->data->num_columns ||
        pk_indices[i] >= state->data->rows[state->cursor_row].num_cells) {
      tui_set_error(state, "Primary key column index out of bounds");
      return;
    }
  }

  /* Build arrays of primary key column names and values */
  const char **pk_col_names = malloc(sizeof(char *) * num_pk);
  DbValue *pk_vals = malloc(sizeof(DbValue) * num_pk);
  if (!pk_col_names || !pk_vals) {
    free(pk_col_names);
    free(pk_vals);
    tui_set_error(state, "Memory allocation failed");
    return;
  }

  for (size_t i = 0; i < num_pk; i++) {
    pk_col_names[i] = state->data->columns[pk_indices[i]].name;
    /* Deep copy the value to avoid issues with shared pointers */
    pk_vals[i] = db_value_copy(
        &state->data->rows[state->cursor_row].cells[pk_indices[i]]);
  }

  /* Get the column being edited */
  const char *col_name = state->data->columns[state->cursor_col].name;

  /* Create new value */
  DbValue new_val;
  if (set_null) {
    new_val = db_value_null();
  } else {
    new_val = db_value_text("");
  }

  /* Attempt to update */
  char *err = NULL;
  bool success = db_update_cell(state->conn, table, pk_col_names, pk_vals,
                                num_pk, col_name, &new_val, &err);

  free(pk_col_names);
  for (size_t i = 0; i < num_pk; i++) {
    db_value_free(&pk_vals[i]);
  }
  free(pk_vals);

  if (success) {
    /* Update the local data */
    DbValue *cell =
        &state->data->rows[state->cursor_row].cells[state->cursor_col];
    db_value_free(cell);
    *cell = new_val;
    tui_set_status(state, set_null ? "Cell set to NULL" : "Cell set to empty");
  } else {
    db_value_free(&new_val);
    tui_set_error(state, "Update failed: %s", err ? err : "unknown error");
    free(err);
  }
}

/* Delete current row */
void tui_delete_row(TuiState *state) {
  if (!state || !state->data || !state->conn)
    return;
  if (!state->tables || state->num_tables == 0)
    return;
  if (state->cursor_row >= state->data->num_rows)
    return;

  /* Find all primary key columns */
  size_t pk_indices[16]; /* Support up to 16 PK columns */
  size_t num_pk = tui_find_pk_columns(state, pk_indices, 16);
  if (num_pk == 0) {
    tui_set_error(state, "Cannot delete: no primary key found");
    return;
  }

  /* Verify all PK indices are within data bounds */
  for (size_t i = 0; i < num_pk; i++) {
    if (pk_indices[i] >= state->data->num_columns ||
        pk_indices[i] >= state->data->rows[state->cursor_row].num_cells) {
      tui_set_error(state, "Primary key column index out of bounds");
      return;
    }
  }

  /* Get the current table name */
  const char *table = state->tables[state->current_table];

  /* Build arrays of primary key column names and values */
  const char **pk_col_names = malloc(sizeof(char *) * num_pk);
  DbValue *pk_vals = malloc(sizeof(DbValue) * num_pk);
  if (!pk_col_names || !pk_vals) {
    free(pk_col_names);
    free(pk_vals);
    tui_set_error(state, "Memory allocation failed");
    return;
  }

  for (size_t i = 0; i < num_pk; i++) {
    pk_col_names[i] = state->data->columns[pk_indices[i]].name;
    /* Deep copy the value to avoid issues with shared pointers */
    pk_vals[i] = db_value_copy(
        &state->data->rows[state->cursor_row].cells[pk_indices[i]]);
  }

  /* Highlight the row being deleted with danger background */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);
  (void)win_rows; /* unused */

  /* Calculate screen Y position: 3 header rows + (cursor_row - scroll_row) */
  int row_y = 3 + (int)(state->cursor_row - state->scroll_row);

  /* Draw the entire row with danger background */
  Row *del_row = &state->data->rows[state->cursor_row];
  wattron(state->main_win, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
  int x = 1;
  for (size_t col = state->scroll_col;
       col < state->data->num_columns && col < del_row->num_cells; col++) {
    int col_width = tui_get_column_width(state, col);
    if (x + col_width + 3 > win_cols)
      break;

    DbValue *val = &del_row->cells[col];
    if (val->is_null) {
      mvwprintw(state->main_win, row_y, x, "%-*s", col_width, "NULL");
    } else {
      char *str = db_value_to_string(val);
      if (str) {
        char *safe = tui_sanitize_for_display(str);
        mvwprintw(state->main_win, row_y, x, "%-*.*s", col_width, col_width,
                  safe ? safe : str);
        free(safe);
        free(str);
      }
    }
    x += col_width + 1;
    mvwaddch(state->main_win, row_y, x - 1, ACS_VLINE);
  }
  wattroff(state->main_win, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
  wrefresh(state->main_win);

  /* Show confirmation dialog */
  int height = 7;
  int width = 50;
  int starty = (state->term_rows - height) / 2;
  int startx = (state->term_cols - width) / 2;

  WINDOW *confirm_win = newwin(height, width, starty, startx);
  if (!confirm_win) {
    free(pk_col_names);
    for (size_t i = 0; i < num_pk; i++) {
      db_value_free(&pk_vals[i]);
    }
    free(pk_vals);
    return;
  }

  box(confirm_win, 0, 0);
  wattron(confirm_win, A_BOLD | COLOR_PAIR(COLOR_ERROR));
  mvwprintw(confirm_win, 0, (width - 18) / 2, " Delete Row ");
  wattroff(confirm_win, A_BOLD | COLOR_PAIR(COLOR_ERROR));

  mvwprintw(confirm_win, 2, 2, "Are you sure you want to delete this row?");
  mvwprintw(confirm_win, 4, 2, "[Enter/y] Delete    [n/Esc] Cancel");

  wrefresh(confirm_win);

  int ch = wgetch(confirm_win);
  delwin(confirm_win);
  touchwin(stdscr);
  tui_refresh(state);

  if (ch != 'y' && ch != 'Y' && ch != '\n' && ch != KEY_ENTER) {
    tui_set_status(state, "Delete cancelled");
    free(pk_col_names);
    for (size_t i = 0; i < num_pk; i++) {
      db_value_free(&pk_vals[i]);
    }
    free(pk_vals);
    return;
  }

  /* Calculate absolute row position before delete */
  size_t abs_row = state->loaded_offset + state->cursor_row;
  size_t saved_col = state->cursor_col;
  size_t saved_scroll_col = state->scroll_col;
  /* Save visual offset (how far cursor is from top of visible area) */
  size_t visual_offset = state->cursor_row >= state->scroll_row
                             ? state->cursor_row - state->scroll_row
                             : 0;

  /* Perform the delete */
  char *err = NULL;
  bool success =
      db_delete_row(state->conn, table, pk_col_names, pk_vals, num_pk, &err);

  free(pk_col_names);
  for (size_t i = 0; i < num_pk; i++) {
    db_value_free(&pk_vals[i]);
  }
  free(pk_vals);

  if (success) {
    tui_set_status(state, "Row deleted");

    /* Update total row count */
    if (state->total_rows > 0) {
      state->total_rows--;
    }

    /* Adjust target row if we deleted the last row */
    if (abs_row >= state->total_rows && state->total_rows > 0) {
      abs_row = state->total_rows - 1;
    }

    /* Calculate which page to load */
    size_t target_offset = (abs_row / PAGE_SIZE) * PAGE_SIZE;

    /* Reload data at the appropriate offset */
    tui_load_rows_at(state, target_offset);

    /* Set cursor position relative to loaded data */
    if (state->data && state->data->num_rows > 0) {
      state->cursor_row = abs_row - state->loaded_offset;
      if (state->cursor_row >= state->data->num_rows) {
        state->cursor_row = state->data->num_rows - 1;
      }
      state->cursor_col = saved_col;
      state->scroll_col = saved_scroll_col;

      /* Restore scroll position to maintain visual offset */
      if (state->cursor_row >= visual_offset) {
        state->scroll_row = state->cursor_row - visual_offset;
      } else {
        state->scroll_row = 0;
      }
    }
  } else {
    tui_set_error(state, "Delete failed: %s", err ? err : "unknown error");
    free(err);
  }
}

/* Handle edit mode input */
bool tui_handle_edit_input(TuiState *state, int ch) {
  if (!state->editing)
    return false;

  size_t len = state->edit_buffer ? strlen(state->edit_buffer) : 0;

  switch (ch) {
  case 27: /* Escape - cancel */
    tui_cancel_edit(state);
    return true;

  case '\n':
  case KEY_ENTER:
    tui_confirm_edit(state);
    return true;

  case KEY_LEFT:
    if (state->edit_pos > 0) {
      state->edit_pos--;
    }
    return true;

  case KEY_RIGHT:
    if (state->edit_pos < len) {
      state->edit_pos++;
    }
    return true;

  case KEY_HOME:
  case 1: /* Ctrl+A */
    state->edit_pos = 0;
    return true;

  case KEY_END:
  case 5: /* Ctrl+E */
    state->edit_pos = len;
    return true;

  case KEY_BACKSPACE:
  case 127:
  case 8:
    if (state->edit_pos > 0 && state->edit_buffer) {
      memmove(state->edit_buffer + state->edit_pos - 1,
              state->edit_buffer + state->edit_pos, len - state->edit_pos + 1);
      state->edit_pos--;
    }
    return true;

  case KEY_DC: /* Delete */
    if (state->edit_pos < len && state->edit_buffer) {
      memmove(state->edit_buffer + state->edit_pos,
              state->edit_buffer + state->edit_pos + 1, len - state->edit_pos);
    }
    return true;

  case 21: /* Ctrl+U - clear line */
    if (state->edit_buffer) {
      state->edit_buffer[0] = '\0';
      state->edit_pos = 0;
    }
    return true;

  case 14: /* Ctrl+N - set to NULL */
    free(state->edit_buffer);
    state->edit_buffer = NULL;
    state->edit_pos = 0;
    tui_confirm_edit(state);
    return true;

  case 4: /* Ctrl+D - set to empty string */
    free(state->edit_buffer);
    state->edit_buffer = str_dup("");
    state->edit_pos = 0;
    tui_confirm_edit(state);
    return true;

  default:
    if (ch >= 32 && ch < 127) {
      /* Insert character */
      size_t new_len = len + 2;
      char *new_buf = realloc(state->edit_buffer, new_len);
      if (new_buf) {
        state->edit_buffer = new_buf;
        if (len == 0) {
          /* Buffer was NULL or empty - initialize it */
          state->edit_buffer[0] = (char)ch;
          state->edit_buffer[1] = '\0';
          state->edit_pos = 1;
        } else {
          memmove(state->edit_buffer + state->edit_pos + 1,
                  state->edit_buffer + state->edit_pos,
                  len - state->edit_pos + 1);
          state->edit_buffer[state->edit_pos] = (char)ch;
          state->edit_pos++;
        }
      }
    }
    return true;
  }

  return false;
}
