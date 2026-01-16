/*
 * Lace
 * ViewModel - Base abstraction for all view models
 *
 * Provides common infrastructure for cursor, scroll, focus, and change
 * notification. Concrete view models (Table, Sidebar, Filters, Query) extend
 * this base to create platform-independent UI components.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_VIEWMODEL_H
#define LACE_VIEWMODEL_H

#include <stdbool.h>
#include <stddef.h>

/* Forward declarations */
typedef struct ViewModel ViewModel;
typedef struct ViewModelOps ViewModelOps;
typedef struct UiEvent UiEvent;

/* ============================================================================
 * Change Flags - Bitmask indicating what changed (for partial UI refresh)
 * ============================================================================
 */

/* VMChangeFlags is an unsigned int to allow viewmodel-specific extensions.
 * Base viewmodel uses bits 0-7. Concrete types can define extended flags from
 * bit 8+. */
typedef unsigned int VMChangeFlags;

/* Base viewmodel change flags */
#define VM_CHANGE_NONE      0
#define VM_CHANGE_CURSOR    (1u << 0) /* Cursor position changed */
#define VM_CHANGE_SCROLL    (1u << 1) /* Scroll position changed */
#define VM_CHANGE_FOCUS     (1u << 2) /* Focus state changed */
#define VM_CHANGE_SELECTION (1u << 3) /* Selection changed */
#define VM_CHANGE_DATA      (1u << 4) /* Underlying data changed */
#define VM_CHANGE_VISIBLE   (1u << 5) /* Visibility changed */
#define VM_CHANGE_EDITING   (1u << 6) /* Edit mode changed */
#define VM_CHANGE_ALL       0xFFFFFFFFu /* Everything changed */

/* ============================================================================
 * ViewModel State - Common state for all view models
 * ============================================================================
 * ViewModels own their cursor/scroll state. This is the single source of
 * truth.
 */

typedef struct ViewModelState {
  /* Cursor position within the viewmodel's content */
  size_t cursor_row;
  size_t cursor_col;

  /* Scroll offset (first visible row/col) */
  size_t scroll_row;
  size_t scroll_col;

  /* Focus and visibility */
  bool focused;
  bool visible;

  /* Dirty flag for optimized rendering */
  VMChangeFlags dirty;
} ViewModelState;

/* ============================================================================
 * ViewModel Callbacks - UI bindings for change notifications
 * ============================================================================
 */

typedef struct ViewModelCallbacks {
  /* Called when viewmodel state changes. Flags indicate what changed. */
  void (*on_change)(ViewModel *vm, VMChangeFlags flags, void *ctx);

  /* Called when viewmodel gains focus */
  void (*on_focus)(ViewModel *vm, void *ctx);

  /* Called when viewmodel loses focus */
  void (*on_blur)(ViewModel *vm, void *ctx);

  /* User context passed to all callbacks */
  void *context;
} ViewModelCallbacks;

/* ============================================================================
 * ViewModel Operations - Virtual table for polymorphism
 * ============================================================================
 * Each concrete viewmodel type provides an ops table implementing these.
 */

struct ViewModelOps {
  /* Type name for debugging */
  const char *type_name;

  /* Handle input event. Returns true if event was consumed. */
  bool (*handle_event)(ViewModel *vm, const UiEvent *event);

  /* Get content dimensions */
  size_t (*get_row_count)(const ViewModel *vm);
  size_t (*get_col_count)(const ViewModel *vm);

  /* Optional: Called when focus changes */
  void (*on_focus_in)(ViewModel *vm);
  void (*on_focus_out)(ViewModel *vm);

  /* Optional: Called to validate cursor position after data change */
  void (*validate_cursor)(ViewModel *vm);

  /* Optional: Cleanup viewmodel-specific data */
  void (*destroy)(ViewModel *vm);
};

/* ============================================================================
 * ViewModel - Base structure for all view models
 * ============================================================================
 * Concrete view models embed this as their first field for polymorphism.
 *
 * Example:
 *   struct TableViewModel {
 *     ViewModel base;        // Must be first field
 *     ResultSet *data;       // ViewModel-specific data
 *     ...
 *   };
 *
 * Usage:
 *   TableViewModel *tvm = ...;
 *   vm_set_cursor(&tvm->base, row, col);  // Use base functions
 */

struct ViewModel {
  /* Virtual table (type-specific operations) */
  const ViewModelOps *ops;

  /* Common state (cursor, scroll, focus) */
  ViewModelState state;

  /* Change notification callbacks */
  ViewModelCallbacks callbacks;

  /* Opaque pointer for viewmodel-specific use */
  void *user_data;
};

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================
 */

/* Initialize viewmodel with operations table. Clears state to defaults. */
void vm_init(ViewModel *vm, const ViewModelOps *ops);

/* Cleanup viewmodel. Calls ops->destroy if set. Does NOT free the viewmodel
 * itself. */
void vm_cleanup(ViewModel *vm);

/* Set callbacks for change notifications */
void vm_set_callbacks(ViewModel *vm, const ViewModelCallbacks *callbacks);

/* ============================================================================
 * Cursor Functions
 * ============================================================================
 */

/* Get current cursor position */
void vm_get_cursor(const ViewModel *vm, size_t *row, size_t *col);

/* Set cursor position (clamped to valid range). Notifies on change. */
void vm_set_cursor(ViewModel *vm, size_t row, size_t col);

/* Move cursor by delta (clamped to valid range). Notifies on change. */
void vm_move_cursor(ViewModel *vm, int row_delta, int col_delta);

/* Move to first/last row */
void vm_goto_first_row(ViewModel *vm);
void vm_goto_last_row(ViewModel *vm);

