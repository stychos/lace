/*
 * Lace
 * TableViewModel - Table data display view model
 *
 * Provides table-specific functionality:
 * - Data binding (ResultSet, schema)
 * - Cursor and scroll position
 * - Multi-row selection
 * - Inline cell editing
 * - Multi-column sorting
 * - Pagination state
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TABLE_VIEWMODEL_H
#define LACE_TABLE_VIEWMODEL_H

#include "../core/app_state.h"
#include "../db_compat.h"
#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * Base ViewModel types (inlined from viewmodel.h)
 * ============================================================================
 */

/* Change flags for notifying views of state changes */
#define VM_CHANGE_NONE      0
#define VM_CHANGE_CURSOR    (1u << 0)
#define VM_CHANGE_SCROLL    (1u << 1)
#define VM_CHANGE_SELECTION (1u << 2)
#define VM_CHANGE_DATA      (1u << 3)
#define VM_CHANGE_EDIT      (1u << 4)
#define VM_CHANGE_EDITING   (1u << 4)  /* Alias for VM_CHANGE_EDIT */
#define VM_CHANGE_VISIBLE   (1u << 5)
#define VM_CHANGE_FOCUS     (1u << 6)
#define VM_CHANGE_ALL       0xFFu

/* Base state for any viewmodel */
typedef struct ViewModelState {
  size_t cursor_row;
  size_t cursor_col;
  size_t scroll_row;
  size_t scroll_col;
  bool visible;
  bool focused;
} ViewModelState;

/* Forward declarations */
typedef struct ViewModel ViewModel;
struct UiEvent;

/* Operations for a viewmodel */
typedef struct ViewModelOps {
  /* Required operations */
  void (*destroy)(ViewModel *vm);
  void (*reset)(ViewModel *vm);
  unsigned int (*get_change_flags)(const ViewModel *vm);
  void (*clear_change_flags)(ViewModel *vm);
  /* Optional operations */
  const char *type_name;
  bool (*handle_event)(ViewModel *vm, const struct UiEvent *event);
  size_t (*get_row_count)(const ViewModel *vm);
  size_t (*get_col_count)(const ViewModel *vm);
  void (*on_focus_in)(ViewModel *vm);
  void (*on_focus_out)(ViewModel *vm);
  void (*validate_cursor)(ViewModel *vm);
} ViewModelOps;

/* Base viewmodel structure */
struct ViewModel {
  ViewModelState state;
  const ViewModelOps *ops;
  unsigned int change_flags;
};

/* Helper functions for viewmodels */
static inline void vm_init(ViewModel *vm, const ViewModelOps *ops) {
  vm->state = (ViewModelState){0};
  vm->ops = ops;
  vm->change_flags = 0;
}

static inline void vm_cleanup(ViewModel *vm) {
  (void)vm; /* No resources to free in base ViewModel */
}

static inline void vm_mark_dirty(ViewModel *vm, unsigned int flags) {
  if (vm) vm->change_flags |= flags;
}

static inline void vm_notify(ViewModel *vm, unsigned int flags) {
  /* Same as mark_dirty - caller handles notification */
  if (vm) vm->change_flags |= flags;
}

/* Forward declarations */
typedef struct TableViewModel TableViewModel;

/* ============================================================================
 * TableViewModel-specific change flags (extend base VM_CHANGE_* flags)
 * ============================================================================
 */

#define TABLE_VM_CHANGE_COLUMN_WIDTHS (1u << 8)  /* Column widths changed */
#define TABLE_VM_CHANGE_LOADING       (1u << 9)  /* Loading state changed */
#define TABLE_VM_CHANGE_SORT          (1u << 10) /* Sort order changed */
#define TABLE_VM_CHANGE_FILTER        (1u << 11) /* Filter changed */
#define TABLE_VM_CHANGE_ERROR         (1u << 12) /* Error occurred */

/* ============================================================================
 * Selection State
 * ============================================================================
 */

typedef struct TableSelection {
  size_t *rows;    /* Array of selected row indices */
  size_t count;    /* Number of selected rows */
  size_t capacity; /* Allocated capacity */
  size_t anchor;   /* Anchor row for shift-select */
  bool anchor_set; /* Whether anchor is valid */
} TableSelection;

