/*
 * lace - Database Viewer and Manager
 * Cell editing and row deletion
 */

#include "tui_internal.h"
#include "views/editor_view.h"
#include <stdlib.h>
#include <string.h>

/* Primary key info for database operations */
typedef struct {
  const char **col_names; /* Array of PK column names (not owned) */
  DbValue *values;        /* Array of PK values (owned, must be freed) */
  size_t count;           /* Number of PK columns */
} PkInfo;

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

/* Build PK info from result set row. Returns false on error. */
static bool pk_info_build(PkInfo *pk, ResultSet *data, size_t row_idx,
                          TableSchema *schema) {
  if (!pk || !data || !schema || row_idx >= data->num_rows)
    return false;

  /* Find PK column indices */
  size_t pk_indices[16];
  size_t num_pk = 0;
  for (size_t i = 0; i < schema->num_columns && num_pk < 16; i++) {
    if (schema->columns[i].primary_key) {
      pk_indices[num_pk++] = i;
    }
  }

  if (num_pk == 0)
    return false;

  /* Verify indices are within bounds */
  Row *row = &data->rows[row_idx];
  for (size_t i = 0; i < num_pk; i++) {
    if (pk_indices[i] >= data->num_columns || pk_indices[i] >= row->num_cells) {
      return false;
    }
  }

  /* Allocate arrays */
  pk->col_names = malloc(sizeof(char *) * num_pk);
  pk->values = malloc(sizeof(DbValue) * num_pk);
  if (!pk->col_names || !pk->values) {
    free(pk->col_names);
    free(pk->values);
    pk->col_names = NULL;
    pk->values = NULL;
    return false;
  }

  /* Fill arrays */
  for (size_t i = 0; i < num_pk; i++) {
    pk->col_names[i] = data->columns[pk_indices[i]].name;
    pk->values[i] = db_value_copy(&row->cells[pk_indices[i]]);
  }
  pk->count = num_pk;

  return true;
}

/* Free PK info resources */
static void pk_info_free(PkInfo *pk) {
  if (!pk)
    return;
  free(pk->col_names);
  if (pk->values) {
    for (size_t i = 0; i < pk->count; i++) {
      db_value_free(&pk->values[i]);
    }
    free(pk->values);
  }
  pk->col_names = NULL;
  pk->values = NULL;
  pk->count = 0;
}

/* Start inline editing */
void tui_start_edit(TuiState *state) {
  if (!state || !state->data || state->editing)
    return;
  if (state->cursor_row >= state->data->num_rows)
    return;
  if (state->cursor_col >= state->data->num_columns)
    return;
  if (!state->data->rows)
    return;

  Row *row = &state->data->rows[state->cursor_row];
  if (!row->cells || state->cursor_col >= row->num_cells)
    return;

  /* Get current cell value */
  DbValue *val = &row->cells[state->cursor_col];

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
  if (!state->data->rows)
    return;

  Row *row = &state->data->rows[state->cursor_row];
  if (!row->cells || state->cursor_col >= row->num_cells)
    return;

  /* Get current cell value */
  DbValue *val = &row->cells[state->cursor_col];

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

  /* Build primary key info */
  PkInfo pk = {0};
  if (!pk_info_build(&pk, state->data, state->cursor_row, state->schema)) {
    tui_set_error(state, "Cannot update: no primary key found");
    tui_cancel_edit(state);
    return;
  }

  /* Get the current table name */
  const char *table = state->tables[state->current_table];
  const char *col_name = state->data->columns[state->cursor_col].name;

  /* Create new value from edit buffer */
  DbValue new_val;
  if (state->edit_buffer == NULL || state->edit_buffer[0] == '\0') {
    new_val = db_value_null();
  } else {
    new_val = db_value_text(state->edit_buffer);
  }

  /* Attempt to update */
  char *err = NULL;
  bool success = db_update_cell(state->conn, table, pk.col_names, pk.values,
                                pk.count, col_name, &new_val, &err);
  pk_info_free(&pk);

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

  /* Build primary key info */
  PkInfo pk = {0};
  if (!pk_info_build(&pk, state->data, state->cursor_row, state->schema)) {
    tui_set_error(state, "Cannot update: no primary key found");
    return;
  }

  const char *table = state->tables[state->current_table];
  const char *col_name = state->data->columns[state->cursor_col].name;
  DbValue new_val = set_null ? db_value_null() : db_value_text("");

  /* Attempt to update */
  char *err = NULL;
  bool success = db_update_cell(state->conn, table, pk.col_names, pk.values,
                                pk.count, col_name, &new_val, &err);
  pk_info_free(&pk);

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

  /* Build primary key info */
  PkInfo pk = {0};
  if (!pk_info_build(&pk, state->data, state->cursor_row, state->schema)) {
    tui_set_error(state, "Cannot delete: no primary key found");
    return;
  }

  const char *table = state->tables[state->current_table];

  /* Highlight the row being deleted with danger background */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);
  (void)win_rows;

  int row_y = 3 + (int)(state->cursor_row - state->scroll_row);
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
    pk_info_free(&pk);
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
    pk_info_free(&pk);
    return;
  }

  /* Save position info before delete */
  size_t abs_row = state->loaded_offset + state->cursor_row;
  size_t saved_col = state->cursor_col;
  size_t saved_scroll_col = state->scroll_col;
  size_t visual_offset = state->cursor_row >= state->scroll_row
                             ? state->cursor_row - state->scroll_row
                             : 0;

  /* Perform the delete */
  char *err = NULL;
  bool success =
      db_delete_row(state->conn, table, pk.col_names, pk.values, pk.count, &err);
  pk_info_free(&pk);

  if (success) {
    tui_set_status(state, "Row deleted");

    if (state->total_rows > 0)
      state->total_rows--;

    if (abs_row >= state->total_rows && state->total_rows > 0)
      abs_row = state->total_rows - 1;

    size_t target_offset = (abs_row / PAGE_SIZE) * PAGE_SIZE;
    tui_load_rows_at(state, target_offset);

    if (state->data && state->data->num_rows > 0) {
      state->cursor_row = abs_row - state->loaded_offset;
      if (state->cursor_row >= state->data->num_rows)
        state->cursor_row = state->data->num_rows - 1;
      state->cursor_col = saved_col;
      state->scroll_col = saved_scroll_col;

      if (state->cursor_row >= visual_offset)
        state->scroll_row = state->cursor_row - visual_offset;
      else
        state->scroll_row = 0;
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
    if (state->edit_pos > 0 && state->edit_pos <= len && state->edit_buffer) {
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
      /* Insert character - check for overflow */
      if (len > SIZE_MAX - 2) {
        /* Buffer too large - ignore keystroke */
        return true;
      }
      size_t new_len = len + 2;
      char *new_buf = realloc(state->edit_buffer, new_len);
      if (!new_buf) {
        /* Allocation failed - ignore keystroke */
        return true;
      }
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
    return true;
  }

  return false;
}
