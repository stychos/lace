/*
 * Lace
 * Table ViewModel - Implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "vm_table.h"
#include "../db/db.h"
#include "../util/str.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

static void notify_change(VmTable *vm, VmTableChangeFlags flags) {
  if (vm->callbacks.on_change) {
    vm->callbacks.on_change(vm, flags, vm->callbacks.context);
  }
}

static void selection_init(VmSelection *sel) { memset(sel, 0, sizeof(*sel)); }

static void selection_free(VmSelection *sel) {
  free(sel->rows);
  memset(sel, 0, sizeof(*sel));
}

static void selection_clear(VmSelection *sel) {
  sel->count = 0;
  sel->anchor_set = false;
}

static bool selection_contains(const VmSelection *sel, size_t row) {
  for (size_t i = 0; i < sel->count; i++) {
    if (sel->rows[i] == row)
      return true;
  }
  return false;
}

static void selection_add(VmSelection *sel, size_t row) {
  if (selection_contains(sel, row))
    return;

  if (sel->count >= sel->capacity) {
    size_t new_cap = sel->capacity ? sel->capacity * 2 : 16;

    /* Check for overflow in capacity doubling */
    if (sel->capacity > 0 && new_cap < sel->capacity)
      return;

    /* Check for overflow in allocation size calculation */
    if (new_cap > SIZE_MAX / sizeof(size_t))
      return;

    size_t *new_rows = realloc(sel->rows, new_cap * sizeof(size_t));
    if (!new_rows)
      return;
    sel->rows = new_rows;
    sel->capacity = new_cap;
  }
  sel->rows[sel->count++] = row;
}

static void selection_remove(VmSelection *sel, size_t row) {
  for (size_t i = 0; i < sel->count; i++) {
    if (sel->rows[i] == row) {
      memmove(&sel->rows[i], &sel->rows[i + 1],
              (sel->count - i - 1) * sizeof(size_t));
      sel->count--;
      return;
    }
  }
}

static void edit_init(VmEditState *edit) { memset(edit, 0, sizeof(*edit)); }

static void edit_free(VmEditState *edit) {
  free(edit->buffer);
  free(edit->original);
  memset(edit, 0, sizeof(*edit));
}

static void edit_clear(VmEditState *edit) {
  edit->active = false;
  free(edit->buffer);
  edit->buffer = NULL;
  edit->buffer_len = 0;
  edit->buffer_cap = 0;
  edit->cursor_pos = 0;
  free(edit->original);
  edit->original = NULL;
}