/* ============================================================================
 * Edit State
 * ============================================================================
 */

typedef struct TableEditState {
  bool active;       /* Currently editing */
  size_t row;        /* Row being edited */
  size_t col;        /* Column being edited */
  char *buffer;      /* Edit buffer */
  size_t buffer_len; /* Buffer content length */
  size_t buffer_cap; /* Buffer capacity */
  size_t cursor_pos; /* Cursor position in edit buffer */
  char *original;    /* Original value (for cancel) */
} TableEditState;

/* ============================================================================
 * TableViewModel Callbacks
 * ============================================================================
 */

typedef struct TableViewModelCallbacks {
  /* Called when async load completes */
  void (*on_load_complete)(TableViewModel *vm, bool success, void *ctx);

  /* Called when edit completes (success/error) */
  void (*on_edit_complete)(TableViewModel *vm, bool success, const char *error,
                           void *ctx);

  /* Called when table selection changes */
  void (*on_table_select)(TableViewModel *vm, const char *table_name, void *ctx);

  /* User context */
  void *context;
} TableViewModelCallbacks;

/* ============================================================================
 * TableViewModel
 * ============================================================================
 */

struct TableViewModel {
  /* Base viewmodel - MUST be first field for polymorphism */
  ViewModel base;

  /* Model bindings (references, not owned) */
  Tab *tab;            /* Underlying tab (owns data, schema) */
  AppState *app;       /* App state for connection access */
  ResultSet *data;     /* Current data (convenience pointer to tab->data) */
  TableSchema *schema; /* Schema (convenience pointer to tab->schema) */

  /* Table-specific callbacks */
  TableViewModelCallbacks table_callbacks;

  /* Pagination state (owned by viewmodel, Tab is updated) */
  size_t loaded_offset;       /* First loaded row index (global) */
  size_t loaded_count;        /* Number of loaded rows */
  size_t total_rows;          /* Total rows in table (may be approximate) */
  bool row_count_approximate; /* Is total_rows approximate? */
  bool is_loading;            /* Currently loading data */

  /* Column widths (calculated from data) */
  int *col_widths;
  size_t num_col_widths;

  /* Selection state */
  TableSelection selection;

  /* Edit state */
  TableEditState edit;

  /* Sort state (mirrors Tab for convenience) */
  SortEntry sort_entries[MAX_SORT_COLUMNS];
  size_t num_sort_entries;

  /* UI hints for visible range (for lazy loading) */
  size_t visible_first_row;
  size_t visible_row_count;
};

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create a table viewmodel bound to a tab.
 * Returns NULL on allocation failure.
 * The viewmodel does NOT own the tab - caller manages tab lifetime. */
TableViewModel *table_vm_create(AppState *app, Tab *tab);

/* Destroy table viewmodel. Does NOT free the bound tab. */
void table_vm_destroy(TableViewModel *vm);

/* Rebind viewmodel to a different tab (e.g., on tab switch).
 * Clears selection and edit state. Preserves scroll if same table. */
void table_vm_bind(TableViewModel *vm, Tab *tab);

/* Set table-specific callbacks */
void table_vm_set_callbacks(TableViewModel *vm,
                            const TableViewModelCallbacks *callbacks);

/* ============================================================================
 * Data Access
 * ============================================================================
 */

/* Get loaded row count (rows in memory) */
size_t table_vm_row_count(const TableViewModel *vm);

/* Get column count */
size_t table_vm_col_count(const TableViewModel *vm);

/* Get total row count (including unloaded) */
size_t table_vm_total_rows(const TableViewModel *vm);

/* Get column info */
const char *table_vm_column_name(const TableViewModel *vm, size_t col);
DbValueType table_vm_column_type(const TableViewModel *vm, size_t col);
bool table_vm_column_nullable(const TableViewModel *vm, size_t col);
bool table_vm_column_is_pk(const TableViewModel *vm, size_t col);
int table_vm_column_width(const TableViewModel *vm, size_t col);