/* Move to first/last column */
void vm_goto_first_col(ViewModel *vm);
void vm_goto_last_col(ViewModel *vm);

/* ============================================================================
 * Scroll Functions
 * ============================================================================
 */

/* Get current scroll position */
void vm_get_scroll(const ViewModel *vm, size_t *row, size_t *col);

/* Set scroll position. Notifies on change. */
void vm_set_scroll(ViewModel *vm, size_t row, size_t col);

/* Adjust scroll to keep cursor visible within viewport.
 * visible_rows/cols: size of the visible viewport */
void vm_scroll_to_cursor(ViewModel *vm, size_t visible_rows, size_t visible_cols);

/* Page movement (moves both cursor and scroll) */
void vm_page_up(ViewModel *vm, size_t page_size);
void vm_page_down(ViewModel *vm, size_t page_size);

/* ============================================================================
 * Focus Functions
 * ============================================================================
 */

/* Check if viewmodel is focused */
bool vm_is_focused(const ViewModel *vm);

/* Set focus state. Calls on_focus_in/out callbacks. Notifies on change. */
void vm_set_focus(ViewModel *vm, bool focused);

/* ============================================================================
 * Visibility Functions
 * ============================================================================
 */

/* Check if viewmodel is visible */
bool vm_is_visible(const ViewModel *vm);

/* Set visibility. Notifies on change. */
void vm_set_visible(ViewModel *vm, bool visible);

/* ============================================================================
 * Notification Functions
 * ============================================================================
 */

/* Notify listeners of changes. Called automatically by state-changing funcs. */
void vm_notify(ViewModel *vm, VMChangeFlags flags);

/* Mark viewmodel dirty (needs redraw) without triggering callback */
void vm_mark_dirty(ViewModel *vm, VMChangeFlags flags);

/* Clear dirty flags after redraw */
void vm_clear_dirty(ViewModel *vm);

/* Check if viewmodel is dirty */
bool vm_is_dirty(const ViewModel *vm);

/* ============================================================================
 * Input Handling
 * ============================================================================
 */

/* Dispatch event to viewmodel. Returns true if event was consumed.
 * Delegates to ops->handle_event if set. */
bool vm_handle_event(ViewModel *vm, const UiEvent *event);

/* ============================================================================
 * Dimension Queries
 * ============================================================================
 */

/* Get row count. Delegates to ops->get_row_count. Returns 0 if not set. */
size_t vm_row_count(const ViewModel *vm);

/* Get column count. Delegates to ops->get_col_count. Returns 0 if not set. */
size_t vm_col_count(const ViewModel *vm);

/* ============================================================================
 * Utility Functions
 * ============================================================================
 */

/* Validate cursor position (clamp to valid range).
 * Calls ops->validate_cursor if set, otherwise uses get_row/col_count. */
void vm_validate_cursor(ViewModel *vm);

/* Get viewmodel type name for debugging */
const char *vm_type_name(const ViewModel *vm);

/* Check if viewmodel is valid (has ops table set) */
bool vm_valid(const ViewModel *vm);

/* ============================================================================
 * Backward Compatibility Aliases
 * ============================================================================
 * These aliases allow gradual migration from Widget to ViewModel naming.
 * They will be removed in a future version.
 */

/* Type aliases */
typedef ViewModel Widget;
typedef ViewModelState WidgetState;
typedef ViewModelCallbacks WidgetCallbacks;
typedef ViewModelOps WidgetOps;
typedef VMChangeFlags WidgetChangeFlags;

/* Flag aliases */
#define WIDGET_CHANGE_NONE      VM_CHANGE_NONE
#define WIDGET_CHANGE_CURSOR    VM_CHANGE_CURSOR
#define WIDGET_CHANGE_SCROLL    VM_CHANGE_SCROLL
#define WIDGET_CHANGE_FOCUS     VM_CHANGE_FOCUS
#define WIDGET_CHANGE_SELECTION VM_CHANGE_SELECTION
#define WIDGET_CHANGE_DATA      VM_CHANGE_DATA
#define WIDGET_CHANGE_VISIBLE   VM_CHANGE_VISIBLE
#define WIDGET_CHANGE_EDITING   VM_CHANGE_EDITING
#define WIDGET_CHANGE_ALL       VM_CHANGE_ALL

/* Function aliases */
#define widget_init             vm_init
#define widget_cleanup          vm_cleanup
#define widget_set_callbacks    vm_set_callbacks
#define widget_get_cursor       vm_get_cursor
#define widget_set_cursor       vm_set_cursor
#define widget_move_cursor      vm_move_cursor
#define widget_goto_first_row   vm_goto_first_row
#define widget_goto_last_row    vm_goto_last_row
#define widget_goto_first_col   vm_goto_first_col
#define widget_goto_last_col    vm_goto_last_col
#define widget_get_scroll       vm_get_scroll
#define widget_set_scroll       vm_set_scroll
#define widget_scroll_to_cursor vm_scroll_to_cursor
#define widget_page_up          vm_page_up
#define widget_page_down        vm_page_down
#define widget_is_focused       vm_is_focused
#define widget_set_focus        vm_set_focus
#define widget_is_visible       vm_is_visible
#define widget_set_visible      vm_set_visible
#define widget_notify           vm_notify
#define widget_mark_dirty       vm_mark_dirty
#define widget_clear_dirty      vm_clear_dirty
#define widget_is_dirty         vm_is_dirty
#define widget_handle_event     vm_handle_event
#define widget_row_count        vm_row_count
#define widget_col_count        vm_col_count
#define widget_validate_cursor  vm_validate_cursor
#define widget_type_name        vm_type_name
#define widget_valid            vm_valid

#endif /* LACE_VIEWMODEL_H */
