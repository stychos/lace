/*
 * Lace
 * Application ViewModel - Implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "vm_app.h"
#include "../db/db.h"
#include "../util/mem.h"
#include "../util/str.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

static void notify_change(VmApp *vm, VmAppChangeFlags flags) {
  if (vm->callbacks.on_change) {
    vm->callbacks.on_change(vm, flags, vm->callbacks.context);
  }
}

static void status_clear(VmStatus *status) {
  free(status->message);
  status->message = NULL;
  status->is_error = false;
}

static void status_set(VmStatus *status, const char *message, bool is_error) {
  free(status->message);
  status->message = message ? str_dup(message) : NULL;
  status->is_error = is_error;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

VmApp *vm_app_create(AppState *app, const VmAppCallbacks *callbacks) {
  if (!app)
    return NULL;

  VmApp *vm = safe_calloc(1, sizeof(VmApp));
  vm->app = app;

  if (callbacks) {
    vm->callbacks = *callbacks;
  }

  /* Layout defaults */
  vm->sidebar_visible = true;
  vm->header_visible = app->header_visible;
  vm->status_visible = app->status_visible;

  return vm;
}

void vm_app_destroy(VmApp *vm) {
  if (!vm)
    return;

  /* Destroy child view models */
  table_vm_destroy(vm->table_vm);

  status_clear(&vm->status);

  free(vm);
}

/* ============================================================================
 * Connection Management
 * ============================================================================
 */

void vm_app_connect(VmApp *vm, const char *connstr) {
  if (!vm || !connstr)
    return;

  char *err = NULL;
  DbConnection *db_conn = db_connect(connstr, &err);

  if (!db_conn) {
    vm_app_set_error(vm, err ? err : "Connection failed");
    free(err);
    return;
  }

  /* Apply config limits to the new connection */
  if (vm->app && vm->app->config) {
    db_conn->max_result_rows = (size_t)vm->app->config->general.max_result_rows;
  }

  /* Add to connection pool */
  Connection *conn = app_add_connection(vm->app, db_conn, connstr);
  if (!conn) {
    db_disconnect(db_conn);
    vm_app_set_error(vm, "Failed to add connection");
    return;
  }

  /* Load tables */
  conn->tables = db_list_tables(db_conn, &conn->num_tables, &err);
  if (!conn->tables) {
    vm_app_set_error(vm, err ? err : "Failed to load tables");
    free(err);
    /* Keep connection anyway */
  }

  vm_app_set_status(vm, "Connected");
  notify_change(vm, VM_APP_CHANGE_CONNECTION);
}

void vm_app_disconnect(VmApp *vm) {
  if (!vm || !vm->app)
    return;

  /* Get current tab's connection */
  Tab *tab = app_current_tab(vm->app);
  if (!tab)
    return;

  size_t conn_idx = tab->connection_index;
  if (app_close_connection(vm->app, conn_idx)) {
    vm_app_set_status(vm, "Disconnected");
    notify_change(vm, VM_APP_CHANGE_CONNECTION);
  }
}

size_t vm_app_connection_count(const VmApp *vm) {
  if (!vm || !vm->app)
    return 0;
  return vm->app->num_connections;
}

const char *vm_app_connection_name(const VmApp *vm, size_t index) {
  if (!vm || !vm->app)
    return NULL;

  Connection *conn = app_get_connection(vm->app, index);
  return conn ? conn->connstr : NULL;
}

/* ============================================================================
 * Workspace Management
 * ============================================================================
 */

size_t vm_app_workspace_count(const VmApp *vm) {
  if (!vm || !vm->app)
    return 0;
  return vm->app->num_workspaces;
}

size_t vm_app_current_workspace(const VmApp *vm) {
  if (!vm || !vm->app)
    return 0;
  return vm->app->current_workspace;
}