/* Get cell value */
const DbValue *table_vm_cell(const TableViewModel *vm, size_t row, size_t col);
const char *table_vm_cell_text(const TableViewModel *vm, size_t row,
                               size_t col);
bool table_vm_cell_is_null(const TableViewModel *vm, size_t row, size_t col);

/* Get table name */
const char *table_vm_table_name(const TableViewModel *vm);

/* Get full schema (for GUI backends that need all column metadata) */
const TableSchema *table_vm_get_schema(const TableViewModel *vm);

/* Get column foreign key info (returns "table.column" or NULL) */
const char *table_vm_column_fk(const TableViewModel *vm, size_t col);

/* Get column auto-increment flag */
bool table_vm_column_auto_increment(const TableViewModel *vm, size_t col);

/* Get column default value (or NULL) */
const char *table_vm_column_default(const TableViewModel *vm, size_t col);

/* Get primary key column indices (for row identification).
 * Returns number of PK columns. Fills pk_cols array if not NULL. */
size_t table_vm_pk_columns(const TableViewModel *vm, size_t *pk_cols,
                           size_t max_cols);

/* Check if viewmodel has valid data */
bool table_vm_valid(const TableViewModel *vm);

/* ============================================================================
 * Pagination
 * ============================================================================
 */

/* Get pagination state */
size_t table_vm_loaded_offset(const TableViewModel *vm);
size_t table_vm_loaded_count(const TableViewModel *vm);
bool table_vm_is_loading(const TableViewModel *vm);

/* Set visible range (for preloading optimization) */
void table_vm_set_visible_range(TableViewModel *vm, size_t first, size_t count);

/* Update pagination state after data load.
 * Called by pagination layer after merging results. */
void table_vm_update_pagination(TableViewModel *vm, size_t offset, size_t count,
                                size_t total);

/* ============================================================================
 * Selection
 * ============================================================================
 */

/* Single row selection */
void table_vm_select_row(TableViewModel *vm, size_t row);
void table_vm_deselect_row(TableViewModel *vm, size_t row);
void table_vm_toggle_row_selection(TableViewModel *vm, size_t row);
bool table_vm_row_selected(const TableViewModel *vm, size_t row);

/* Range selection (for shift+click) */
void table_vm_select_range(TableViewModel *vm, size_t from, size_t to);
void table_vm_extend_selection(TableViewModel *vm, size_t to_row);

/* Bulk selection */
void table_vm_select_all(TableViewModel *vm);
void table_vm_clear_selection(TableViewModel *vm);

/* Get selected rows */
size_t table_vm_selection_count(const TableViewModel *vm);
const size_t *table_vm_selected_rows(const TableViewModel *vm);

/* ============================================================================
 * Editing
 * ============================================================================
 */

/* Start editing */
bool table_vm_start_edit(TableViewModel *vm, size_t row, size_t col);
bool table_vm_start_edit_at_cursor(TableViewModel *vm);

/* Edit operations */
void table_vm_edit_insert_char(TableViewModel *vm, char ch);
void table_vm_edit_insert_text(TableViewModel *vm, const char *text);
void table_vm_edit_delete_char(TableViewModel *vm);
void table_vm_edit_backspace(TableViewModel *vm);
void table_vm_edit_clear(TableViewModel *vm);
void table_vm_edit_set_cursor(TableViewModel *vm, size_t pos);
void table_vm_edit_move_cursor(TableViewModel *vm, int delta);
void table_vm_edit_home(TableViewModel *vm);
void table_vm_edit_end(TableViewModel *vm);

/* Get edit state */
bool table_vm_is_editing(const TableViewModel *vm);
const char *table_vm_edit_buffer(const TableViewModel *vm);
size_t table_vm_edit_cursor(const TableViewModel *vm);
void table_vm_get_edit_cell(const TableViewModel *vm, size_t *row, size_t *col);

/* Commit or cancel edit */
bool table_vm_commit_edit(TableViewModel *vm);
void table_vm_cancel_edit(TableViewModel *vm);

/* ============================================================================
 * Sorting
 * ============================================================================
 */

