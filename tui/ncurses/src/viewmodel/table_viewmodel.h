/*
 * Lace
 * TableViewModel - Table data display view model
 *
 * Provides table-specific functionality:
 * - Data binding (ResultSet, schema)
 * - Cursor and scroll position
 * - Multi-row selection
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
/* (1u << 4) was VM_CHANGE_EDIT: inline editing lives in edit.c */
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

/* TableViewModel change flag (extends the base VM_CHANGE_* flags) */
#define TABLE_VM_CHANGE_COLUMN_WIDTHS (1u << 8)

/* ============================================================================
 * TableViewModel
 * ============================================================================
 */

struct TableViewModel {
  /* Base viewmodel - MUST be first field for polymorphism */
  ViewModel base;

  /* The app the current tab is resolved from on every access (not owned).
   * Data, schema, pagination, cursor/scroll and sort live in that Tab; the
   * viewmodel keeps no pointer to it, since tab arrays are reallocated. */
  AppState *app;

  /* Column widths (calculated from data) */
  int *col_widths;
  size_t num_col_widths;
  /* Which loaded window the widths were measured on (see
   * table_vm_update_column_widths) */
  const void *widths_data;
  size_t widths_rows;
  size_t widths_offset;
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

/* Call when the current tab, or the table it shows, may have changed:
 * re-measures column widths for it. The viewmodel stores no tab pointer. */
void table_vm_bind(TableViewModel *vm, Tab *tab);

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

/* Name of result column col (the cursor's column), which need not be
 * schema column col: map between them by name */
const char *table_vm_result_column_name(const TableViewModel *vm, size_t col);

/* Get cell value */
const DbValue *table_vm_cell(const TableViewModel *vm, size_t row, size_t col);

/* Get table name */
const char *table_vm_table_name(const TableViewModel *vm);

/* Get full schema (for GUI backends that need all column metadata) */
const TableSchema *table_vm_get_schema(const TableViewModel *vm);

/* Check if viewmodel has valid data */
bool table_vm_valid(const TableViewModel *vm);

/* ============================================================================
 * Pagination
 * ============================================================================
 */

/* Get pagination state */
size_t table_vm_loaded_offset(const TableViewModel *vm);
size_t table_vm_loaded_count(const TableViewModel *vm);

/* ============================================================================
 * Selection
 * ============================================================================
 */

/* ============================================================================
 * Sorting
 * ============================================================================
 */

/* ============================================================================
 * Column Widths
 * ============================================================================
 */

/* Recalculate column widths from data */
void table_vm_recalc_column_widths(TableViewModel *vm);

/* Re-measure widths if the loaded rows changed since the last measurement.
 * Widths only grow (no jitter while scrolling); a column-count change
 * triggers a full recalculation. Cheap when nothing changed. */
void table_vm_update_column_widths(TableViewModel *vm);

/* ============================================================================
 * Cursor & Navigation
 * ============================================================================
 */

/* Get/set cursor position (stored in base.state) */
void table_vm_get_cursor(const TableViewModel *vm, size_t *row, size_t *col);
void table_vm_set_cursor(TableViewModel *vm, size_t row, size_t col);

/* ============================================================================
 * Scroll
 * ============================================================================
 */

/* Get/set scroll position (stored in base.state) */
void table_vm_get_scroll(const TableViewModel *vm, size_t *row, size_t *col);
void table_vm_set_scroll(TableViewModel *vm, size_t row, size_t col);

/* ============================================================================
 * Row Loading
 * ============================================================================
 */

/* ============================================================================
 * Actions
 * ============================================================================
 */

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

/* ============================================================================
 * ViewModel Operations (internal)
 * ============================================================================
 */

/* ============================================================================
 * Type Alias (for backward compatibility in tui.h)
 * ============================================================================
 */
typedef TableViewModel VmTable;

/* A grid column's width as the notcurses TUI measures it: its name (plus
 * room for a key's ⚷ and a foreign key's ⇒ in keys' schema) or its widest
 * value in rows [from, to), at most DEFAULT_COL_WIDTH */
int grid_measure_column(const ResultSet *data, size_t col, const TableSchema *keys, size_t from, size_t to);

#endif /* LACE_TABLE_VIEWMODEL_H */
