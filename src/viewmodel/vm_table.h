/*
 * Lace
 * Table ViewModel - Platform-independent table data model
 *
 * Provides a clean interface for both TUI and GUI to access table data,
 * cursor state, selection, pagination, and editing operations.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_VM_TABLE_H
#define LACE_VM_TABLE_H

#include "../core/app_state.h"
#include "../db/db_types.h"
#include <stdbool.h>
#include <stddef.h>

/* Forward declarations */
typedef struct VmTable VmTable;

/* ============================================================================
 * Change Flags - What changed (for partial UI refresh)
 * ============================================================================
 */

typedef enum {
  VM_TABLE_CHANGE_NONE = 0,
  VM_TABLE_CHANGE_DATA = (1 << 0),      /* Row data changed */
  VM_TABLE_CHANGE_CURSOR = (1 << 1),    /* Cursor position changed */
  VM_TABLE_CHANGE_SELECTION = (1 << 2), /* Selection changed */
  VM_TABLE_CHANGE_SCROLL = (1 << 3),    /* Scroll position changed */
  VM_TABLE_CHANGE_COLUMNS = (1 << 4),   /* Column widths/order changed */
  VM_TABLE_CHANGE_LOADING = (1 << 5),   /* Loading state changed */
  VM_TABLE_CHANGE_EDITING = (1 << 6),   /* Edit mode changed */
  VM_TABLE_CHANGE_ERROR = (1 << 7),     /* Error occurred */
  VM_TABLE_CHANGE_ALL = 0xFF
} VmTableChangeFlags;

/* ============================================================================
 * Callbacks - UI bindings for change notifications
 * ============================================================================
 */

typedef struct {
  /* Called when table state changes */
  void (*on_change)(VmTable *vm, VmTableChangeFlags changes, void *ctx);

  /* Called when async operation completes */
  void (*on_load_complete)(VmTable *vm, bool success, void *ctx);

  /* Called when edit completes */
  void (*on_edit_complete)(VmTable *vm, bool success, const char *error,
                           void *ctx);

  /* User context passed to all callbacks */
  void *context;
} VmTableCallbacks;

/* ============================================================================
 * Selection - Multi-row selection support
 * ============================================================================
 */

typedef struct {
  size_t *rows;    /* Array of selected row indices */
  size_t count;    /* Number of selected rows */
  size_t capacity; /* Allocated capacity */
  size_t anchor;   /* Anchor row for shift-select */
  bool anchor_set; /* Is anchor valid */
} VmSelection;

/* ============================================================================
 * Edit State
 * ============================================================================
 */

typedef struct {
  bool active;       /* Currently editing */
  size_t row;        /* Row being edited */
  size_t col;        /* Column being edited */
  char *buffer;      /* Edit buffer */
  size_t buffer_len; /* Buffer content length */
  size_t buffer_cap; /* Buffer capacity */
  size_t cursor_pos; /* Cursor position in buffer */
  char *original;    /* Original value (for cancel) */
} VmEditState;

/* ============================================================================
 * VmTable - Table ViewModel
 * ============================================================================
 */

struct VmTable {
  /* Source data (references, not owned) */
  Tab *tab;      /* Underlying tab (contains ResultSet, schema) */
  AppState *app; /* App state for connection access */

  /* Callbacks */
  VmTableCallbacks callbacks;

  /* Selection state */
  VmSelection selection;

  /* Edit state */
  VmEditState edit;

  /* Computed column widths */
  int *col_widths;
  size_t num_col_widths;

  /* Sort state */
  size_t sort_column;
  bool sort_descending;
  bool sort_active;

  /* UI hints for native controls */
  size_t visible_first_row; /* First visible row (for lazy loading) */
  size_t visible_row_count; /* Number of visible rows */
};

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create a table viewmodel bound to a tab */
VmTable *vm_table_create(AppState *app, Tab *tab,
                         const VmTableCallbacks *callbacks);

/* Destroy viewmodel (does NOT free tab) */
void vm_table_destroy(VmTable *vm);

/* Rebind to a different tab (e.g., on tab switch) */
void vm_table_bind(VmTable *vm, Tab *tab);

/* ============================================================================
 * Data Access (for native table controls)
 * ============================================================================
 */

/* Row/column counts */
size_t vm_table_row_count(const VmTable *vm);
size_t vm_table_col_count(const VmTable *vm);
size_t vm_table_total_rows(const VmTable *vm); /* Total including unloaded */

/* Column info */
const char *vm_table_column_name(const VmTable *vm, size_t col);
DbValueType vm_table_column_type(const VmTable *vm, size_t col);
bool vm_table_column_nullable(const VmTable *vm, size_t col);
bool vm_table_column_is_primary_key(const VmTable *vm, size_t col);
int vm_table_column_width(const VmTable *vm, size_t col);

/* Cell access */
const DbValue *vm_table_cell(const VmTable *vm, size_t row, size_t col);
const char *vm_table_cell_text(const VmTable *vm, size_t row, size_t col);
bool vm_table_cell_is_null(const VmTable *vm, size_t row, size_t col);

/* Check if row is loaded (for lazy loading) */
bool vm_table_row_loaded(const VmTable *vm, size_t row);

/* ============================================================================
 * Cursor & Navigation
 * ============================================================================
 */

