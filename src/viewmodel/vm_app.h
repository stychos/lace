/*
 * Lace
 * Application ViewModel - Platform-independent app-level model
 *
 * Provides a clean interface for managing workspaces, tabs, and connections.
 * Coordinates between sidebar, table, and query view models.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_VM_APP_H
#define LACE_VM_APP_H

#include "../core/app_state.h"
#include "vm_query.h"
#include "vm_sidebar.h"
#include "vm_table.h"
#include <stdbool.h>
#include <stddef.h>

/* Forward declarations */
typedef struct VmApp VmApp;

/* ============================================================================
 * Change Flags
 * ============================================================================
 */

typedef enum {
  VM_APP_CHANGE_NONE = 0,
  VM_APP_CHANGE_CONNECTION = (1 << 0), /* Connection added/removed/changed */
  VM_APP_CHANGE_WORKSPACE = (1 << 1),  /* Workspace added/removed/switched */
  VM_APP_CHANGE_TAB = (1 << 2),        /* Tab added/removed/switched */
  VM_APP_CHANGE_STATUS = (1 << 3),     /* Status message changed */
  VM_APP_CHANGE_LAYOUT = (1 << 4),     /* UI layout changed (sidebar visible) */
  VM_APP_CHANGE_ALL = 0xFF
} VmAppChangeFlags;

/* ============================================================================
 * Status Message
 * ============================================================================
 */

typedef struct {
  char *message;
  bool is_error;
} VmStatus;

/* ============================================================================
 * Callbacks
 * ============================================================================
 */

typedef struct {
  /* Called when app state changes */
  void (*on_change)(VmApp *vm, VmAppChangeFlags changes, void *ctx);

  /* Called when a new connection is needed */
  void (*on_connect_request)(VmApp *vm, void *ctx);

  /* Called when user wants to quit */
  void (*on_quit_request)(VmApp *vm, void *ctx);

  /* User context */
  void *context;
} VmAppCallbacks;

/* ============================================================================
 * VmApp - Application ViewModel
 * ============================================================================
 */

struct VmApp {
  /* Core app state (not owned) */
  AppState *app;

  /* Callbacks */
  VmAppCallbacks callbacks;

  /* Child view models (owned) */
  VmSidebar *sidebar_vm;
  VmTable *table_vm;
  VmQuery *query_vm;

  /* Status */
  VmStatus status;

  /* Layout state */
  bool sidebar_visible;
  bool sidebar_focused;
  bool header_visible;
  bool status_visible;
};

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create application viewmodel */
VmApp *vm_app_create(AppState *app, const VmAppCallbacks *callbacks);

/* Destroy viewmodel and all child view models */
void vm_app_destroy(VmApp *vm);

/* ============================================================================
 * Connection Management
 * ============================================================================
 */

/* Connect to database (async) */
void vm_app_connect(VmApp *vm, const char *connstr);

/* Disconnect current connection */
void vm_app_disconnect(VmApp *vm);

/* Get number of connections */
size_t vm_app_connection_count(const VmApp *vm);

/* Get connection info */
const char *vm_app_connection_name(const VmApp *vm, size_t index);

/* ============================================================================
 * Workspace Management
 * ============================================================================
 */

/* Get number of workspaces */
size_t vm_app_workspace_count(const VmApp *vm);

/* Get current workspace index */
size_t vm_app_current_workspace(const VmApp *vm);

/* Switch workspace */
void vm_app_switch_workspace(VmApp *vm, size_t index);
void vm_app_next_workspace(VmApp *vm);
void vm_app_prev_workspace(VmApp *vm);

/* Create new workspace */
bool vm_app_create_workspace(VmApp *vm);

/* Close workspace */
bool vm_app_close_workspace(VmApp *vm, size_t index);
bool vm_app_close_current_workspace(VmApp *vm);

/* ============================================================================
 * Tab Management
 * ============================================================================
 */

/* Get number of tabs in current workspace */
size_t vm_app_tab_count(const VmApp *vm);

/* Get current tab index */
size_t vm_app_current_tab(const VmApp *vm);

/* Switch tab */
void vm_app_switch_tab(VmApp *vm, size_t index);
void vm_app_next_tab(VmApp *vm);
void vm_app_prev_tab(VmApp *vm);

/* Create tabs */
bool vm_app_open_table(VmApp *vm, size_t connection_index, size_t table_index,
                       const char *table_name);
bool vm_app_create_query_tab(VmApp *vm);

/* Close tab */
bool vm_app_close_tab(VmApp *vm, size_t index);
bool vm_app_close_current_tab(VmApp *vm);

/* Get tab info */
TabType vm_app_tab_type(const VmApp *vm, size_t index);
const char *vm_app_tab_name(const VmApp *vm, size_t index);

/* ============================================================================
 * Current Tab Access
 * ============================================================================
 */

/* Get viewmodel for current tab (either table or query) */
VmTable *vm_app_current_table_vm(VmApp *vm);
VmQuery *vm_app_current_query_vm(VmApp *vm);

/* Get sidebar viewmodel */
VmSidebar *vm_app_sidebar_vm(VmApp *vm);

/* ============================================================================
 * Layout
 * ============================================================================
 */

/* Sidebar visibility */
bool vm_app_sidebar_visible(const VmApp *vm);
void vm_app_set_sidebar_visible(VmApp *vm, bool visible);
void vm_app_toggle_sidebar(VmApp *vm);

/* Sidebar focus */
bool vm_app_sidebar_focused(const VmApp *vm);
void vm_app_set_sidebar_focused(VmApp *vm, bool focused);
void vm_app_toggle_sidebar_focus(VmApp *vm);

/* Header visibility */
bool vm_app_header_visible(const VmApp *vm);
void vm_app_set_header_visible(VmApp *vm, bool visible);
void vm_app_toggle_header(VmApp *vm);

/* Status bar visibility */
bool vm_app_status_visible(const VmApp *vm);
void vm_app_set_status_visible(VmApp *vm, bool visible);
void vm_app_toggle_status(VmApp *vm);

/* ============================================================================
 * Status Messages
 * ============================================================================
 */

/* Set status message */
void vm_app_set_status(VmApp *vm, const char *message);
void vm_app_set_error(VmApp *vm, const char *message);
void vm_app_clear_status(VmApp *vm);

/* Get status */
const char *vm_app_get_status(const VmApp *vm);
bool vm_app_status_is_error(const VmApp *vm);

/* ============================================================================
 * Actions
 * ============================================================================
 */

/* Request connection dialog */
void vm_app_request_connect(VmApp *vm);

/* Request quit */
void vm_app_request_quit(VmApp *vm);

/* Refresh current view */
void vm_app_refresh(VmApp *vm);

/* ============================================================================
 * State Sync
 * ============================================================================
 */

/* Called after tab/workspace switch to update child view models */
void vm_app_sync_to_current_tab(VmApp *vm);

/* Called before tab/workspace switch to save current state */
void vm_app_sync_from_current_tab(VmApp *vm);

/* ============================================================================
 * Utility
 * ============================================================================
 */

/* Check if app has any content */
bool vm_app_has_content(const VmApp *vm);

/* Check if app is running */
bool vm_app_running(const VmApp *vm);

/* Set running flag */
void vm_app_set_running(VmApp *vm, bool running);

#endif /* LACE_VM_APP_H */
