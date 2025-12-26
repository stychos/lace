/*
 * Lace
 * Cell editing and row deletion
 *
 * Uses VmTable for data access where possible.
 * TUI-specific code remains for ncurses dialogs and confirmation UI.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../viewmodel/vm_table.h"
#include "tui_internal.h"
#include "views/editor_view.h"
#include <stdlib.h>
#include <string.h>

/* Helper to get VmTable, returns NULL if not valid for editing */
static VmTable *get_vm_table(TuiState *state) {
  if (!state || !state->vm_table)
    return NULL;
  if (!vm_table_valid(state->vm_table))
    return NULL;
  return state->vm_table;
}

/* Primary key info for database operations */
typedef struct {
  const char **col_names; /* Array of PK column names (not owned) */
  DbValue *values;        /* Array of PK values (owned, must be freed) */
  size_t count;           /* Number of PK columns */
} PkInfo;

/* Find all primary key column indices using VmTable */
size_t tui_find_pk_columns(TuiState *state, size_t *pk_indices,
                           size_t max_pks) {
  if (!pk_indices || max_pks == 0)
    return 0;

  VmTable *vm = get_vm_table(state);
  if (!vm)
    return 0;

  const TableSchema *schema = vm_table_schema(vm);
  if (!schema)
    return 0;

  size_t count = 0;
  for (size_t i = 0; i < schema->num_columns && count < max_pks; i++) {
    if (schema->columns[i].primary_key) {
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
  if (!state || state->editing)
    return;

  VmTable *vm = get_vm_table(state);
  if (!vm)
    return;

  /* Get cursor position from viewmodel */
  size_t cursor_row, cursor_col;
  vm_table_get_cursor(vm, &cursor_row, &cursor_col);

  /* Validate bounds */
  size_t num_rows = vm_table_row_count(vm);
  size_t num_cols = vm_table_col_count(vm);
  if (cursor_row >= num_rows || cursor_col >= num_cols)
    return;

  /* Get current cell value via viewmodel */
  const DbValue *val = vm_table_cell(vm, cursor_row, cursor_col);
  if (!val)
    return;

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
  int col_width = tui_get_column_width(state, cursor_col);
  size_t content_len = content ? strlen(content) : 0;
  bool is_truncated = content_len > (size_t)col_width;

  /* Also check if content has newlines (always use modal for multi-line) */
  bool has_newlines = content && strchr(content, '\n') != NULL;

  if (is_truncated || has_newlines) {
    /* Use modal editor for truncated or multi-line content */
    const char *col_name = vm_table_column_name(vm, cursor_col);
    char *title = str_printf("Edit: %s", col_name ? col_name : "Cell");

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
  if (!state || state->editing)
    return;

  VmTable *vm = get_vm_table(state);
  if (!vm)
    return;

  /* Get cursor position from viewmodel */
  size_t cursor_row, cursor_col;
  vm_table_get_cursor(vm, &cursor_row, &cursor_col);

  /* Validate bounds */
  size_t num_rows = vm_table_row_count(vm);
  size_t num_cols = vm_table_col_count(vm);
  if (cursor_row >= num_rows || cursor_col >= num_cols)
    return;

  /* Get current cell value via viewmodel */
  const DbValue *val = vm_table_cell(vm, cursor_row, cursor_col);
  if (!val)
    return;

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
  const char *col_name = vm_table_column_name(vm, cursor_col);
  char *title = str_printf("Edit: %s", col_name ? col_name : "Cell");

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
  if (!state || !state->editing) {
    tui_cancel_edit(state);
    return;
  }

  VmTable *vm = get_vm_table(state);
  if (!vm) {
    tui_cancel_edit(state);
    return;
  }

  DbConnection *conn = vm_table_connection(vm);
  const char *table = vm_table_name(vm);
  const TableSchema *schema = vm_table_schema(vm);

  if (!conn || !table || !schema) {
    tui_cancel_edit(state);
    return;
  }

  /* Get cursor position from viewmodel */
  size_t cursor_row, cursor_col;
  vm_table_get_cursor(vm, &cursor_row, &cursor_col);

  /* Build primary key info - still needs direct data access for PK values */
  PkInfo pk = {0};
  if (!pk_info_build(&pk, state->data, cursor_row, (TableSchema *)schema)) {
    tui_set_error(state, "Cannot update: no primary key found");
    tui_cancel_edit(state);
    return;
  }

  /* Get column name from viewmodel */
  const char *col_name = vm_table_column_name(vm, cursor_col);
  if (!col_name) {
    pk_info_free(&pk);
    tui_cancel_edit(state);
    return;
  }

  /* Create new value from edit buffer */
  DbValue new_val;
  if (state->edit_buffer == NULL || state->edit_buffer[0] == '\0') {
    new_val = db_value_null();
  } else {
    new_val = db_value_text(state->edit_buffer);
  }

  /* Attempt to update */
  char *err = NULL;
  bool success = db_update_cell(conn, table, pk.col_names, pk.values,
                                pk.count, col_name, &new_val, &err);
  pk_info_free(&pk);

  if (success) {
    /* Update the local data - still needs direct access for in-place update */
    if (state->data && cursor_row < state->data->num_rows &&
        state->data->rows && state->data->rows[cursor_row].cells &&
        cursor_col < state->data->rows[cursor_row].num_cells) {
      DbValue *cell = &state->data->rows[cursor_row].cells[cursor_col];
      db_value_free(cell);
      *cell = new_val;
    } else {
      db_value_free(&new_val);
    }
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
  VmTable *vm = get_vm_table(state);
  if (!vm)
    return;

  DbConnection *conn = vm_table_connection(vm);
  const char *table = vm_table_name(vm);
  const TableSchema *schema = vm_table_schema(vm);

  if (!conn || !table || !schema)
    return;

  /* Get cursor position from viewmodel */
  size_t cursor_row, cursor_col;
  vm_table_get_cursor(vm, &cursor_row, &cursor_col);

  /* Validate bounds */
  size_t num_rows = vm_table_row_count(vm);
  size_t num_cols = vm_table_col_count(vm);
  if (cursor_row >= num_rows || cursor_col >= num_cols)
    return;

  /* Build primary key info - still needs direct data access */
  PkInfo pk = {0};
  if (!pk_info_build(&pk, state->data, cursor_row, (TableSchema *)schema)) {
    tui_set_error(state, "Cannot update: no primary key found");
    return;
  }

  const char *col_name = vm_table_column_name(vm, cursor_col);
  if (!col_name) {
    pk_info_free(&pk);
    return;
  }

  DbValue new_val = set_null ? db_value_null() : db_value_text("");

  /* Attempt to update */
  char *err = NULL;
  bool success = db_update_cell(conn, table, pk.col_names, pk.values,
                                pk.count, col_name, &new_val, &err);
  pk_info_free(&pk);

  if (success) {
    /* Update the local data - still needs direct access */
    if (state->data && cursor_row < state->data->num_rows &&
        state->data->rows && state->data->rows[cursor_row].cells &&
        cursor_col < state->data->rows[cursor_row].num_cells) {
      DbValue *cell = &state->data->rows[cursor_row].cells[cursor_col];
      db_value_free(cell);
      *cell = new_val;
    } else {
      db_value_free(&new_val);
    }
    tui_set_status(state, set_null ? "Cell set to NULL" : "Cell set to empty");
  } else {
    db_value_free(&new_val);
    tui_set_error(state, "Update failed: %s", err ? err : "unknown error");
    free(err);
  }
}

/* Delete current row */
void tui_delete_row(TuiState *state) {
  VmTable *vm = get_vm_table(state);
  if (!vm)
    return;

  DbConnection *conn = vm_table_connection(vm);
  const char *table = vm_table_name(vm);
  const TableSchema *schema = vm_table_schema(vm);

  if (!conn || !table || !schema)
    return;

  /* Get cursor and scroll positions from viewmodel */
  size_t cursor_row, cursor_col;
  vm_table_get_cursor(vm, &cursor_row, &cursor_col);
  size_t scroll_row, scroll_col;
  vm_table_get_scroll(vm, &scroll_row, &scroll_col);

  size_t num_rows = vm_table_row_count(vm);
  size_t num_cols = vm_table_col_count(vm);
  if (cursor_row >= num_rows)
    return;

  /* Build primary key info - still needs direct data access */
  PkInfo pk = {0};
  if (!pk_info_build(&pk, state->data, cursor_row, (TableSchema *)schema)) {
    tui_set_error(state, "Cannot delete: no primary key found");
    return;
  }

  /* Highlight the row being deleted with danger background (TUI-specific) */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);
  (void)win_rows;

  int row_y = 3 + (int)(cursor_row - scroll_row);
  wattron(state->main_win, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
  int x = 1;
  for (size_t col = scroll_col; col < num_cols; col++) {
    int col_width = tui_get_column_width(state, col);
    if (x + col_width + 3 > win_cols)
      break;

    /* Get cell value via viewmodel */
    const DbValue *val = vm_table_cell(vm, cursor_row, col);
    if (!val)
      continue;

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

  /* Show confirmation dialog (TUI-specific) */
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
  size_t loaded_offset = vm_table_loaded_offset(vm);
  size_t total_rows = vm_table_total_rows(vm);
  size_t abs_row = loaded_offset + cursor_row;
  size_t saved_col = cursor_col;
  size_t saved_scroll_col = scroll_col;
  size_t visual_offset = cursor_row >= scroll_row ? cursor_row - scroll_row : 0;

  /* Perform the delete */
  char *err = NULL;
  bool success =
      db_delete_row(conn, table, pk.col_names, pk.values, pk.count, &err);
  pk_info_free(&pk);

  if (success) {
    tui_set_status(state, "Row deleted");

    if (total_rows > 0)
      total_rows--;

    /* Update compatibility layer total_rows (will be read by reload) */
    state->total_rows = total_rows;

    if (abs_row >= total_rows && total_rows > 0)
      abs_row = total_rows - 1;

    size_t target_offset = (abs_row / PAGE_SIZE) * PAGE_SIZE;
    tui_load_rows_at(state, target_offset);

    /* Re-read state after reload */
    num_rows = vm_table_row_count(vm);

    if (num_rows > 0) {
      loaded_offset = vm_table_loaded_offset(vm);
      cursor_row = abs_row - loaded_offset;
      if (cursor_row >= num_rows)
        cursor_row = num_rows - 1;

      if (cursor_row >= visual_offset)
        scroll_row = cursor_row - visual_offset;
      else
        scroll_row = 0;

      /* Update via viewmodel */
      vm_table_set_cursor(vm, cursor_row, saved_col);
      vm_table_set_scroll(vm, scroll_row, saved_scroll_col);

      /* Sync to compatibility layer (temporary) */
      state->cursor_row = cursor_row;
      state->cursor_col = saved_col;
      state->scroll_row = scroll_row;
      state->scroll_col = saved_scroll_col;
    }
  } else {
    tui_set_error(state, "Delete failed: %s", err ? err : "unknown error");
    free(err);
  }
}

/* Handle edit mode input */
bool tui_handle_edit_input(TuiState *state, const UiEvent *event) {
  if (!state->editing)
    return false;
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  size_t len = state->edit_buffer ? strlen(state->edit_buffer) : 0;
  int key_char = render_event_get_char(event);

  /* Escape - cancel */
  if (render_event_is_special(event, UI_KEY_ESCAPE)) {
    tui_cancel_edit(state);
    return true;
  }

  /* Enter - confirm */
  if (render_event_is_special(event, UI_KEY_ENTER)) {
    tui_confirm_edit(state);
    return true;
  }

  /* Left arrow - move cursor left */
  if (render_event_is_special(event, UI_KEY_LEFT)) {
    if (state->edit_pos > 0) {
      state->edit_pos--;
    }
    return true;
  }

  /* Right arrow - move cursor right */
  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    if (state->edit_pos < len) {
      state->edit_pos++;
    }
    return true;
  }

  /* Home or Ctrl+A - go to start */
  if (render_event_is_special(event, UI_KEY_HOME) ||
      render_event_is_ctrl(event, 'A')) {
    state->edit_pos = 0;
    return true;
  }

  /* End or Ctrl+E - go to end */
  if (render_event_is_special(event, UI_KEY_END) ||
      render_event_is_ctrl(event, 'E')) {
    state->edit_pos = len;
    return true;
  }

  /* Backspace - delete character before cursor */
  if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
    if (state->edit_pos > 0 && state->edit_pos <= len && state->edit_buffer) {
      memmove(state->edit_buffer + state->edit_pos - 1,
              state->edit_buffer + state->edit_pos, len - state->edit_pos + 1);
      state->edit_pos--;
    }
    return true;
  }

  /* Delete - delete character at cursor */
  if (render_event_is_special(event, UI_KEY_DELETE)) {
    if (state->edit_pos < len && state->edit_buffer) {
      memmove(state->edit_buffer + state->edit_pos,
              state->edit_buffer + state->edit_pos + 1, len - state->edit_pos);
    }
    return true;
  }

  /* Ctrl+U - clear line */
  if (render_event_is_ctrl(event, 'U')) {
    if (state->edit_buffer) {
      state->edit_buffer[0] = '\0';
      state->edit_pos = 0;
    }
    return true;
  }

  /* Ctrl+N - set to NULL */
  if (render_event_is_ctrl(event, 'N')) {
    free(state->edit_buffer);
    state->edit_buffer = NULL;
    state->edit_pos = 0;
    tui_confirm_edit(state);
    return true;
  }

  /* Ctrl+D - set to empty string */
  if (render_event_is_ctrl(event, 'D')) {
    free(state->edit_buffer);
    state->edit_buffer = str_dup("");
    state->edit_pos = 0;
    tui_confirm_edit(state);
    return true;
  }

  /* Printable character - insert at cursor */
  if (render_event_is_char(event) && key_char >= 32 && key_char < 127) {
    /* Check for overflow */
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
      state->edit_buffer[0] = (char)key_char;
      state->edit_buffer[1] = '\0';
      state->edit_pos = 1;
    } else {
      memmove(state->edit_buffer + state->edit_pos + 1,
              state->edit_buffer + state->edit_pos, len - state->edit_pos + 1);
      state->edit_buffer[state->edit_pos] = (char)key_char;
      state->edit_pos++;
    }
    return true;
  }

  /* Consume all other keys when editing */
  return true;
}
