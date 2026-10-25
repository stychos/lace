/*
 * Lace
 * TableViewModel - Table data display view model
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "table_viewmodel.h"

/* The current Tab is the single source of truth for data, schema,
 * pagination, cursor, scroll and sort. Pagination code replaces tab->data and
 * adjusts offsets directly, so the viewmodel must never cache copies of them
 * (a cached data pointer was freed underneath it on "jump to last row").
 * Without a current tab, accesses land in a zeroed scratch Tab. */
static Tab g_vm_unbound_tab;

/* Resolve the tab freshly on every access. Workspace tab arrays are
 * realloc'ed when tabs are added and shifted when tabs close, so a stored
 * Tab* can dangle; the viewmodel always represents the current tab. */
static Tab *vm_tab(const TableViewModel *vm) {
  if (vm->app) {
    Tab *cur = app_current_tab(vm->app);
    if (cur) {
      return cur;
    }
  }
  return &g_vm_unbound_tab;
}
#define VM_T(v) vm_tab(v)
#include "../core/constants.h"
#include "../core/catalog_rows.h"
#include "../core/utf8.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
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
  (void)vm;
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


  /* Free column widths */
  free(tvm->col_widths);
  tvm->col_widths = NULL;
  tvm->num_col_widths = 0;

  tvm->app = NULL; /* Not owned */
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

  /* The tab (or its table) may have changed: measure widths afresh */
  if (tab) {
    table_vm_recalc_column_widths(vm);
    vm->widths_data = NULL; /* Re-measure on next draw against live data */
  }
  vm_notify(&vm->base, VM_CHANGE_DATA);
}

/* ============================================================================
 * Data Access
 * ============================================================================
 */

size_t table_vm_row_count(const TableViewModel *vm) {
  if (!vm || !VM_T(vm)->data)
    return 0;
  return VM_T(vm)->data->num_rows;
}

size_t table_vm_col_count(const TableViewModel *vm) {
  if (!vm || !VM_T(vm)->data)
    return 0;
  return VM_T(vm)->data->num_columns;
}

size_t table_vm_total_rows(const TableViewModel *vm) {
  return vm ? VM_T(vm)->total_rows : 0;
}

const char *table_vm_column_name(const TableViewModel *vm, size_t col) {
  if (!vm || !VM_T(vm)->schema || col >= VM_T(vm)->schema->num_columns)
    return NULL;
  return VM_T(vm)->schema->columns[col].name;
}

const char *table_vm_result_column_name(const TableViewModel *vm, size_t col) {
  if (!vm || !VM_T(vm)->data || col >= VM_T(vm)->data->num_columns ||
      !VM_T(vm)->data->columns)
    return NULL;
  return VM_T(vm)->data->columns[col].name;
}

const DbValue *table_vm_cell(const TableViewModel *vm, size_t row, size_t col) {
  if (!vm || !VM_T(vm)->data || row >= VM_T(vm)->data->num_rows ||
      col >= VM_T(vm)->data->num_columns)
    return NULL;
  if (!VM_T(vm)->data->rows)
    return NULL;
  Row *r = &VM_T(vm)->data->rows[row];
  if (!r->cells || col >= r->num_cells)
    return NULL;
  return &r->cells[col];
}

const char *table_vm_table_name(const TableViewModel *vm) {
  if (!vm)
    return NULL;
  return VM_T(vm)->table_name;
}

const TableSchema *table_vm_get_schema(const TableViewModel *vm) {
  if (!vm)
    return NULL;
  return VM_T(vm)->schema;
}

bool table_vm_valid(const TableViewModel *vm) {
  return vm && VM_T(vm)->data;
}

/* ============================================================================
 * Pagination
 * ============================================================================
 */

size_t table_vm_loaded_offset(const TableViewModel *vm) {
  return vm ? VM_T(vm)->loaded_offset : 0;
}

size_t table_vm_loaded_count(const TableViewModel *vm) {
  return vm ? VM_T(vm)->loaded_count : 0;
}

/* ============================================================================
 * Column Widths
 * ============================================================================
 */

