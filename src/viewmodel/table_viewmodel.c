/*
 * Lace
 * TableViewModel - Table data display view model
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "table_viewmodel.h"
#include "../core/constants.h"
#include "../util/mem.h"
#include "../util/str.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Forward declarations for ViewModelOps
 * ============================================================================
 */

static bool table_vm_handle_event(ViewModel *vm, const UiEvent *event);
static size_t table_vm_get_row_count(const ViewModel *vm);
static size_t table_vm_get_col_count(const ViewModel *vm);
static void table_vm_on_focus_in(ViewModel *vm);
static void table_vm_on_focus_out(ViewModel *vm);
static void table_vm_validate_cursor_impl(ViewModel *vm);
static void table_vm_ops_destroy(ViewModel *vm);

/* ViewModelOps vtable for TableViewModel */
static const ViewModelOps s_table_vm_ops = {
    .type_name = "TableViewModel",
    .handle_event = table_vm_handle_event,
    .get_row_count = table_vm_get_row_count,
    .get_col_count = table_vm_get_col_count,
    .on_focus_in = table_vm_on_focus_in,
    .on_focus_out = table_vm_on_focus_out,
    .validate_cursor = table_vm_validate_cursor_impl,
    .destroy = table_vm_ops_destroy,
};

const ViewModelOps *table_vm_ops(void) { return &s_table_vm_ops; }

/* ============================================================================
 * ViewModelOps implementations
 * ============================================================================
 */

static bool table_vm_handle_event(ViewModel *vm, const UiEvent *event) {
  (void)vm;
  (void)event;
  /* Input handling will be implemented when we integrate with TUI.
   * For now, input is still routed through the existing tui.c handlers. */
  return false;
}

static size_t table_vm_get_row_count(const ViewModel *vm) {
  const TableViewModel *tvm = (const TableViewModel *)vm;
  return table_vm_row_count(tvm);
}

static size_t table_vm_get_col_count(const ViewModel *vm) {
  const TableViewModel *tvm = (const TableViewModel *)vm;
  return table_vm_col_count(tvm);
}

static void table_vm_on_focus_in(ViewModel *vm) {
  (void)vm;
  /* Could update visual state here if needed */
}

static void table_vm_on_focus_out(ViewModel *vm) {
  TableViewModel *tvm = (TableViewModel *)vm;
  /* Cancel edit when losing focus */
  if (tvm->edit.active) {
    table_vm_cancel_edit(tvm);
  }
}

static void table_vm_validate_cursor_impl(ViewModel *vm) {
  TableViewModel *tvm = (TableViewModel *)vm;
  size_t row_count = table_vm_row_count(tvm);
  size_t col_count = table_vm_col_count(tvm);

  if (row_count > 0 && vm->state.cursor_row >= row_count) {
    vm->state.cursor_row = row_count - 1;
    vm_mark_dirty(vm, VM_CHANGE_CURSOR);
  }

  if (col_count > 0 && vm->state.cursor_col >= col_count) {
    vm->state.cursor_col = col_count - 1;
    vm_mark_dirty(vm, VM_CHANGE_CURSOR);
  }
}