void vm_app_switch_workspace(VmApp *vm, size_t index) {
  if (!vm || !vm->app)
    return;

  vm_app_sync_from_current_tab(vm);

  if (app_switch_workspace(vm->app, index)) {
    vm_app_sync_to_current_tab(vm);
    notify_change(vm, VM_APP_CHANGE_WORKSPACE | VM_APP_CHANGE_TAB);
  }
}

void vm_app_next_workspace(VmApp *vm) {
  if (!vm || !vm->app)
    return;

  size_t current = vm->app->current_workspace;
  size_t count = vm->app->num_workspaces;

  if (count > 1) {
    size_t next = (current + 1) % count;
    vm_app_switch_workspace(vm, next);
  }
}

void vm_app_prev_workspace(VmApp *vm) {
  if (!vm || !vm->app)
    return;

  size_t current = vm->app->current_workspace;
  size_t count = vm->app->num_workspaces;

  if (count > 1) {
    size_t prev = (current == 0) ? count - 1 : current - 1;
    vm_app_switch_workspace(vm, prev);
  }
}

bool vm_app_create_workspace(VmApp *vm) {
  if (!vm || !vm->app)
    return false;

  Workspace *ws = app_create_workspace(vm->app);
  if (ws) {
    notify_change(vm, VM_APP_CHANGE_WORKSPACE);
    return true;
  }
  return false;
}

bool vm_app_close_workspace(VmApp *vm, size_t index) {
  if (!vm || !vm->app)
    return false;

  if (app_close_workspace(vm->app, index)) {
    vm_app_sync_to_current_tab(vm);
    notify_change(vm, VM_APP_CHANGE_WORKSPACE | VM_APP_CHANGE_TAB);
    return true;
  }
  return false;
}

bool vm_app_close_current_workspace(VmApp *vm) {
  if (!vm || !vm->app)
    return false;
  return vm_app_close_workspace(vm, vm->app->current_workspace);
}

/* ============================================================================
 * Tab Management
 * ============================================================================
 */

size_t vm_app_tab_count(const VmApp *vm) {
  if (!vm || !vm->app)
    return 0;

  Workspace *ws = app_current_workspace(vm->app);
  return ws ? ws->num_tabs : 0;
}

size_t vm_app_current_tab(const VmApp *vm) {
  if (!vm || !vm->app)
    return 0;

  Workspace *ws = app_current_workspace(vm->app);
  return ws ? ws->current_tab : 0;
}

void vm_app_switch_tab(VmApp *vm, size_t index) {
  if (!vm || !vm->app)
    return;

  Workspace *ws = app_current_workspace(vm->app);
  if (!ws || index >= ws->num_tabs)
    return;

  vm_app_sync_from_current_tab(vm);

  if (workspace_switch_tab(ws, index)) {
    vm_app_sync_to_current_tab(vm);
    notify_change(vm, VM_APP_CHANGE_TAB);
  }
}

void vm_app_next_tab(VmApp *vm) {
  if (!vm || !vm->app)
    return;

  Workspace *ws = app_current_workspace(vm->app);
  if (!ws || ws->num_tabs <= 1)
    return;

  size_t next = (ws->current_tab + 1) % ws->num_tabs;
  vm_app_switch_tab(vm, next);
}

void vm_app_prev_tab(VmApp *vm) {
  if (!vm || !vm->app)
    return;

  Workspace *ws = app_current_workspace(vm->app);
  if (!ws || ws->num_tabs <= 1)
    return;

  size_t prev = (ws->current_tab == 0) ? ws->num_tabs - 1 : ws->current_tab - 1;
  vm_app_switch_tab(vm, prev);
}

bool vm_app_open_table(VmApp *vm, size_t connection_index, size_t table_index,
                       const char *table_name) {
  if (!vm || !vm->app)
    return false;

  Workspace *ws = app_current_workspace(vm->app);
  if (!ws)
    return false;

  vm_app_sync_from_current_tab(vm);

  Tab *tab =
      workspace_create_table_tab(ws, connection_index, table_index, table_name);
  if (tab) {
    vm_app_sync_to_current_tab(vm);
    notify_change(vm, VM_APP_CHANGE_TAB);
    return true;
  }
  return false;
}