/* Sort by column */
void table_vm_sort_by(TableViewModel *vm, size_t col, bool descending);
void table_vm_toggle_sort(TableViewModel *vm, size_t col);
void table_vm_add_sort(TableViewModel *vm, size_t col, bool descending);
void table_vm_clear_sort(TableViewModel *vm);

/* Get sort state */
bool table_vm_is_sorted(const TableViewModel *vm);
size_t table_vm_sort_column_count(const TableViewModel *vm);
const SortEntry *table_vm_sort_entries(const TableViewModel *vm);

/* ============================================================================
 * Column Widths
 * ============================================================================
 */

/* Recalculate column widths from data */
void table_vm_recalc_column_widths(TableViewModel *vm);

/* Set column width manually */
void table_vm_set_column_width(TableViewModel *vm, size_t col, int width);

/* Get column widths array */
const int *table_vm_get_column_widths(const TableViewModel *vm,
                                      size_t *num_cols);

/* ============================================================================
 * Cursor & Navigation
 * ============================================================================
 */

/* Get/set cursor position (stored in base.state) */
void table_vm_get_cursor(const TableViewModel *vm, size_t *row, size_t *col);
void table_vm_set_cursor(TableViewModel *vm, size_t row, size_t col);

/* Move cursor (with clamping) */
void table_vm_move_cursor(TableViewModel *vm, int row_delta, int col_delta);

/* Jump to positions */
void table_vm_goto_first_row(TableViewModel *vm);
void table_vm_goto_last_row(TableViewModel *vm);
void table_vm_goto_first_col(TableViewModel *vm);
void table_vm_goto_last_col(TableViewModel *vm);

/* Page movement */
void table_vm_page_up(TableViewModel *vm, size_t page_size);
void table_vm_page_down(TableViewModel *vm, size_t page_size);

/* ============================================================================
 * Scroll
 * ============================================================================
 */

/* Get/set scroll position (stored in base.state) */
void table_vm_get_scroll(const TableViewModel *vm, size_t *row, size_t *col);
void table_vm_set_scroll(TableViewModel *vm, size_t row, size_t col);

/* Ensure cursor is visible (adjusts scroll) */
void table_vm_ensure_cursor_visible(TableViewModel *vm, size_t visible_rows,
                                    size_t visible_cols);

/* ============================================================================
 * Row Loading
 * ============================================================================
 */

/* Check if a specific row is loaded */
bool table_vm_row_loaded(const TableViewModel *vm, size_t row);

/* Request that a specific row be loaded (triggers loading if needed) */
void table_vm_ensure_row_loaded(TableViewModel *vm, size_t row);

/* ============================================================================
 * Actions
 * ============================================================================
 */

/* Delete selected rows */
bool table_vm_delete_selected(TableViewModel *vm, char **error);

/* Refresh/reload data */
void table_vm_refresh(TableViewModel *vm);

/* ============================================================================
 * Connection
 * ============================================================================
 */

/* Get database connection for this table */
DbConnection *table_vm_connection(const TableViewModel *vm);

/* ============================================================================
 * Clipboard
 * ============================================================================
 */

/* Copy cell/selection to clipboard (returns allocated string) */
char *table_vm_copy_cell(const TableViewModel *vm);
char *table_vm_copy_selection(const TableViewModel *vm, bool include_headers);

/* ============================================================================
 * Sync with Tab
 * ============================================================================
 * These functions sync viewmodel state with the underlying Tab.
 * Used during migration - will be removed when Tab no longer owns cursor.
 */

/* Sync viewmodel state FROM tab (after tab switch or data load) */
void table_vm_sync_from_tab(TableViewModel *vm);

/* Sync viewmodel state TO tab (before tab switch or save) */
void table_vm_sync_to_tab(TableViewModel *vm);

/* ============================================================================
 * ViewModel Operations (internal)
 * ============================================================================
 */

/* Get ViewModelOps for TableViewModel */
const ViewModelOps *table_vm_ops(void);

/* ============================================================================
 * Type Alias (for backward compatibility in tui.h)
 * ============================================================================
 */
typedef TableViewModel VmTable;

#endif /* LACE_TABLE_VIEWMODEL_H */
