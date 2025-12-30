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

#include "../../config/config.h"
#include "../../viewmodel/vm_table.h"
#include "tui_internal.h"
#include "views/editor_view.h"
#include <stdlib.h>
#include <string.h>

/* Maximum number of primary key columns we support.
 * Composite PKs with more than 16 columns are extremely rare in practice. */
#define MAX_PK_COLUMNS 16

/* Helper to get VmTable, returns NULL if not valid for editing */
static VmTable *get_vm_table(TuiState *state) {
  if (!state || !state->vm_table)
    return NULL;
  if (!vm_table_valid(state->vm_table))
    return NULL;
  return state->vm_table;
}

/* Note: History recording is now handled automatically by the database layer
 * via the history callback set up in app_add_connection(). */

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

  /* Count total PK columns first */
  size_t total_pk = 0;
  for (size_t i = 0; i < schema->num_columns; i++) {
    if (schema->columns[i].primary_key) {
      total_pk++;
    }
  }

  if (total_pk == 0)
    return false;

  /* Check for tables with too many PK columns (extremely rare) */
  if (total_pk > MAX_PK_COLUMNS)
    return false;

  /* Find PK column indices */
  size_t pk_indices[MAX_PK_COLUMNS];
  size_t num_pk = 0;
  for (size_t i = 0; i < schema->num_columns && num_pk < MAX_PK_COLUMNS; i++) {
    if (schema->columns[i].primary_key) {
      pk_indices[num_pk++] = i;
    }
  }

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

  if (success) {
    /* History is recorded automatically by database layer */
    pk_info_free(&pk);

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

    /* Mark other tabs with the same table as needing refresh */
    Tab *tab = TUI_TAB(state);
    if (tab) {
      app_mark_table_tabs_dirty(state->app, tab->connection_index, table, tab);
    }
  } else {
    pk_info_free(&pk);
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

  if (success) {
    /* History is recorded automatically by database layer */
    pk_info_free(&pk);

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

    /* Mark other tabs with the same table as needing refresh */
    Tab *tab = TUI_TAB(state);
    if (tab) {
      app_mark_table_tabs_dirty(state->app, tab->connection_index, table, tab);
    }
  } else {
    pk_info_free(&pk);
    db_value_free(&new_val);
    tui_set_error(state, "Update failed: %s", err ? err : "unknown error");
    free(err);
  }
}

/* Delete a single row by its local index in current data */
static bool delete_single_row(TuiState *state, size_t local_row, char **err) {
  VmTable *vm = state->vm_table;
  if (!vm)
    return false;

  DbConnection *conn = vm_table_connection(vm);
  const char *table = vm_table_name(vm);
  const TableSchema *schema = vm_table_schema(vm);

  if (!conn || !table || !schema)
    return false;

  PkInfo pk = {0};
  if (!pk_info_build(&pk, state->data, local_row, (TableSchema *)schema)) {
    if (err)
      *err = str_dup("No primary key found");
    return false;
  }

  bool success =
      db_delete_row(conn, table, pk.col_names, pk.values, pk.count, err);

  /* History is recorded automatically by database layer */
  pk_info_free(&pk);
  return success;
}