static bool edit_ensure_capacity(VmEditState *edit, size_t needed) {
  if (needed <= edit->buffer_cap)
    return true;

  size_t new_cap = edit->buffer_cap ? edit->buffer_cap * 2 : 256;
  while (new_cap < needed) {
    /* Check for overflow before doubling */
    if (new_cap > SIZE_MAX / 2) {
      new_cap = needed; /* Fall back to exact size */
      break;
    }
    new_cap *= 2;
  }

  char *new_buf = realloc(edit->buffer, new_cap);
  if (!new_buf)
    return false;

  edit->buffer = new_buf;
  edit->buffer_cap = new_cap;
  return true;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

VmTable *vm_table_create(AppState *app, Tab *tab,
                         const VmTableCallbacks *callbacks) {
  VmTable *vm = calloc(1, sizeof(VmTable));
  if (!vm)
    return NULL;

  vm->app = app;
  vm->tab = tab;

  if (callbacks) {
    vm->callbacks = *callbacks;
  }

  selection_init(&vm->selection);
  edit_init(&vm->edit);

  return vm;
}

void vm_table_destroy(VmTable *vm) {
  if (!vm)
    return;

  selection_free(&vm->selection);
  edit_free(&vm->edit);
  free(vm->col_widths);
  free(vm);
}

void vm_table_bind(VmTable *vm, Tab *tab) {
  if (!vm)
    return;

  /* Cancel any ongoing edit */
  if (vm->edit.active) {
    edit_clear(&vm->edit);
  }

  /* Clear selection */
  selection_clear(&vm->selection);

  /* Bind to new tab */
  vm->tab = tab;

  /* Notify full change */
  notify_change(vm, VM_TABLE_CHANGE_ALL);
}

/* ============================================================================
 * Data Access
 * ============================================================================
 */

bool vm_table_valid(const VmTable *vm) {
  return vm && vm->tab && vm->tab->data;
}

size_t vm_table_row_count(const VmTable *vm) {
  if (!vm_table_valid(vm))
    return 0;
  return vm->tab->data->num_rows;
}

size_t vm_table_col_count(const VmTable *vm) {
  if (!vm_table_valid(vm))
    return 0;
  return vm->tab->data->num_columns;
}

size_t vm_table_total_rows(const VmTable *vm) {
  if (!vm || !vm->tab)
    return 0;
  return vm->tab->total_rows;
}

const char *vm_table_column_name(const VmTable *vm, size_t col) {
  if (!vm_table_valid(vm) || col >= vm->tab->data->num_columns)
    return NULL;
  return vm->tab->data->columns[col].name;
}

DbValueType vm_table_column_type(const VmTable *vm, size_t col) {
  if (!vm_table_valid(vm) || col >= vm->tab->data->num_columns)
    return DB_TYPE_NULL;
  return vm->tab->data->columns[col].type;
}

bool vm_table_column_nullable(const VmTable *vm, size_t col) {
  if (!vm || !vm->tab || !vm->tab->schema ||
      col >= vm->tab->schema->num_columns)
    return true;
  return vm->tab->schema->columns[col].nullable;
}

bool vm_table_column_is_primary_key(const VmTable *vm, size_t col) {
  if (!vm || !vm->tab || !vm->tab->schema ||
      col >= vm->tab->schema->num_columns)
    return false;
  return vm->tab->schema->columns[col].primary_key;
}

int vm_table_column_width(const VmTable *vm, size_t col) {
  if (!vm || col >= vm->num_col_widths)
    return 10; /* Default width */
  return vm->col_widths[col];
}

const DbValue *vm_table_cell(const VmTable *vm, size_t row, size_t col) {
  if (!vm_table_valid(vm))
    return NULL;

  ResultSet *data = vm->tab->data;
  if (row >= data->num_rows || col >= data->num_columns)
    return NULL;

  if (!data->rows || !data->rows[row].cells)
    return NULL;

  /* Validate column index against actual cell count in this row */
  if (col >= data->rows[row].num_cells)
    return NULL;

  return &data->rows[row].cells[col];
}

const char *vm_table_cell_text(const VmTable *vm, size_t row, size_t col) {
  const DbValue *val = vm_table_cell(vm, row, col);
  if (!val || val->is_null)
    return NULL;

  if (val->type == DB_TYPE_TEXT && val->text.data)
    return val->text.data;

  return NULL; /* Other types need conversion */
}

bool vm_table_cell_is_null(const VmTable *vm, size_t row, size_t col) {
  const DbValue *val = vm_table_cell(vm, row, col);
  return !val || val->is_null;
}

bool vm_table_row_loaded(const VmTable *vm, size_t row) {
  if (!vm || !vm->tab)
    return false;

  size_t offset = vm->tab->loaded_offset;
  size_t count = vm->tab->loaded_count;

  return row >= offset && row < offset + count;
}

/* ============================================================================
 * Cursor & Navigation
 * ============================================================================
 */

void vm_table_get_cursor(const VmTable *vm, size_t *row, size_t *col) {
  if (!vm || !vm->tab) {
    if (row)
      *row = 0;
    if (col)
      *col = 0;
    return;
  }
  if (row)
    *row = vm->tab->cursor_row;
  if (col)
    *col = vm->tab->cursor_col;
}

void vm_table_set_cursor(VmTable *vm, size_t row, size_t col) {
  if (!vm || !vm->tab)
    return;

  size_t max_row = vm_table_total_rows(vm);
  size_t max_col = vm_table_col_count(vm);

  if (max_row > 0 && row >= max_row)
    row = max_row - 1;
  if (max_col > 0 && col >= max_col)
    col = max_col - 1;

  bool changed = (vm->tab->cursor_row != row || vm->tab->cursor_col != col);

  vm->tab->cursor_row = row;
  vm->tab->cursor_col = col;

  if (changed) {
    notify_change(vm, VM_TABLE_CHANGE_CURSOR);
  }
}

void vm_table_move_cursor(VmTable *vm, int row_delta, int col_delta) {
  size_t row, col;
  vm_table_get_cursor(vm, &row, &col);

  if (row_delta < 0) {
    size_t abs_delta =
        (row_delta == INT_MIN) ? (size_t)INT_MAX + 1 : (size_t)(-row_delta);
    row = (abs_delta > row) ? 0 : row - abs_delta;
  } else {
    row = row + (size_t)row_delta;
  }

  if (col_delta < 0) {
    size_t abs_delta =
        (col_delta == INT_MIN) ? (size_t)INT_MAX + 1 : (size_t)(-col_delta);
    col = (abs_delta > col) ? 0 : col - abs_delta;
  } else {
    col = col + (size_t)col_delta;
  }

  vm_table_set_cursor(vm, row, col);
}

void vm_table_goto_first_row(VmTable *vm) {
  size_t col;
  vm_table_get_cursor(vm, NULL, &col);
  vm_table_set_cursor(vm, 0, col);
}

void vm_table_goto_last_row(VmTable *vm) {
  size_t total = vm_table_total_rows(vm);
  size_t col;
  vm_table_get_cursor(vm, NULL, &col);
  if (total > 0) {
    vm_table_set_cursor(vm, total - 1, col);
  }
}

void vm_table_goto_first_col(VmTable *vm) {
  size_t row;
  vm_table_get_cursor(vm, &row, NULL);
  vm_table_set_cursor(vm, row, 0);
}

void vm_table_goto_last_col(VmTable *vm) {
  size_t cols = vm_table_col_count(vm);
  size_t row;
  vm_table_get_cursor(vm, &row, NULL);
  if (cols > 0) {
    vm_table_set_cursor(vm, row, cols - 1);
  }
}

void vm_table_page_up(VmTable *vm, size_t page_size) {
  size_t row, col;
  vm_table_get_cursor(vm, &row, &col);
  if (row > page_size)
    row -= page_size;
  else
    row = 0;
  vm_table_set_cursor(vm, row, col);
}

void vm_table_page_down(VmTable *vm, size_t page_size) {
  size_t row, col;
  vm_table_get_cursor(vm, &row, &col);
  row += page_size;
  vm_table_set_cursor(vm, row, col);
}

/* ============================================================================
 * Scroll
 * ============================================================================
 */

void vm_table_get_scroll(const VmTable *vm, size_t *row, size_t *col) {
  if (!vm || !vm->tab) {
    if (row)
      *row = 0;
    if (col)
      *col = 0;
    return;
  }
  if (row)
    *row = vm->tab->scroll_row;
  if (col)
    *col = vm->tab->scroll_col;
}

void vm_table_set_scroll(VmTable *vm, size_t row, size_t col) {
  if (!vm || !vm->tab)
    return;

  bool changed = (vm->tab->scroll_row != row || vm->tab->scroll_col != col);

  vm->tab->scroll_row = row;
  vm->tab->scroll_col = col;

  if (changed) {
    notify_change(vm, VM_TABLE_CHANGE_SCROLL);
  }
}

void vm_table_ensure_cursor_visible(VmTable *vm, size_t visible_rows,
                                    size_t visible_cols) {
  if (!vm || !vm->tab)
    return;

  size_t cursor_row = vm->tab->cursor_row;
  size_t cursor_col = vm->tab->cursor_col;
  size_t scroll_row = vm->tab->scroll_row;
  size_t scroll_col = vm->tab->scroll_col;

  /* Adjust vertical scroll */
  if (cursor_row < scroll_row) {
    scroll_row = cursor_row;
  } else if (visible_rows > 0 && cursor_row >= scroll_row + visible_rows) {
    scroll_row = cursor_row - visible_rows + 1;
  }

  /* Adjust horizontal scroll */
  if (cursor_col < scroll_col) {
    scroll_col = cursor_col;
  } else if (visible_cols > 0 && cursor_col >= scroll_col + visible_cols) {
    scroll_col = cursor_col - visible_cols + 1;
  }

  vm_table_set_scroll(vm, scroll_row, scroll_col);
}

/* ============================================================================
 * Selection
 * ============================================================================
 */

void vm_table_select_row(VmTable *vm, size_t row) {
  if (!vm)
    return;
  selection_add(&vm->selection, row);
  vm->selection.anchor = row;
  vm->selection.anchor_set = true;
  notify_change(vm, VM_TABLE_CHANGE_SELECTION);
}

void vm_table_deselect_row(VmTable *vm, size_t row) {
  if (!vm)
    return;
  selection_remove(&vm->selection, row);
  notify_change(vm, VM_TABLE_CHANGE_SELECTION);
}

void vm_table_toggle_row_selection(VmTable *vm, size_t row) {
  if (!vm)
    return;
  if (selection_contains(&vm->selection, row)) {
    selection_remove(&vm->selection, row);
  } else {
    selection_add(&vm->selection, row);
    vm->selection.anchor = row;
    vm->selection.anchor_set = true;
  }
  notify_change(vm, VM_TABLE_CHANGE_SELECTION);
}

bool vm_table_row_selected(const VmTable *vm, size_t row) {
  if (!vm)
    return false;
  return selection_contains(&vm->selection, row);
}

void vm_table_select_range(VmTable *vm, size_t from, size_t to) {
  if (!vm)
    return;

  selection_clear(&vm->selection);

  size_t start = from < to ? from : to;
  size_t end = from < to ? to : from;

  for (size_t i = start; i <= end; i++) {
    selection_add(&vm->selection, i);
  }

  notify_change(vm, VM_TABLE_CHANGE_SELECTION);
}

void vm_table_extend_selection(VmTable *vm, size_t to_row) {
  if (!vm)
    return;

  if (!vm->selection.anchor_set) {
    vm_table_select_row(vm, to_row);
    return;
  }

  vm_table_select_range(vm, vm->selection.anchor, to_row);
}

void vm_table_select_all(VmTable *vm) {
  if (!vm)
    return;

  size_t total = vm_table_total_rows(vm);
  selection_clear(&vm->selection);

  for (size_t i = 0; i < total; i++) {
    selection_add(&vm->selection, i);
  }

  notify_change(vm, VM_TABLE_CHANGE_SELECTION);
}

void vm_table_clear_selection(VmTable *vm) {
  if (!vm)
    return;
  selection_clear(&vm->selection);
  notify_change(vm, VM_TABLE_CHANGE_SELECTION);
}

size_t vm_table_selection_count(const VmTable *vm) {
  if (!vm)
    return 0;
  return vm->selection.count;
}

const size_t *vm_table_selected_rows(const VmTable *vm) {
  if (!vm)
    return NULL;
  return vm->selection.rows;
}

/* ============================================================================
 * Editing
 * ============================================================================
 */

bool vm_table_start_edit(VmTable *vm, size_t row, size_t col) {
  if (!vm_table_valid(vm))
    return false;

  /* Get current value */
  const DbValue *val = vm_table_cell(vm, row, col);
  char *text = val ? db_value_to_string(val) : str_dup("");

  if (!text)
    return false;

  /* Initialize edit state */
  edit_clear(&vm->edit);

  size_t len = strlen(text);
  if (!edit_ensure_capacity(&vm->edit, len + 1)) {
    free(text);
    return false;
  }

  memcpy(vm->edit.buffer, text, len + 1);
  vm->edit.buffer_len = len;
  vm->edit.cursor_pos = len;
  vm->edit.original = text;
  vm->edit.row = row;
  vm->edit.col = col;
  vm->edit.active = true;

  notify_change(vm, VM_TABLE_CHANGE_EDITING);
  return true;
}

bool vm_table_start_edit_at_cursor(VmTable *vm) {
  size_t row, col;
  vm_table_get_cursor(vm, &row, &col);
  return vm_table_start_edit(vm, row, col);
}

void vm_table_edit_insert_char(VmTable *vm, char ch) {
  if (!vm || !vm->edit.active)
    return;

  if (!edit_ensure_capacity(&vm->edit, vm->edit.buffer_len + 2))
    return;

  /* Insert at cursor position */
  memmove(vm->edit.buffer + vm->edit.cursor_pos + 1,
          vm->edit.buffer + vm->edit.cursor_pos,
          vm->edit.buffer_len - vm->edit.cursor_pos + 1);
  vm->edit.buffer[vm->edit.cursor_pos] = ch;
  vm->edit.cursor_pos++;
  vm->edit.buffer_len++;

  notify_change(vm, VM_TABLE_CHANGE_EDITING);
}

void vm_table_edit_insert_text(VmTable *vm, const char *text) {
  if (!vm || !vm->edit.active || !text)
    return;

  size_t len = strlen(text);
  if (!edit_ensure_capacity(&vm->edit, vm->edit.buffer_len + len + 1))
    return;

  memmove(vm->edit.buffer + vm->edit.cursor_pos + len,
          vm->edit.buffer + vm->edit.cursor_pos,
          vm->edit.buffer_len - vm->edit.cursor_pos + 1);
  memcpy(vm->edit.buffer + vm->edit.cursor_pos, text, len);
  vm->edit.cursor_pos += len;
  vm->edit.buffer_len += len;

  notify_change(vm, VM_TABLE_CHANGE_EDITING);
}

void vm_table_edit_delete_char(VmTable *vm) {
  if (!vm || !vm->edit.active)
    return;

  if (vm->edit.cursor_pos >= vm->edit.buffer_len)
    return;

  memmove(vm->edit.buffer + vm->edit.cursor_pos,
          vm->edit.buffer + vm->edit.cursor_pos + 1,
          vm->edit.buffer_len - vm->edit.cursor_pos);
  vm->edit.buffer_len--;

  notify_change(vm, VM_TABLE_CHANGE_EDITING);
}

void vm_table_edit_backspace(VmTable *vm) {
  if (!vm || !vm->edit.active || vm->edit.cursor_pos == 0)
    return;

  vm->edit.cursor_pos--;
  vm_table_edit_delete_char(vm);
}

void vm_table_edit_clear(VmTable *vm) {
  if (!vm || !vm->edit.active)
    return;

  vm->edit.buffer[0] = '\0';
  vm->edit.buffer_len = 0;
  vm->edit.cursor_pos = 0;

  notify_change(vm, VM_TABLE_CHANGE_EDITING);
}

void vm_table_edit_set_cursor(VmTable *vm, size_t pos) {
  if (!vm || !vm->edit.active)
    return;

  if (pos > vm->edit.buffer_len)
    pos = vm->edit.buffer_len;
  vm->edit.cursor_pos = pos;

  notify_change(vm, VM_TABLE_CHANGE_EDITING);
}

void vm_table_edit_move_cursor(VmTable *vm, int delta) {
  if (!vm || !vm->edit.active)
    return;

  if (delta < 0 && (size_t)(-delta) > vm->edit.cursor_pos) {
    vm->edit.cursor_pos = 0;
  } else {
    size_t new_pos = vm->edit.cursor_pos + delta;
    if (new_pos > vm->edit.buffer_len)
      new_pos = vm->edit.buffer_len;
    vm->edit.cursor_pos = new_pos;
  }

  notify_change(vm, VM_TABLE_CHANGE_EDITING);
}

void vm_table_edit_home(VmTable *vm) { vm_table_edit_set_cursor(vm, 0); }

void vm_table_edit_end(VmTable *vm) {
  if (!vm || !vm->edit.active)
    return;
  vm_table_edit_set_cursor(vm, vm->edit.buffer_len);
}

bool vm_table_is_editing(const VmTable *vm) { return vm && vm->edit.active; }

const char *vm_table_edit_buffer(const VmTable *vm) {
  if (!vm || !vm->edit.active)
    return NULL;
  return vm->edit.buffer;
}

size_t vm_table_edit_cursor(const VmTable *vm) {
  if (!vm || !vm->edit.active)
    return 0;
  return vm->edit.cursor_pos;
}

bool vm_table_commit_edit(VmTable *vm) {
  if (!vm || !vm->edit.active || !vm->tab)
    return false;

  /* Get connection for this tab */
  Connection *conn = app_get_tab_connection(vm->app, vm->tab);
  if (!conn || !conn->conn)
    return false;

  /* Get schema and table name */
  const char *table = vm->tab->table_name;
  TableSchema *schema = vm->tab->schema;

  if (!table || !schema) {
    edit_clear(&vm->edit);
    notify_change(vm, VM_TABLE_CHANGE_EDITING);
    return false;
  }

  /* Get row data */
  ResultSet *data = vm->tab->data;
  if (!data || vm->edit.row >= data->num_rows) {
    edit_clear(&vm->edit);
    notify_change(vm, VM_TABLE_CHANGE_EDITING);
    return false;
  }

  Row *row = &data->rows[vm->edit.row];
  size_t col = vm->edit.col;

  /* Find primary key columns */
  const char **pk_cols = NULL;
  DbValue *pk_vals = NULL;
  size_t num_pk = 0;

  /* Count primary keys */
  for (size_t i = 0; i < schema->num_columns; i++) {
    if (schema->columns[i].primary_key)
      num_pk++;
  }

  if (num_pk == 0) {
    /* No primary key - cannot update */
    if (vm->callbacks.on_edit_complete) {
      vm->callbacks.on_edit_complete(vm, false, "Table has no primary key",
                                     vm->callbacks.context);
    }
    edit_clear(&vm->edit);
    notify_change(vm, VM_TABLE_CHANGE_EDITING);
    return false;
  }

  pk_cols = malloc(num_pk * sizeof(char *));
  pk_vals = malloc(num_pk * sizeof(DbValue));
  if (!pk_cols || !pk_vals) {
    free(pk_cols);
    free(pk_vals);
    edit_clear(&vm->edit);
    notify_change(vm, VM_TABLE_CHANGE_EDITING);
    return false;
  }

  /* Collect primary key column names and values */
  size_t pk_idx = 0;
  for (size_t i = 0; i < schema->num_columns && pk_idx < num_pk; i++) {
    if (schema->columns[i].primary_key) {
      pk_cols[pk_idx] = schema->columns[i].name;
      if (i < row->num_cells) {
        pk_vals[pk_idx] = row->cells[i];
      } else {
        pk_vals[pk_idx] = db_value_null();
      }
      pk_idx++;
    }
  }

  /* Build new value */
  DbValue new_val;
  const char *new_text = vm->edit.buffer;
  if (new_text && *new_text) {
    new_val = db_value_text(new_text);
  } else {
    new_val = db_value_null();
  }

  /* Get column name */
  const char *col_name = NULL;
  if (col < schema->num_columns) {
    col_name = schema->columns[col].name;
  }

  /* Update cell in database */
  char *err = NULL;
  bool success = false;

  if (col_name) {
    success = db_update_cell(conn->conn, table, pk_cols, pk_vals, num_pk,
                             col_name, &new_val, &err);
  }

  /* Clean up temporary value if we created it */
  if (new_val.type == DB_TYPE_TEXT && new_val.text.data) {
    db_value_free(&new_val);
  }

  free(pk_cols);
  free(pk_vals);

  if (success) {
    /* Update local data */
    DbValue *cell = &row->cells[col];
    db_value_free(cell);

    if (new_text && *new_text) {
      *cell = db_value_text(new_text);
    } else {
      *cell = db_value_null();
    }

    edit_clear(&vm->edit);
    notify_change(vm, VM_TABLE_CHANGE_DATA | VM_TABLE_CHANGE_EDITING);

    if (vm->callbacks.on_edit_complete) {
      vm->callbacks.on_edit_complete(vm, true, NULL, vm->callbacks.context);
    }
  } else {
    if (vm->callbacks.on_edit_complete) {
      vm->callbacks.on_edit_complete(vm, false, err, vm->callbacks.context);
    }
    free(err);
  }

  return success;
}

void vm_table_cancel_edit(VmTable *vm) {
  if (!vm || !vm->edit.active)
    return;

  edit_clear(&vm->edit);
  notify_change(vm, VM_TABLE_CHANGE_EDITING);
}

/* ============================================================================
 * Sorting
 * ============================================================================
 */

void vm_table_sort_by(VmTable *vm, size_t col, bool descending) {
  if (!vm)
    return;

  vm->sort_column = col;
  vm->sort_descending = descending;
  vm->sort_active = true;

  /* Trigger data reload with new sort */
  vm_table_refresh(vm);
}

void vm_table_toggle_sort(VmTable *vm, size_t col) {
  if (!vm)
    return;

  if (vm->sort_active && vm->sort_column == col) {
    if (vm->sort_descending) {
      /* Already descending, clear sort */
      vm_table_clear_sort(vm);
    } else {
      /* Was ascending, switch to descending */
      vm_table_sort_by(vm, col, true);
    }
  } else {
    /* New column, sort ascending */
    vm_table_sort_by(vm, col, false);
  }
}

void vm_table_clear_sort(VmTable *vm) {
  if (!vm)
    return;

  vm->sort_active = false;
  vm_table_refresh(vm);
}

bool vm_table_is_sorted(const VmTable *vm) { return vm && vm->sort_active; }

size_t vm_table_sort_column(const VmTable *vm) {
  if (!vm)
    return 0;
  return vm->sort_column;
}

bool vm_table_sort_descending(const VmTable *vm) {
  return vm && vm->sort_descending;
}

/* ============================================================================
 * Pagination
 * ============================================================================
 */

void vm_table_set_visible_range(VmTable *vm, size_t first, size_t count) {
  if (!vm)
    return;

  vm->visible_first_row = first;
  vm->visible_row_count = count;

  /* Check if we need to load more data */
  vm_table_ensure_row_loaded(vm, first);
  if (count > 0) {
    vm_table_ensure_row_loaded(vm, first + count - 1);
  }
}

void vm_table_ensure_row_loaded(VmTable *vm, size_t row) {
  if (!vm || !vm->tab)
    return;

  /* Check if row is already loaded */
  if (vm_table_row_loaded(vm, row))
    return;

  /* Need to load this row - this will be handled by the UI layer
   * which calls the appropriate data loading function */
  notify_change(vm, VM_TABLE_CHANGE_LOADING);
}

bool vm_table_is_loading(const VmTable *vm) {
  if (!vm || !vm->tab)
    return false;
  return vm->tab->bg_load_op != NULL;
}

size_t vm_table_loaded_offset(const VmTable *vm) {
  if (!vm || !vm->tab)
    return 0;
  return vm->tab->loaded_offset;
}

size_t vm_table_loaded_count(const VmTable *vm) {
  if (!vm || !vm->tab)
    return 0;
  return vm->tab->loaded_count;
}

/* ============================================================================
 * Actions
 * ============================================================================
 */

bool vm_table_delete_selected(VmTable *vm, char **error) {
  if (!vm || vm->selection.count == 0) {
    if (error)
      *error = str_dup("No rows selected");
    return false;
  }

  /* Get connection */
  Connection *conn = app_get_tab_connection(vm->app, vm->tab);
  if (!conn || !conn->conn) {
    if (error)
      *error = str_dup("No database connection");
    return false;
  }

  /* Get table info */
  const char *table = vm->tab->table_name;
  TableSchema *schema = vm->tab->schema;
  ResultSet *data = vm->tab->data;

  if (!table || !schema || !data) {
    if (error)
      *error = str_dup("No table data");
    return false;
  }

  /* Delete rows (in reverse order to maintain indices) */
  /* Sort selection in descending order */
  /* TODO: Implement proper deletion with transaction */

  if (error)
    *error = str_dup("Delete not implemented yet");
  return false;
}

void vm_table_refresh(VmTable *vm) {
  if (!vm)
    return;

  notify_change(vm, VM_TABLE_CHANGE_DATA | VM_TABLE_CHANGE_LOADING);
}

char *vm_table_copy_cell(const VmTable *vm) {
  if (!vm_table_valid(vm))
    return NULL;

  size_t row, col;
  vm_table_get_cursor(vm, &row, &col);

  const DbValue *val = vm_table_cell(vm, row, col);
  if (!val)
    return str_dup("");

  return db_value_to_string(val);
}

char *vm_table_copy_selection(const VmTable *vm, bool include_headers) {
  /* TODO: Implement multi-row copy with tab separation */
  (void)include_headers;
  return vm_table_copy_cell(vm);
}

/* ============================================================================
 * Column Widths
 * ============================================================================
 */

void vm_table_recalc_column_widths(VmTable *vm) {
  if (!vm_table_valid(vm))
    return;

  ResultSet *data = vm->tab->data;
  size_t num_cols = data->num_columns;

  /* Reallocate if needed */
  if (vm->num_col_widths != num_cols) {
    free(vm->col_widths);
    vm->col_widths = calloc(num_cols, sizeof(int));
    if (!vm->col_widths) {
      vm->num_col_widths = 0;
      return;
    }
    vm->num_col_widths = num_cols;
  }

  /* Calculate widths */
  for (size_t c = 0; c < num_cols; c++) {
    /* Start with column name width */
    int width = data->columns[c].name ? (int)strlen(data->columns[c].name) : 4;

    /* Check data widths */
    for (size_t r = 0; r < data->num_rows && r < 100; r++) {
      if (!data->rows[r].cells || c >= data->rows[r].num_cells)
        continue;

      const DbValue *val = &data->rows[r].cells[c];
      if (val->is_null) {
        if (width < 4)
          width = 4; /* "NULL" */
      } else if (val->type == DB_TYPE_TEXT && val->text.data) {
        int len = (int)strlen(val->text.data);
        if (len > width)
          width = len;
      } else {
        char *str = db_value_to_string(val);
        if (str) {
          int len = (int)strlen(str);
          if (len > width)
            width = len;
          free(str);
        }
      }
    }

    /* Clamp to reasonable bounds */
    if (width < 4)
      width = 4;
    if (width > 50)
      width = 50;

    vm->col_widths[c] = width;
  }

  notify_change(vm, VM_TABLE_CHANGE_COLUMNS);
}

void vm_table_set_column_width(VmTable *vm, size_t col, int width) {
  if (!vm || col >= vm->num_col_widths)
    return;

  vm->col_widths[col] = width;
  notify_change(vm, VM_TABLE_CHANGE_COLUMNS);
}

/* ============================================================================
 * Utility
 * ============================================================================
 */

DbConnection *vm_table_connection(const VmTable *vm) {
  if (!vm || !vm->app || !vm->tab)
    return NULL;

  Connection *conn = app_get_tab_connection(vm->app, vm->tab);
  return conn ? conn->conn : NULL;
}

const char *vm_table_name(const VmTable *vm) {
  if (!vm || !vm->tab)
    return NULL;
  return vm->tab->table_name;
}

const TableSchema *vm_table_schema(const VmTable *vm) {
  if (!vm || !vm->tab)
    return NULL;
  return vm->tab->schema;
}