bool vm_app_create_query_tab(VmApp *vm) {
  if (!vm || !vm->app)
    return false;

  Workspace *ws = app_current_workspace(vm->app);
  if (!ws)
    return false;

  /* Get connection index from current tab or use first connection */
  size_t conn_idx = 0;
  Tab *current = workspace_current_tab(ws);
  if (current) {
    conn_idx = current->connection_index;
  }

  vm_app_sync_from_current_tab(vm);

  Tab *tab = workspace_create_query_tab(ws, conn_idx);
  if (tab) {
    vm_app_sync_to_current_tab(vm);
    notify_change(vm, VM_APP_CHANGE_TAB);
    return true;
  }
  return false;
}

bool vm_app_close_tab(VmApp *vm, size_t index) {
  if (!vm || !vm->app)
    return false;

  Workspace *ws = app_current_workspace(vm->app);
  if (!ws)
    return false;

  if (workspace_close_tab(ws, index)) {
    vm_app_sync_to_current_tab(vm);
    notify_change(vm, VM_APP_CHANGE_TAB);
    return true;
  }
  return false;
}

bool vm_app_close_current_tab(VmApp *vm) {
  if (!vm || !vm->app)
    return false;

  Workspace *ws = app_current_workspace(vm->app);
  if (!ws)
    return false;

  return vm_app_close_tab(vm, ws->current_tab);
}

TabType vm_app_tab_type(const VmApp *vm, size_t index) {
  if (!vm || !vm->app)
    return TAB_TYPE_TABLE;

  Workspace *ws = app_current_workspace(vm->app);
  if (!ws || index >= ws->num_tabs)
    return TAB_TYPE_TABLE;

  return ws->tabs[index].type;
}

const char *vm_app_tab_name(const VmApp *vm, size_t index) {
  if (!vm || !vm->app)
    return NULL;

  Workspace *ws = app_current_workspace(vm->app);
  if (!ws || index >= ws->num_tabs)
    return NULL;

  Tab *tab = &ws->tabs[index];
  if (tab->type == TAB_TYPE_QUERY) {
    return "Query";
  }
  return tab->table_name;
}

/* ============================================================================
 * Current Tab Access
 * ============================================================================
 */

TableViewModel *vm_app_current_table_vm(VmApp *vm) {
  if (!vm)
    return NULL;

  Tab *tab = app_current_tab(vm->app);
  if (!tab || tab->type != TAB_TYPE_TABLE)
    return NULL;

  /* Create table VM if needed */
  if (!vm->table_vm) {
    vm->table_vm = table_vm_create(vm->app, tab);
  } else {
    table_vm_bind(vm->table_vm, tab);
  }

  return vm->table_vm;
}

/* ============================================================================
 * Layout
 * ============================================================================
 */

bool vm_app_sidebar_visible(const VmApp *vm) {
  return vm && vm->sidebar_visible;
}

void vm_app_set_sidebar_visible(VmApp *vm, bool visible) {
  if (!vm || vm->sidebar_visible == visible)
    return;

  vm->sidebar_visible = visible;
  notify_change(vm, VM_APP_CHANGE_LAYOUT);
}

void vm_app_toggle_sidebar(VmApp *vm) {
  if (vm)
    vm_app_set_sidebar_visible(vm, !vm->sidebar_visible);
}

bool vm_app_sidebar_focused(const VmApp *vm) {
  return vm && vm->sidebar_focused;
}

void vm_app_set_sidebar_focused(VmApp *vm, bool focused) {
  if (!vm || vm->sidebar_focused == focused)
    return;

  vm->sidebar_focused = focused;
  notify_change(vm, VM_APP_CHANGE_LAYOUT);
}

void vm_app_toggle_sidebar_focus(VmApp *vm) {
  if (vm)
    vm_app_set_sidebar_focused(vm, !vm->sidebar_focused);
}