/* Delete current row or selected rows */
void tui_delete_row(TuiState *state) {
  VmTable *vm = get_vm_table(state);
  if (!vm)
    return;

  DbConnection *conn = vm_table_connection(vm);
  const char *table = vm_table_name(vm);
  const TableSchema *schema = vm_table_schema(vm);

  if (!conn || !table || !schema)
    return;

  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;

  /* Get cursor and scroll positions from viewmodel */
  size_t cursor_row, cursor_col;
  vm_table_get_cursor(vm, &cursor_row, &cursor_col);
  size_t scroll_row, scroll_col;
  vm_table_get_scroll(vm, &scroll_row, &scroll_col);

  size_t num_rows = vm_table_row_count(vm);
  if (cursor_row >= num_rows)
    return;

  /* Check if we have selected rows for bulk delete */
  size_t num_selected = tab->num_selected;
  bool bulk_delete = (num_selected > 0);
  size_t rows_to_delete = bulk_delete ? num_selected : 1;

  /* Check if confirmation is required */
  bool need_confirmation =
      state->app && state->app->config &&
      state->app->config->general.delete_confirmation;

  /* Show confirmation if needed */
  if (need_confirmation) {
    char msg[64];
    if (rows_to_delete == 1) {
      snprintf(msg, sizeof(msg), "Delete this row?");
    } else {
      snprintf(msg, sizeof(msg), "Delete %zu selected rows?", rows_to_delete);
    }
    if (!tui_show_confirm_dialog(state, msg)) {
      tui_set_status(state, "Delete cancelled");
      return;
    }
  }

  size_t loaded_offset = vm_table_loaded_offset(vm);
  size_t total_rows = vm_table_total_rows(vm);
  size_t deleted_count = 0;
  size_t failed_count = 0;

  if (bulk_delete) {
    /* Delete selected rows - iterate in reverse to avoid index shifting issues */
    /* First, collect global indices and sort in descending order */
    size_t *to_delete = malloc(num_selected * sizeof(size_t));
    if (!to_delete) {
      tui_set_error(state, "Out of memory");
      return;
    }
    memcpy(to_delete, tab->selected_rows, num_selected * sizeof(size_t));

    /* Sort descending so we delete from highest index first */
    for (size_t i = 0; i < num_selected - 1; i++) {
      for (size_t j = i + 1; j < num_selected; j++) {
        if (to_delete[j] > to_delete[i]) {
          size_t tmp = to_delete[i];
          to_delete[i] = to_delete[j];
          to_delete[j] = tmp;
        }
      }
    }

    /* Delete each selected row */
    for (size_t i = 0; i < num_selected; i++) {
      size_t global_row = to_delete[i];

      /* Check if this row is in the currently loaded data */
      if (global_row < loaded_offset ||
          global_row >= loaded_offset + num_rows) {
        /* Row not loaded - would need to load it first, skip for now */
        failed_count++;
        continue;
      }

      size_t local_row = global_row - loaded_offset;
      char *err = NULL;
      if (delete_single_row(state, local_row, &err)) {
        deleted_count++;
        if (total_rows > 0)
          total_rows--;
      } else {
        failed_count++;
        free(err);
      }
    }

    free(to_delete);

    /* Clear selections after bulk delete */
    tab_clear_selections(tab);
  } else {
    /* Delete single row at cursor */
    char *err = NULL;
    if (delete_single_row(state, cursor_row, &err)) {
      deleted_count = 1;
      if (total_rows > 0)
        total_rows--;
    } else {
      tui_set_error(state, "Delete failed: %s", err ? err : "unknown error");
      free(err);
      return;
    }
  }

  /* Show result message */
  if (deleted_count > 0) {
    if (failed_count > 0) {
      tui_set_status(state, "%zu row(s) deleted, %zu failed",
                     deleted_count, failed_count);
    } else if (deleted_count == 1) {
      tui_set_status(state, "Row deleted");
    } else {
      tui_set_status(state, "%zu rows deleted", deleted_count);
    }

    /* Mark other tabs with the same table as needing refresh */
    app_mark_table_tabs_dirty(state->app, tab->connection_index,
                              vm_table_name(vm), tab);

    /* Update state and reload data */
    state->total_rows = total_rows;

    size_t abs_row = loaded_offset + cursor_row;
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

      size_t visual_offset =
          cursor_row >= scroll_row ? cursor_row - scroll_row : 0;
      if (cursor_row >= visual_offset)
        scroll_row = cursor_row - visual_offset;
      else
        scroll_row = 0;

      /* Update via viewmodel */
      vm_table_set_cursor(vm, cursor_row, cursor_col);
      vm_table_set_scroll(vm, scroll_row, scroll_col);

      /* Sync to compatibility layer */
      state->cursor_row = cursor_row;
      state->cursor_col = cursor_col;
      state->scroll_row = scroll_row;
      state->scroll_col = scroll_col;
    }
  } else if (failed_count > 0) {
    tui_set_error(state, "Failed to delete %zu row(s)", failed_count);
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

  /* Set to NULL - uses configurable hotkey */
  if (state->app && state->app->config &&
      hotkey_matches(state->app->config, event, HOTKEY_EDITOR_NULL)) {
    free(state->edit_buffer);
    state->edit_buffer = NULL;
    state->edit_pos = 0;
    tui_confirm_edit(state);
    return true;
  }

  /* Set to empty string - uses configurable hotkey */
  if (state->app && state->app->config &&
      hotkey_matches(state->app->config, event, HOTKEY_EDITOR_EMPTY)) {
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