static void table_vm_ops_destroy(ViewModel *vm) {
  TableViewModel *tvm = (TableViewModel *)vm;

  /* Free selection array */
  free(tvm->selection.rows);
  tvm->selection.rows = NULL;
  tvm->selection.count = 0;
  tvm->selection.capacity = 0;

  /* Free edit state */
  free(tvm->edit.buffer);
  free(tvm->edit.original);
  tvm->edit.buffer = NULL;
  tvm->edit.original = NULL;
  tvm->edit.active = false;

  /* Free column widths */
  free(tvm->col_widths);
  tvm->col_widths = NULL;
  tvm->num_col_widths = 0;

  /* Clear callbacks */
  memset(&tvm->table_callbacks, 0, sizeof(tvm->table_callbacks));

  /* Clear bindings (we don't own these) */
  tvm->tab = NULL;
  tvm->app = NULL;
  tvm->data = NULL;
  tvm->schema = NULL;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

TableViewModel *table_vm_create(AppState *app, Tab *tab) {
  TableViewModel *vm = safe_calloc(1, sizeof(TableViewModel));

  /* Initialize base viewmodel */
  vm_init(&vm->base, &s_table_vm_ops);

  /* Bind to tab */
  vm->app = app;
  table_vm_bind(vm, tab);

  return vm;
}

void table_vm_destroy(TableViewModel *vm) {
  if (!vm)
    return;

  vm_cleanup(&vm->base);
  free(vm);
}

void table_vm_bind(TableViewModel *vm, Tab *tab) {
  if (!vm)
    return;

  /* Clear edit state */
  if (vm->edit.active) {
    table_vm_cancel_edit(vm);
  }

  /* Clear selection */
  vm->selection.count = 0;
  vm->selection.anchor_set = false;

  /* Bind to new tab */
  vm->tab = tab;

  if (tab) {
    vm->data = tab->data;
    vm->schema = tab->schema;
    vm->loaded_offset = tab->loaded_offset;
    vm->loaded_count = tab->loaded_count;
    vm->total_rows = tab->total_rows;
    vm->row_count_approximate = tab->row_count_approximate;

    /* Copy sort state */
    vm->num_sort_entries = tab->num_sort_entries;
    memcpy(vm->sort_entries, tab->sort_entries,
           tab->num_sort_entries * sizeof(SortEntry));

    /* Sync cursor/scroll from tab (during migration) */
    table_vm_sync_from_tab(vm);

    /* Recalculate column widths */
    table_vm_recalc_column_widths(vm);
  } else {
    vm->data = NULL;
    vm->schema = NULL;
    vm->loaded_offset = 0;
    vm->loaded_count = 0;
    vm->total_rows = 0;
    vm->num_sort_entries = 0;
  }

  vm_notify(&vm->base, VM_CHANGE_DATA);
}

void table_vm_set_callbacks(TableViewModel *vm,
                            const TableViewModelCallbacks *callbacks) {
  if (!vm)
    return;

  if (callbacks) {
    vm->table_callbacks = *callbacks;
  } else {
    memset(&vm->table_callbacks, 0, sizeof(vm->table_callbacks));
  }
}

/* ============================================================================
 * Data Access
 * ============================================================================
 */

size_t table_vm_row_count(const TableViewModel *vm) {
  if (!vm || !vm->data)
    return 0;
  return vm->data->num_rows;
}

size_t table_vm_col_count(const TableViewModel *vm) {
  if (!vm || !vm->data)
    return 0;
  return vm->data->num_columns;
}

size_t table_vm_total_rows(const TableViewModel *vm) {
  return vm ? vm->total_rows : 0;
}

const char *table_vm_column_name(const TableViewModel *vm, size_t col) {
  if (!vm || !vm->schema || col >= vm->schema->num_columns)
    return NULL;
  return vm->schema->columns[col].name;
}

DbValueType table_vm_column_type(const TableViewModel *vm, size_t col) {
  if (!vm || !vm->schema || col >= vm->schema->num_columns)
    return DB_TYPE_NULL;
  return vm->schema->columns[col].type;
}

bool table_vm_column_nullable(const TableViewModel *vm, size_t col) {
  if (!vm || !vm->schema || col >= vm->schema->num_columns)
    return true;
  return vm->schema->columns[col].nullable;
}

bool table_vm_column_is_pk(const TableViewModel *vm, size_t col) {
  if (!vm || !vm->schema || col >= vm->schema->num_columns)
    return false;
  return vm->schema->columns[col].primary_key;
}

int table_vm_column_width(const TableViewModel *vm, size_t col) {
  if (!vm || col >= vm->num_col_widths)
    return DEFAULT_COL_WIDTH;
  return vm->col_widths[col];
}

const DbValue *table_vm_cell(const TableViewModel *vm, size_t row, size_t col) {
  if (!vm || !vm->data || row >= vm->data->num_rows ||
      col >= vm->data->num_columns)
    return NULL;
  if (!vm->data->rows)
    return NULL;
  Row *r = &vm->data->rows[row];
  if (!r->cells || col >= r->num_cells)
    return NULL;
  return &r->cells[col];
}

const char *table_vm_cell_text(const TableViewModel *vm, size_t row,
                               size_t col) {
  const DbValue *val = table_vm_cell(vm, row, col);
  if (!val || val->is_null)
    return NULL;

  switch (val->type) {
  case DB_TYPE_TEXT:
    return val->text.data;
  case DB_TYPE_INT:
  case DB_TYPE_FLOAT:
  case DB_TYPE_BLOB:
    /* Could format these, but for now return NULL */
    return NULL;
  case DB_TYPE_NULL:
  default:
    return NULL;
  }
}

bool table_vm_cell_is_null(const TableViewModel *vm, size_t row, size_t col) {
  const DbValue *val = table_vm_cell(vm, row, col);
  return !val || val->is_null || val->type == DB_TYPE_NULL;
}

const char *table_vm_table_name(const TableViewModel *vm) {
  if (!vm || !vm->tab)
    return NULL;
  return vm->tab->table_name;
}

const TableSchema *table_vm_get_schema(const TableViewModel *vm) {
  if (!vm || !vm->tab)
    return NULL;
  return vm->tab->schema;
}

const char *table_vm_column_fk(const TableViewModel *vm, size_t col) {
  if (!vm || !vm->schema || col >= vm->schema->num_columns)
    return NULL;
  return vm->schema->columns[col].foreign_key;
}

bool table_vm_column_auto_increment(const TableViewModel *vm, size_t col) {
  if (!vm || !vm->schema || col >= vm->schema->num_columns)
    return false;
  return vm->schema->columns[col].auto_increment;
}

const char *table_vm_column_default(const TableViewModel *vm, size_t col) {
  if (!vm || !vm->schema || col >= vm->schema->num_columns)
    return NULL;
  return vm->schema->columns[col].default_val;
}

size_t table_vm_pk_columns(const TableViewModel *vm, size_t *pk_cols,
                           size_t max_cols) {
  if (!vm || !vm->schema)
    return 0;

  size_t count = 0;
  for (size_t i = 0; i < vm->schema->num_columns; i++) {
    if (vm->schema->columns[i].primary_key) {
      if (pk_cols && count < max_cols)
        pk_cols[count] = i;
      count++;
    }
  }
  return count;
}

bool table_vm_valid(const TableViewModel *vm) {
  return vm && vm->tab && vm->data;
}

/* ============================================================================
 * Pagination
 * ============================================================================
 */

size_t table_vm_loaded_offset(const TableViewModel *vm) {
  return vm ? vm->loaded_offset : 0;
}

size_t table_vm_loaded_count(const TableViewModel *vm) {
  return vm ? vm->loaded_count : 0;
}

bool table_vm_is_loading(const TableViewModel *vm) {
  return vm ? vm->is_loading : false;
}

void table_vm_set_visible_range(TableViewModel *vm, size_t first,
                                size_t count) {
  if (!vm)
    return;
  vm->visible_first_row = first;
  vm->visible_row_count = count;
}

void table_vm_update_pagination(TableViewModel *vm, size_t offset, size_t count,
                                size_t total) {
  if (!vm)
    return;

  vm->loaded_offset = offset;
  vm->loaded_count = count;
  vm->total_rows = total;

  /* Also update tab for sync */
  if (vm->tab) {
    vm->tab->loaded_offset = offset;
    vm->tab->loaded_count = count;
    vm->tab->total_rows = total;
  }

  vm_mark_dirty(&vm->base, TABLE_VM_CHANGE_LOADING);
}

/* ============================================================================
 * Selection
 * ============================================================================
 */

static bool selection_contains(const TableSelection *sel, size_t row) {
  for (size_t i = 0; i < sel->count; i++) {
    if (sel->rows[i] == row)
      return true;
  }
  return false;
}

static bool selection_add(TableSelection *sel, size_t row) {
  if (selection_contains(sel, row))
    return false;

  /* Grow if needed */
  if (sel->count >= sel->capacity) {
    size_t new_cap = sel->capacity == 0 ? 16 : sel->capacity * 2;
    sel->rows = safe_reallocarray(sel->rows, new_cap, sizeof(size_t));
    sel->capacity = new_cap;
  }

  sel->rows[sel->count++] = row;
  return true;
}

static bool selection_remove(TableSelection *sel, size_t row) {
  for (size_t i = 0; i < sel->count; i++) {
    if (sel->rows[i] == row) {
      /* Shift remaining elements */
      memmove(&sel->rows[i], &sel->rows[i + 1],
              (sel->count - i - 1) * sizeof(size_t));
      sel->count--;
      return true;
    }
  }
  return false;
}

void table_vm_select_row(TableViewModel *vm, size_t row) {
  if (!vm)
    return;
  if (selection_add(&vm->selection, row)) {
    vm_notify(&vm->base, VM_CHANGE_SELECTION);
  }
}

void table_vm_deselect_row(TableViewModel *vm, size_t row) {
  if (!vm)
    return;
  if (selection_remove(&vm->selection, row)) {
    vm_notify(&vm->base, VM_CHANGE_SELECTION);
  }
}

void table_vm_toggle_row_selection(TableViewModel *vm, size_t row) {
  if (!vm)
    return;
  if (selection_contains(&vm->selection, row)) {
    selection_remove(&vm->selection, row);
  } else {
    selection_add(&vm->selection, row);
  }
  vm_notify(&vm->base, VM_CHANGE_SELECTION);
}

bool table_vm_row_selected(const TableViewModel *vm, size_t row) {
  if (!vm)
    return false;
  return selection_contains(&vm->selection, row);
}

void table_vm_select_range(TableViewModel *vm, size_t from, size_t to) {
  if (!vm)
    return;

  size_t start = (from < to) ? from : to;
  size_t end = (from < to) ? to : from;

  for (size_t i = start; i <= end; i++) {
    selection_add(&vm->selection, i);
  }

  vm_notify(&vm->base, VM_CHANGE_SELECTION);
}

void table_vm_extend_selection(TableViewModel *vm, size_t to_row) {
  if (!vm)
    return;

  if (!vm->selection.anchor_set) {
    vm->selection.anchor = vm->base.state.cursor_row;
    vm->selection.anchor_set = true;
  }

  table_vm_clear_selection(vm);
  table_vm_select_range(vm, vm->selection.anchor, to_row);
}

void table_vm_select_all(TableViewModel *vm) {
  if (!vm)
    return;

  size_t row_count = table_vm_row_count(vm);
  for (size_t i = 0; i < row_count; i++) {
    selection_add(&vm->selection, i);
  }

  vm_notify(&vm->base, VM_CHANGE_SELECTION);
}

void table_vm_clear_selection(TableViewModel *vm) {
  if (!vm)
    return;

  if (vm->selection.count > 0) {
    vm->selection.count = 0;
    vm->selection.anchor_set = false;
    vm_notify(&vm->base, VM_CHANGE_SELECTION);
  }
}

size_t table_vm_selection_count(const TableViewModel *vm) {
  return vm ? vm->selection.count : 0;
}

const size_t *table_vm_selected_rows(const TableViewModel *vm) {
  return vm ? vm->selection.rows : NULL;
}

/* ============================================================================
 * Editing
 * ============================================================================
 */

#define EDIT_BUFFER_INITIAL_SIZE 256

bool table_vm_start_edit(TableViewModel *vm, size_t row, size_t col) {
  if (!vm || !table_vm_valid(vm))
    return false;

  if (row >= table_vm_row_count(vm) || col >= table_vm_col_count(vm))
    return false;

  /* Cancel any existing edit */
  if (vm->edit.active) {
    table_vm_cancel_edit(vm);
  }

  /* Get current cell value */
  const DbValue *val = table_vm_cell(vm, row, col);
  const char *text = "";
  if (val && !val->is_null && val->type == DB_TYPE_TEXT && val->text.data) {
    text = val->text.data;
  }

  /* Allocate edit buffer */
  size_t text_len = strlen(text);
  size_t buf_cap = text_len + EDIT_BUFFER_INITIAL_SIZE;
  char *buffer = safe_malloc(buf_cap);

  memcpy(buffer, text, text_len + 1); /* +1 for null terminator */

  /* Store original for cancel */
  char *original = str_dup(text);

  /* Set up edit state */
  vm->edit.active = true;
  vm->edit.row = row;
  vm->edit.col = col;
  vm->edit.buffer = buffer;
  vm->edit.buffer_len = text_len;
  vm->edit.buffer_cap = buf_cap;
  vm->edit.cursor_pos = text_len;
  vm->edit.original = original;

  vm_notify(&vm->base, VM_CHANGE_EDITING);
  return true;
}

bool table_vm_start_edit_at_cursor(TableViewModel *vm) {
  if (!vm)
    return false;
  return table_vm_start_edit(vm, vm->base.state.cursor_row,
                             vm->base.state.cursor_col);
}

void table_vm_edit_insert_char(TableViewModel *vm, char ch) {
  if (!vm || !vm->edit.active)
    return;

  /* Grow buffer if needed */
  if (vm->edit.buffer_len + 2 > vm->edit.buffer_cap) {
    size_t new_cap = vm->edit.buffer_cap * 2;
    vm->edit.buffer = safe_realloc(vm->edit.buffer, new_cap);
    vm->edit.buffer_cap = new_cap;
  }

  /* Insert character at cursor position */
  memmove(&vm->edit.buffer[vm->edit.cursor_pos + 1],
          &vm->edit.buffer[vm->edit.cursor_pos],
          vm->edit.buffer_len - vm->edit.cursor_pos + 1);
  vm->edit.buffer[vm->edit.cursor_pos] = ch;
  vm->edit.cursor_pos++;
  vm->edit.buffer_len++;

  vm_notify(&vm->base, VM_CHANGE_EDITING);
}

void table_vm_edit_insert_text(TableViewModel *vm, const char *text) {
  if (!vm || !vm->edit.active || !text)
    return;

  size_t insert_len = strlen(text);
  if (insert_len == 0)
    return;

  /* Grow buffer if needed */
  while (vm->edit.buffer_len + insert_len + 1 > vm->edit.buffer_cap) {
    size_t new_cap = vm->edit.buffer_cap * 2;
    vm->edit.buffer = safe_realloc(vm->edit.buffer, new_cap);
    vm->edit.buffer_cap = new_cap;
  }

  /* Insert text at cursor position */
  memmove(&vm->edit.buffer[vm->edit.cursor_pos + insert_len],
          &vm->edit.buffer[vm->edit.cursor_pos],
          vm->edit.buffer_len - vm->edit.cursor_pos + 1);
  memcpy(&vm->edit.buffer[vm->edit.cursor_pos], text, insert_len);
  vm->edit.cursor_pos += insert_len;
  vm->edit.buffer_len += insert_len;

  vm_notify(&vm->base, VM_CHANGE_EDITING);
}

void table_vm_edit_delete_char(TableViewModel *vm) {
  if (!vm || !vm->edit.active)
    return;

  if (vm->edit.cursor_pos >= vm->edit.buffer_len)
    return;

  memmove(&vm->edit.buffer[vm->edit.cursor_pos],
          &vm->edit.buffer[vm->edit.cursor_pos + 1],
          vm->edit.buffer_len - vm->edit.cursor_pos);
  vm->edit.buffer_len--;

  vm_notify(&vm->base, VM_CHANGE_EDITING);
}

void table_vm_edit_backspace(TableViewModel *vm) {
  if (!vm || !vm->edit.active)
    return;

  if (vm->edit.cursor_pos == 0)
    return;

  vm->edit.cursor_pos--;
  table_vm_edit_delete_char(vm);
}

void table_vm_edit_clear(TableViewModel *vm) {
  if (!vm || !vm->edit.active)
    return;

  vm->edit.buffer[0] = '\0';
  vm->edit.buffer_len = 0;
  vm->edit.cursor_pos = 0;

  vm_notify(&vm->base, VM_CHANGE_EDITING);
}

void table_vm_edit_set_cursor(TableViewModel *vm, size_t pos) {
  if (!vm || !vm->edit.active)
    return;

  if (pos > vm->edit.buffer_len)
    pos = vm->edit.buffer_len;
  vm->edit.cursor_pos = pos;

  vm_notify(&vm->base, VM_CHANGE_EDITING);
}

void table_vm_edit_move_cursor(TableViewModel *vm, int delta) {
  if (!vm || !vm->edit.active)
    return;

  if (delta < 0) {
    size_t d = (size_t)(-delta);
    vm->edit.cursor_pos = (vm->edit.cursor_pos > d) ? vm->edit.cursor_pos - d : 0;
  } else {
    vm->edit.cursor_pos += (size_t)delta;
    if (vm->edit.cursor_pos > vm->edit.buffer_len)
      vm->edit.cursor_pos = vm->edit.buffer_len;
  }

  vm_notify(&vm->base, VM_CHANGE_EDITING);
}

void table_vm_edit_home(TableViewModel *vm) { table_vm_edit_set_cursor(vm, 0); }

void table_vm_edit_end(TableViewModel *vm) {
  if (!vm || !vm->edit.active)
    return;
  table_vm_edit_set_cursor(vm, vm->edit.buffer_len);
}

bool table_vm_is_editing(const TableViewModel *vm) {
  return vm ? vm->edit.active : false;
}

const char *table_vm_edit_buffer(const TableViewModel *vm) {
  if (!vm || !vm->edit.active)
    return NULL;
  return vm->edit.buffer;
}

size_t table_vm_edit_cursor(const TableViewModel *vm) {
  if (!vm || !vm->edit.active)
    return 0;
  return vm->edit.cursor_pos;
}

void table_vm_get_edit_cell(const TableViewModel *vm, size_t *row,
                            size_t *col) {
  if (!vm || !vm->edit.active) {
    if (row)
      *row = 0;
    if (col)
      *col = 0;
    return;
  }

  if (row)
    *row = vm->edit.row;
  if (col)
    *col = vm->edit.col;
}

bool table_vm_commit_edit(TableViewModel *vm) {
  if (!vm || !vm->edit.active)
    return false;

  /* Note: Actual database update is handled by the TUI layer.
   * This just cleans up the edit state and notifies. */

  bool success = true; /* Assume success for now */

  /* Notify callback */
  if (vm->table_callbacks.on_edit_complete) {
    vm->table_callbacks.on_edit_complete(vm, success, NULL,
                                         vm->table_callbacks.context);
  }

  /* Clean up edit state */
  free(vm->edit.buffer);
  free(vm->edit.original);
  vm->edit.buffer = NULL;
  vm->edit.original = NULL;
  vm->edit.active = false;

  vm_notify(&vm->base, VM_CHANGE_EDITING | VM_CHANGE_DATA);
  return success;
}

void table_vm_cancel_edit(TableViewModel *vm) {
  if (!vm || !vm->edit.active)
    return;

  /* Clean up edit state */
  free(vm->edit.buffer);
  free(vm->edit.original);
  vm->edit.buffer = NULL;
  vm->edit.original = NULL;
  vm->edit.active = false;

  vm_notify(&vm->base, VM_CHANGE_EDITING);
}

/* ============================================================================
 * Sorting
 * ============================================================================
 */

void table_vm_sort_by(TableViewModel *vm, size_t col, bool descending) {
  if (!vm)
    return;

  vm->num_sort_entries = 1;
  vm->sort_entries[0].column = col;
  vm->sort_entries[0].direction = descending ? SORT_DESC : SORT_ASC;

  /* Sync to tab */
  if (vm->tab) {
    vm->tab->num_sort_entries = 1;
    vm->tab->sort_entries[0] = vm->sort_entries[0];
  }

  vm_mark_dirty(&vm->base, TABLE_VM_CHANGE_SORT);
}

void table_vm_toggle_sort(TableViewModel *vm, size_t col) {
  if (!vm)
    return;

  /* Check if already sorting by this column */
  if (vm->num_sort_entries == 1 && vm->sort_entries[0].column == col) {
    /* Toggle direction */
    vm->sort_entries[0].direction =
        (vm->sort_entries[0].direction == SORT_ASC) ? SORT_DESC : SORT_ASC;
  } else {
    /* Sort by this column ascending */
    vm->num_sort_entries = 1;
    vm->sort_entries[0].column = col;
    vm->sort_entries[0].direction = SORT_ASC;
  }

  /* Sync to tab */
  if (vm->tab) {
    vm->tab->num_sort_entries = vm->num_sort_entries;
    memcpy(vm->tab->sort_entries, vm->sort_entries,
           vm->num_sort_entries * sizeof(SortEntry));
  }

  vm_mark_dirty(&vm->base, TABLE_VM_CHANGE_SORT);
}

void table_vm_add_sort(TableViewModel *vm, size_t col, bool descending) {
  if (!vm || vm->num_sort_entries >= MAX_SORT_COLUMNS)
    return;

  SortDirection dir = descending ? SORT_DESC : SORT_ASC;

  /* Check if already sorting by this column */
  for (size_t i = 0; i < vm->num_sort_entries; i++) {
    if (vm->sort_entries[i].column == col) {
      /* Just update direction */
      vm->sort_entries[i].direction = dir;
      goto sync_and_notify;
    }
  }

  /* Add new sort column */
  vm->sort_entries[vm->num_sort_entries].column = col;
  vm->sort_entries[vm->num_sort_entries].direction = dir;
  vm->num_sort_entries++;

sync_and_notify:
  /* Sync to tab */
  if (vm->tab) {
    vm->tab->num_sort_entries = vm->num_sort_entries;
    memcpy(vm->tab->sort_entries, vm->sort_entries,
           vm->num_sort_entries * sizeof(SortEntry));
  }

  vm_mark_dirty(&vm->base, TABLE_VM_CHANGE_SORT);
}

void table_vm_clear_sort(TableViewModel *vm) {
  if (!vm)
    return;

  vm->num_sort_entries = 0;

  /* Sync to tab */
  if (vm->tab) {
    vm->tab->num_sort_entries = 0;
  }

  vm_mark_dirty(&vm->base, TABLE_VM_CHANGE_SORT);
}

bool table_vm_is_sorted(const TableViewModel *vm) {
  return vm && vm->num_sort_entries > 0;
}

size_t table_vm_sort_column_count(const TableViewModel *vm) {
  return vm ? vm->num_sort_entries : 0;
}

const SortEntry *table_vm_sort_entries(const TableViewModel *vm) {
  return vm ? vm->sort_entries : NULL;
}

/* ============================================================================
 * Column Widths
 * ============================================================================
 */

void table_vm_recalc_column_widths(TableViewModel *vm) {
  if (!vm || !vm->data)
    return;

  size_t num_cols = vm->data->num_columns;
  if (num_cols == 0)
    return;

  /* Resize array if needed */
  if (vm->num_col_widths != num_cols) {
    vm->col_widths = safe_reallocarray(vm->col_widths, num_cols, sizeof(int));
    vm->num_col_widths = num_cols;
  }

  /* Calculate width for each column */
  for (size_t col = 0; col < num_cols; col++) {
    int width = MIN_COL_WIDTH;

    /* Consider header width */
    if (vm->schema && col < vm->schema->num_columns &&
        vm->schema->columns[col].name) {
      int header_len = (int)strlen(vm->schema->columns[col].name);
      if (header_len > width)
        width = header_len;
    }

    /* Consider data width (sample first PAGE_SIZE rows) */
    size_t sample_count =
        vm->data->num_rows < PAGE_SIZE ? vm->data->num_rows : PAGE_SIZE;
    for (size_t row = 0; row < sample_count; row++) {
      const DbValue *val = table_vm_cell(vm, row, col);
      if (val && !val->is_null && val->type == DB_TYPE_TEXT && val->text.data) {
        int cell_len = (int)strlen(val->text.data);
        if (cell_len > width)
          width = cell_len;
      }
    }

    /* Clamp to max width */
    if (width > MAX_COL_WIDTH)
      width = MAX_COL_WIDTH;

    vm->col_widths[col] = width;
  }

  vm_mark_dirty(&vm->base, TABLE_VM_CHANGE_COLUMN_WIDTHS);
}

void table_vm_set_column_width(TableViewModel *vm, size_t col, int width) {
  if (!vm || col >= vm->num_col_widths)
    return;

  if (width < MIN_COL_WIDTH)
    width = MIN_COL_WIDTH;
  if (width > MAX_COL_WIDTH)
    width = MAX_COL_WIDTH;

  if (vm->col_widths[col] != width) {
    vm->col_widths[col] = width;
    vm_mark_dirty(&vm->base, TABLE_VM_CHANGE_COLUMN_WIDTHS);
  }
}

const int *table_vm_get_column_widths(const TableViewModel *vm,
                                      size_t *num_cols) {
  if (!vm) {
    if (num_cols)
      *num_cols = 0;
    return NULL;
  }

  if (num_cols)
    *num_cols = vm->num_col_widths;
  return vm->col_widths;
}

/* ============================================================================
 * Clipboard
 * ============================================================================
 */

char *table_vm_copy_cell(const TableViewModel *vm) {
  if (!vm)
    return NULL;

  const DbValue *val =
      table_vm_cell(vm, vm->base.state.cursor_row, vm->base.state.cursor_col);
  if (!val)
    return NULL;

  /* Format value as string */
  return db_value_to_string(val);
}

char *table_vm_copy_selection(const TableViewModel *vm, bool include_headers) {
  if (!vm || vm->selection.count == 0)
    return table_vm_copy_cell(vm);

  /* Build tab-separated output */
  size_t buf_size = 4096;
  char *buf = safe_malloc(buf_size);

  size_t buf_len = 0;

  /* Add headers if requested */
  if (include_headers) {
    size_t num_cols = table_vm_col_count(vm);
    for (size_t col = 0; col < num_cols; col++) {
      const char *name = table_vm_column_name(vm, col);
      if (col > 0) {
        buf[buf_len++] = '\t';
      }
      if (name) {
        size_t name_len = strlen(name);
        while (buf_len + name_len + 2 > buf_size) {
          buf_size *= 2;
          buf = safe_realloc(buf, buf_size);
        }
        memcpy(&buf[buf_len], name, name_len);
        buf_len += name_len;
      }
    }
    buf[buf_len++] = '\n';
  }

  /* Add selected rows */
  for (size_t i = 0; i < vm->selection.count; i++) {
    size_t row = vm->selection.rows[i];
    size_t num_cols = table_vm_col_count(vm);

    for (size_t col = 0; col < num_cols; col++) {
      if (col > 0) {
        buf[buf_len++] = '\t';
      }

      const DbValue *val = table_vm_cell(vm, row, col);
      char *cell_str = val ? db_value_to_string(val) : NULL;
      if (cell_str) {
        size_t cell_len = strlen(cell_str);
        while (buf_len + cell_len + 2 > buf_size) {
          buf_size *= 2;
          buf = safe_realloc(buf, buf_size);
        }
        memcpy(&buf[buf_len], cell_str, cell_len);
        buf_len += cell_len;
        free(cell_str);
      }
    }
    buf[buf_len++] = '\n';
  }

  buf[buf_len] = '\0';
  return buf;
}

/* ============================================================================
 * Sync with Tab
 * ============================================================================
 */

void table_vm_sync_from_tab(TableViewModel *vm) {
  if (!vm || !vm->tab)
    return;

  Tab *tab = vm->tab;

  /* Sync cursor/scroll */
  vm->base.state.cursor_row = tab->cursor_row;
  vm->base.state.cursor_col = tab->cursor_col;
  vm->base.state.scroll_row = tab->scroll_row;
  vm->base.state.scroll_col = tab->scroll_col;

  /* Sync pagination */
  vm->loaded_offset = tab->loaded_offset;
  vm->loaded_count = tab->loaded_count;
  vm->total_rows = tab->total_rows;
  vm->row_count_approximate = tab->row_count_approximate;

  /* Sync data pointers */
  vm->data = tab->data;
  vm->schema = tab->schema;

  /* Sync sort */
  vm->num_sort_entries = tab->num_sort_entries;
  memcpy(vm->sort_entries, tab->sort_entries,
         tab->num_sort_entries * sizeof(SortEntry));
}

void table_vm_sync_to_tab(TableViewModel *vm) {
  if (!vm || !vm->tab)
    return;

  Tab *tab = vm->tab;

  /* Sync cursor/scroll */
  tab->cursor_row = vm->base.state.cursor_row;
  tab->cursor_col = vm->base.state.cursor_col;
  tab->scroll_row = vm->base.state.scroll_row;
  tab->scroll_col = vm->base.state.scroll_col;

  /* Sync pagination */
  tab->loaded_offset = vm->loaded_offset;
  tab->loaded_count = vm->loaded_count;
  tab->total_rows = vm->total_rows;
  tab->row_count_approximate = vm->row_count_approximate;

  /* Sync sort */
  tab->num_sort_entries = vm->num_sort_entries;
  memcpy(tab->sort_entries, vm->sort_entries,
         vm->num_sort_entries * sizeof(SortEntry));
}

/* ============================================================================
 * Cursor & Navigation
 * ============================================================================
 */

void table_vm_get_cursor(const TableViewModel *vm, size_t *row, size_t *col) {
  if (!vm) {
    if (row) *row = 0;
    if (col) *col = 0;
    return;
  }
  if (row) *row = vm->base.state.cursor_row;
  if (col) *col = vm->base.state.cursor_col;
}

void table_vm_set_cursor(TableViewModel *vm, size_t row, size_t col) {
  if (!vm)
    return;

  size_t max_row = table_vm_total_rows(vm);
  size_t max_col = table_vm_col_count(vm);

  if (max_row > 0 && row >= max_row)
    row = max_row - 1;
  if (max_col > 0 && col >= max_col)
    col = max_col - 1;

  bool changed =
      (vm->base.state.cursor_row != row || vm->base.state.cursor_col != col);

  vm->base.state.cursor_row = row;
  vm->base.state.cursor_col = col;

  if (changed) {
    vm_notify(&vm->base, VM_CHANGE_CURSOR);
    /* Also sync to tab for compatibility */
    if (vm->tab) {
      vm->tab->cursor_row = row;
      vm->tab->cursor_col = col;
    }
  }
}

void table_vm_move_cursor(TableViewModel *vm, int row_delta, int col_delta) {
  size_t row, col;
  table_vm_get_cursor(vm, &row, &col);

  if (row_delta < 0 && (size_t)(-row_delta) > row) {
    row = 0;
  } else {
    row = (size_t)((int)row + row_delta);
  }

  if (col_delta < 0 && (size_t)(-col_delta) > col) {
    col = 0;
  } else {
    col = (size_t)((int)col + col_delta);
  }

  table_vm_set_cursor(vm, row, col);
}

void table_vm_goto_first_row(TableViewModel *vm) {
  size_t col;
  table_vm_get_cursor(vm, NULL, &col);
  table_vm_set_cursor(vm, 0, col);
}

void table_vm_goto_last_row(TableViewModel *vm) {
  size_t total = table_vm_total_rows(vm);
  size_t col;
  table_vm_get_cursor(vm, NULL, &col);
  if (total > 0) {
    table_vm_set_cursor(vm, total - 1, col);
  }
}

void table_vm_goto_first_col(TableViewModel *vm) {
  size_t row;
  table_vm_get_cursor(vm, &row, NULL);
  table_vm_set_cursor(vm, row, 0);
}

void table_vm_goto_last_col(TableViewModel *vm) {
  size_t cols = table_vm_col_count(vm);
  size_t row;
  table_vm_get_cursor(vm, &row, NULL);
  if (cols > 0) {
    table_vm_set_cursor(vm, row, cols - 1);
  }
}

void table_vm_page_up(TableViewModel *vm, size_t page_size) {
  size_t row, col;
  table_vm_get_cursor(vm, &row, &col);
  if (row > page_size)
    row -= page_size;
  else
    row = 0;
  table_vm_set_cursor(vm, row, col);
}

void table_vm_page_down(TableViewModel *vm, size_t page_size) {
  size_t row, col;
  table_vm_get_cursor(vm, &row, &col);
  row += page_size;
  table_vm_set_cursor(vm, row, col);
}

/* ============================================================================
 * Scroll
 * ============================================================================
 */

void table_vm_get_scroll(const TableViewModel *vm, size_t *row, size_t *col) {
  if (!vm) {
    if (row) *row = 0;
    if (col) *col = 0;
    return;
  }
  if (row) *row = vm->base.state.scroll_row;
  if (col) *col = vm->base.state.scroll_col;
}

void table_vm_set_scroll(TableViewModel *vm, size_t row, size_t col) {
  if (!vm)
    return;

  bool changed =
      (vm->base.state.scroll_row != row || vm->base.state.scroll_col != col);

  vm->base.state.scroll_row = row;
  vm->base.state.scroll_col = col;

  if (changed) {
    vm_notify(&vm->base, VM_CHANGE_SCROLL);
    /* Also sync to tab for compatibility */
    if (vm->tab) {
      vm->tab->scroll_row = row;
      vm->tab->scroll_col = col;
    }
  }
}

void table_vm_ensure_cursor_visible(TableViewModel *vm, size_t visible_rows,
                                    size_t visible_cols) {
  if (!vm)
    return;

  size_t cursor_row = vm->base.state.cursor_row;
  size_t cursor_col = vm->base.state.cursor_col;
  size_t scroll_row = vm->base.state.scroll_row;
  size_t scroll_col = vm->base.state.scroll_col;

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

  table_vm_set_scroll(vm, scroll_row, scroll_col);
}

/* ============================================================================
 * Row Loading
 * ============================================================================
 */

bool table_vm_row_loaded(const TableViewModel *vm, size_t row) {
  if (!vm)
    return false;

  return row >= vm->loaded_offset && row < vm->loaded_offset + vm->loaded_count;
}

void table_vm_ensure_row_loaded(TableViewModel *vm, size_t row) {
  if (!vm)
    return;

  /* If row is not loaded, notify that loading is needed */
  if (!table_vm_row_loaded(vm, row)) {
    vm_notify(&vm->base, TABLE_VM_CHANGE_LOADING);
  }
}

/* ============================================================================
 * Actions
 * ============================================================================
 */

bool table_vm_delete_selected(TableViewModel *vm, char **error) {
  if (!vm || vm->selection.count == 0) {
    if (error)
      *error = str_dup("No rows selected");
    return false;
  }

  /* Note: Actual database deletion is handled by TUI layer.
   * This just validates and signals the intention. */
  if (error)
    *error = str_dup("Delete not implemented in viewmodel");
  return false;
}

void table_vm_refresh(TableViewModel *vm) {
  if (!vm)
    return;

  vm_notify(&vm->base, VM_CHANGE_DATA | TABLE_VM_CHANGE_LOADING);
}

/* ============================================================================
 * Connection
 * ============================================================================
 */

DbConnection *table_vm_connection(const TableViewModel *vm) {
  if (!vm || !vm->app || !vm->tab)
    return NULL;

  Connection *conn = app_get_tab_connection(vm->app, vm->tab);
  return conn ? conn->conn : NULL;
}