bool vm_app_header_visible(const VmApp *vm) { return vm && vm->header_visible; }

void vm_app_set_header_visible(VmApp *vm, bool visible) {
  if (!vm || vm->header_visible == visible)
    return;

  vm->header_visible = visible;
  if (vm->app)
    vm->app->header_visible = visible;
  notify_change(vm, VM_APP_CHANGE_LAYOUT);
}

void vm_app_toggle_header(VmApp *vm) {
  if (vm)
    vm_app_set_header_visible(vm, !vm->header_visible);
}

bool vm_app_status_visible(const VmApp *vm) { return vm && vm->status_visible; }

void vm_app_set_status_visible(VmApp *vm, bool visible) {
  if (!vm || vm->status_visible == visible)
    return;

  vm->status_visible = visible;
  if (vm->app)
    vm->app->status_visible = visible;
  notify_change(vm, VM_APP_CHANGE_LAYOUT);
}

void vm_app_toggle_status(VmApp *vm) {
  if (vm)
    vm_app_set_status_visible(vm, !vm->status_visible);
}

/* ============================================================================
 * Status Messages
 * ============================================================================
 */

void vm_app_set_status(VmApp *vm, const char *message) {
  if (!vm)
    return;
  status_set(&vm->status, message, false);
  notify_change(vm, VM_APP_CHANGE_STATUS);
}

void vm_app_set_error(VmApp *vm, const char *message) {
  if (!vm)
    return;
  status_set(&vm->status, message, true);
  notify_change(vm, VM_APP_CHANGE_STATUS);
}

void vm_app_clear_status(VmApp *vm) {
  if (!vm)
    return;
  status_clear(&vm->status);
  notify_change(vm, VM_APP_CHANGE_STATUS);
}

const char *vm_app_get_status(const VmApp *vm) {
  return vm ? vm->status.message : NULL;
}

bool vm_app_status_is_error(const VmApp *vm) {
  return vm && vm->status.is_error;
}

/* ============================================================================
 * Actions
 * ============================================================================
 */

void vm_app_request_connect(VmApp *vm) {
  if (vm && vm->callbacks.on_connect_request) {
    vm->callbacks.on_connect_request(vm, vm->callbacks.context);
  }
}

void vm_app_request_quit(VmApp *vm) {
  if (vm && vm->callbacks.on_quit_request) {
    vm->callbacks.on_quit_request(vm, vm->callbacks.context);
  }
}

void vm_app_refresh(VmApp *vm) {
  if (!vm)
    return;

  Tab *tab = app_current_tab(vm->app);
  if (!tab)
    return;

  if (tab->type == TAB_TYPE_TABLE && vm->table_vm) {
    table_vm_refresh(vm->table_vm);
  }
  /* VmQuery removed - query execution handled by QueryWidget */
}

/* ============================================================================
 * State Sync
 * ============================================================================
 */

void vm_app_sync_to_current_tab(VmApp *vm) {
  if (!vm)
    return;

  Tab *tab = app_current_tab(vm->app);
  if (!tab)
    return;

  /* Bind appropriate VM */
  if (tab->type == TAB_TYPE_TABLE) {
    if (vm->table_vm) {
      table_vm_bind(vm->table_vm, tab);
    }
  }
  /* VmQuery removed - use QueryWidget instead */
}

void vm_app_sync_from_current_tab(VmApp *vm) {
  /* Currently, view models update Tab directly, so nothing to sync back */
  (void)vm;
}

/* ============================================================================
 * Utility
 * ============================================================================
 */

bool vm_app_has_content(const VmApp *vm) {
  if (!vm || !vm->app)
    return false;
  return vm->app->num_connections > 0;
}

bool vm_app_running(const VmApp *vm) {
  return vm && vm->app && vm->app->running;
}

void vm_app_set_running(VmApp *vm, bool running) {
  if (vm && vm->app) {
    vm->app->running = running;
  }
}
