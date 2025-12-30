/*
 * Lace
 * Sidebar ViewModel - Platform-independent table list model
 *
 * Provides a clean interface for both TUI and GUI to access the table list,
 * filtering, and navigation.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_VM_SIDEBAR_H
#define LACE_VM_SIDEBAR_H

#include "../core/app_state.h"
#include <stdbool.h>
#include <stddef.h>

/* Forward declarations */
typedef struct VmSidebar VmSidebar;

/* ============================================================================
 * Change Flags
 * ============================================================================
 */

typedef enum {
  VM_SIDEBAR_CHANGE_NONE = 0,
  VM_SIDEBAR_CHANGE_TABLES = (1 << 0),    /* Table list changed */
  VM_SIDEBAR_CHANGE_SELECTION = (1 << 1), /* Selection changed */
  VM_SIDEBAR_CHANGE_FILTER = (1 << 2),    /* Filter changed */
  VM_SIDEBAR_CHANGE_SCROLL = (1 << 3),    /* Scroll position changed */
  VM_SIDEBAR_CHANGE_LOADING = (1 << 4),   /* Loading state changed */
  VM_SIDEBAR_CHANGE_ALL = 0xFF
} VmSidebarChangeFlags;

/* ============================================================================
 * Callbacks
 * ============================================================================
 */

typedef struct {
  /* Called when sidebar state changes */
  void (*on_change)(VmSidebar *vm, VmSidebarChangeFlags changes, void *ctx);

  /* Called when table is opened */
  void (*on_table_open)(VmSidebar *vm, size_t index, const char *name,
                        void *ctx);

  /* User context */
  void *context;
} VmSidebarCallbacks;

/* ============================================================================
 * VmSidebar - Sidebar ViewModel
 * ============================================================================
 */

struct VmSidebar {
  /* Source (references, not owned) */
  AppState *app;
  Connection *connection; /* Current connection */

  /* Callbacks */
  VmSidebarCallbacks callbacks;

  /* Filter state */
  char filter[64];
  size_t filter_len;
  bool filter_active;

  /* Filtered table indices */
  size_t *filtered_indices; /* Indices into connection->tables */
  size_t filtered_count;
  size_t filtered_capacity;

  /* Selection and scroll */
  size_t selection; /* Index into filtered list */
  size_t scroll;    /* Scroll offset */

  /* Loading state */
  bool loading;
};

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create sidebar viewmodel */
VmSidebar *vm_sidebar_create(AppState *app,
                             const VmSidebarCallbacks *callbacks);

/* Destroy viewmodel */
void vm_sidebar_destroy(VmSidebar *vm);

/* Bind to a specific connection */
void vm_sidebar_bind(VmSidebar *vm, Connection *conn);

/* ============================================================================
 * Table List Access (for native list controls)
 * ============================================================================
 */

/* Get number of tables (filtered) */
size_t vm_sidebar_count(const VmSidebar *vm);

/* Get number of tables (unfiltered) */
size_t vm_sidebar_total_count(const VmSidebar *vm);

/* Get table name at filtered index */
const char *vm_sidebar_table_at(const VmSidebar *vm, size_t index);

/* Get original index for filtered index */
size_t vm_sidebar_original_index(const VmSidebar *vm, size_t filtered_index);

/* Check if table matches current filter */
bool vm_sidebar_matches_filter(const VmSidebar *vm, size_t original_index);

/* ============================================================================
 * Selection
 * ============================================================================
 */

/* Get selected index (in filtered list) */
size_t vm_sidebar_get_selection(const VmSidebar *vm);

/* Set selection */
void vm_sidebar_set_selection(VmSidebar *vm, size_t index);

/* Move selection */
void vm_sidebar_select_next(VmSidebar *vm);
void vm_sidebar_select_prev(VmSidebar *vm);
void vm_sidebar_select_first(VmSidebar *vm);
void vm_sidebar_select_last(VmSidebar *vm);

/* Get selected table name */
const char *vm_sidebar_selected_name(const VmSidebar *vm);

/* Get selected original index (for opening table) */
size_t vm_sidebar_selected_original_index(const VmSidebar *vm);

/* ============================================================================
 * Scroll
 * ============================================================================
 */

/* Get/set scroll offset */
size_t vm_sidebar_get_scroll(const VmSidebar *vm);
void vm_sidebar_set_scroll(VmSidebar *vm, size_t scroll);

/* Ensure selection is visible */
void vm_sidebar_ensure_visible(VmSidebar *vm, size_t visible_count);

/* ============================================================================
 * Filtering
 * ============================================================================
 */

/* Get current filter */
const char *vm_sidebar_get_filter(const VmSidebar *vm);

/* Set filter (rebuilds filtered list) */
void vm_sidebar_set_filter(VmSidebar *vm, const char *filter);

/* Filter input (appends character) */
void vm_sidebar_filter_append(VmSidebar *vm, char ch);
void vm_sidebar_filter_backspace(VmSidebar *vm);
void vm_sidebar_filter_clear(VmSidebar *vm);

/* Check if filter is active */
bool vm_sidebar_filter_active(const VmSidebar *vm);

/* ============================================================================
 * Actions
 * ============================================================================
 */

/* Open selected table (triggers callback) */
void vm_sidebar_open_selected(VmSidebar *vm);

/* Refresh table list from database */
void vm_sidebar_refresh(VmSidebar *vm);

/* ============================================================================
 * State
 * ============================================================================
 */

/* Check if loading */
bool vm_sidebar_is_loading(const VmSidebar *vm);

/* Check if valid (has connection with tables) */
bool vm_sidebar_valid(const VmSidebar *vm);

/* Get connection info */
const char *vm_sidebar_connection_name(const VmSidebar *vm);

#endif /* LACE_VM_SIDEBAR_H */