void table_vm_recalc_column_widths(TableViewModel *vm) {
  if (!vm || !VM_T(vm)->data)
    return;

  size_t num_cols = VM_T(vm)->data->num_columns;
  if (num_cols == 0)
    return;

  /* Resize array if needed */
  if (!vm->col_widths || vm->num_col_widths != num_cols) {
    vm->col_widths = safe_reallocarray(vm->col_widths, num_cols, sizeof(int));
    vm->num_col_widths = num_cols;
  }

  /* Calculate width for each column */
  for (size_t col = 0; col < num_cols; col++) {
    int width = MIN_COL_WIDTH;

    /* Consider header width */
    if (VM_T(vm)->schema && col < VM_T(vm)->schema->num_columns &&
        VM_T(vm)->schema->columns[col].name) {
      int header_len = utf8_width(VM_T(vm)->schema->columns[col].name,
                                  strlen(VM_T(vm)->schema->columns[col].name));
      /* room for the header's ⇒ on a foreign key's first column */
      if (catalog_fk_first_column(VM_T(vm)->schema, VM_T(vm)->schema->columns[col].name))
        header_len += 2;
      if (header_len > width)
        width = header_len;
    }

    /* Consider data width (sample first TUI_PAGE_SIZE rows) - handle all types */
    size_t sample_count =
        VM_T(vm)->data->num_rows < TUI_PAGE_SIZE ? VM_T(vm)->data->num_rows : TUI_PAGE_SIZE;
    for (size_t row = 0; row < sample_count; row++) {
      const DbValue *val = table_vm_cell(vm, row, col);
      if (val) {
        char *str = db_value_to_string(val);
        if (str) {
          int cell_len = utf8_width(str, strlen(str));
          if (cell_len > width)
            width = cell_len;
          free(str);
        }
      }
    }

    /* Clamp to max width: content-based sizing up to 15 chars */
    if (width > DEFAULT_COL_WIDTH)
      width = DEFAULT_COL_WIDTH;

    vm->col_widths[col] = width;
  }

  vm_mark_dirty(&vm->base, TABLE_VM_CHANGE_COLUMN_WIDTHS);
}

void table_vm_update_column_widths(TableViewModel *vm) {
  if (!vm) {
    return;
  }
  Tab *tab = VM_T(vm);
  ResultSet *data = tab->data;
  if (!data || data->num_columns == 0) {
    return;
  }
  if (vm->widths_data == data && vm->widths_rows == data->num_rows &&
      vm->widths_offset == tab->loaded_offset &&
      vm->num_col_widths == data->num_columns) {
    return; /* Same window as last time */
  }

  if (vm->num_col_widths != data->num_columns || !vm->col_widths) {
    table_vm_recalc_column_widths(vm);
  } else {
    /* Grow-only: keep current widths, widen where new rows need more */
    int *old = safe_calloc(vm->num_col_widths, sizeof(int));
    memcpy(old, vm->col_widths, vm->num_col_widths * sizeof(int));
    table_vm_recalc_column_widths(vm);
    for (size_t i = 0; i < vm->num_col_widths; i++) {
      if (old[i] > vm->col_widths[i]) {
        vm->col_widths[i] = old[i];
      }
    }
    free(old);
  }
  vm->widths_data = data;
  vm->widths_rows = data->num_rows;
  vm->widths_offset = tab->loaded_offset;
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
  if (row) *row = VM_T(vm)->cursor_row;
  if (col) *col = VM_T(vm)->cursor_col;
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
      (VM_T(vm)->cursor_row != row || VM_T(vm)->cursor_col != col);

  VM_T(vm)->cursor_row = row;
  VM_T(vm)->cursor_col = col;

  if (changed) {
    vm_notify(&vm->base, VM_CHANGE_CURSOR);
  }
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
  if (row) *row = VM_T(vm)->scroll_row;
  if (col) *col = VM_T(vm)->scroll_col;
}

void table_vm_set_scroll(TableViewModel *vm, size_t row, size_t col) {
  if (!vm)
    return;

  bool changed =
      (VM_T(vm)->scroll_row != row || VM_T(vm)->scroll_col != col);

  VM_T(vm)->scroll_row = row;
  VM_T(vm)->scroll_col = col;

  if (changed) {
    vm_notify(&vm->base, VM_CHANGE_SCROLL);
  }
}

/* ============================================================================
 * Connection
 * ============================================================================
 */

DbConnection *table_vm_connection(const TableViewModel *vm) {
  Tab *tab = vm && vm->app ? app_current_tab(vm->app) : NULL;
  if (!tab)
    return NULL;

  Connection *conn = app_get_tab_connection(vm->app, tab);
  return conn ? conn->conn : NULL;
}