/* Get/set cursor position */
void vm_table_get_cursor(const VmTable *vm, size_t *row, size_t *col);
void vm_table_set_cursor(VmTable *vm, size_t row, size_t col);

/* Move cursor (clamps to bounds) */
void vm_table_move_cursor(VmTable *vm, int row_delta, int col_delta);

/* Jump to position */
void vm_table_goto_first_row(VmTable *vm);
void vm_table_goto_last_row(VmTable *vm);
void vm_table_goto_first_col(VmTable *vm);
void vm_table_goto_last_col(VmTable *vm);

/* Page movement */
void vm_table_page_up(VmTable *vm, size_t page_size);
void vm_table_page_down(VmTable *vm, size_t page_size);

/* ============================================================================
 * Scroll
 * ============================================================================
 */

/* Get/set scroll position */
void vm_table_get_scroll(const VmTable *vm, size_t *row, size_t *col);
void vm_table_set_scroll(VmTable *vm, size_t row, size_t col);

/* Ensure cursor is visible (adjusts scroll) */
void vm_table_ensure_cursor_visible(VmTable *vm, size_t visible_rows,
                                    size_t visible_cols);

/* ============================================================================
 * Selection (for multi-row operations)
 * ============================================================================
 */

/* Single row selection */
void vm_table_select_row(VmTable *vm, size_t row);
void vm_table_deselect_row(VmTable *vm, size_t row);
void vm_table_toggle_row_selection(VmTable *vm, size_t row);
bool vm_table_row_selected(const VmTable *vm, size_t row);

/* Range selection (shift+click) */
void vm_table_select_range(VmTable *vm, size_t from, size_t to);
void vm_table_extend_selection(VmTable *vm, size_t to_row);

/* Bulk selection */
void vm_table_select_all(VmTable *vm);
void vm_table_clear_selection(VmTable *vm);

/* Get selected rows */
size_t vm_table_selection_count(const VmTable *vm);
const size_t *vm_table_selected_rows(const VmTable *vm);

/* ============================================================================
 * Editing
 * ============================================================================
 */

/* Start editing a cell */
bool vm_table_start_edit(VmTable *vm, size_t row, size_t col);
bool vm_table_start_edit_at_cursor(VmTable *vm);

/* Edit operations */
void vm_table_edit_insert_char(VmTable *vm, char ch);
void vm_table_edit_insert_text(VmTable *vm, const char *text);
void vm_table_edit_delete_char(VmTable *vm);
void vm_table_edit_backspace(VmTable *vm);
void vm_table_edit_clear(VmTable *vm);
void vm_table_edit_set_cursor(VmTable *vm, size_t pos);
void vm_table_edit_move_cursor(VmTable *vm, int delta);
void vm_table_edit_home(VmTable *vm);
void vm_table_edit_end(VmTable *vm);

/* Get edit state */
bool vm_table_is_editing(const VmTable *vm);
const char *vm_table_edit_buffer(const VmTable *vm);
size_t vm_table_edit_cursor(const VmTable *vm);

/* Commit or cancel edit */
bool vm_table_commit_edit(VmTable *vm);
void vm_table_cancel_edit(VmTable *vm);

/* ============================================================================
 * Sorting
 * ============================================================================
 */

/* Sort by column (triggers data reload) */
void vm_table_sort_by(VmTable *vm, size_t col, bool descending);
void vm_table_toggle_sort(VmTable *vm, size_t col);
void vm_table_clear_sort(VmTable *vm);

/* Get sort state */
bool vm_table_is_sorted(const VmTable *vm);
size_t vm_table_sort_column(const VmTable *vm);
bool vm_table_sort_descending(const VmTable *vm);

/* ============================================================================
 * Pagination (Lazy Loading)
 * ============================================================================
 */

/* Tell viewmodel which rows are visible (for preloading) */
void vm_table_set_visible_range(VmTable *vm, size_t first, size_t count);

/* Ensure a specific row is loaded */
void vm_table_ensure_row_loaded(VmTable *vm, size_t row);

/* Get pagination state */
bool vm_table_is_loading(const VmTable *vm);
size_t vm_table_loaded_offset(const VmTable *vm);
size_t vm_table_loaded_count(const VmTable *vm);

/* ============================================================================
 * Actions
 * ============================================================================
 */

/* Delete selected rows */
bool vm_table_delete_selected(VmTable *vm, char **error);

/* Refresh data */
void vm_table_refresh(VmTable *vm);

/* Copy cell/selection to clipboard (returns allocated string) */
char *vm_table_copy_cell(const VmTable *vm);
char *vm_table_copy_selection(const VmTable *vm, bool include_headers);

/* ============================================================================
 * Column Widths
 * ============================================================================
 */

/* Recalculate column widths from data */
void vm_table_recalc_column_widths(VmTable *vm);

/* Set column width manually */
void vm_table_set_column_width(VmTable *vm, size_t col, int width);

/* ============================================================================
 * Utility
 * ============================================================================
 */

/* Get underlying connection */
DbConnection *vm_table_connection(const VmTable *vm);

/* Get table name */
const char *vm_table_name(const VmTable *vm);

/* Get schema */
const TableSchema *vm_table_schema(const VmTable *vm);

/* Check if viewmodel is valid (has bound tab with data) */
bool vm_table_valid(const VmTable *vm);

#endif /* LACE_VM_TABLE_H */
