/*
 * Lace
 * Core application state implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "app_state.h"
#include "../async/async.h"
#include "../db_compat.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include "../../liblace/include/util/connstr.h"
#include "history.h"
#include "conn_fields.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Default page size uses config constant from config.h */

/* Note: History tracking with liblace is done at the TUI level.
 * The old db layer had callbacks, but liblace doesn't expose these. */

/* ============================================================================
 * Dynamic Array Helpers
 * ============================================================================
 */

/* Grow tabs array if needed, returns true on success */
static bool workspace_ensure_tab_capacity(Workspace *ws) {
  if (!ws)
    return false;

  if (ws->num_tabs < ws->tab_capacity)
    return true; /* Already have capacity */

  size_t new_capacity =
      ws->tab_capacity == 0 ? INITIAL_TAB_CAPACITY : ws->tab_capacity * 2;
  ws->tabs = safe_reallocarray(ws->tabs, new_capacity, sizeof(Tab));

  /* Zero new entries */
  memset(&ws->tabs[ws->tab_capacity], 0,
         (new_capacity - ws->tab_capacity) * sizeof(Tab));

  ws->tab_capacity = new_capacity;
  return true;
}

/* Grow connections array if needed, returns true on success */
static bool app_ensure_connection_capacity(AppState *app) {
  if (!app)
    return false;

  if (app->num_connections < app->connection_capacity)
    return true;

  size_t new_capacity = app->connection_capacity == 0
                            ? INITIAL_CONNECTION_CAPACITY
                            : app->connection_capacity * 2;
  app->connections =
      safe_reallocarray(app->connections, new_capacity, sizeof(Connection));

  memset(&app->connections[app->connection_capacity], 0,
         (new_capacity - app->connection_capacity) * sizeof(Connection));

  app->connection_capacity = new_capacity;
  return true;
}

/* Grow workspaces array if needed, returns true on success */
static bool app_ensure_workspace_capacity(AppState *app) {
  if (!app)
    return false;

  if (app->num_workspaces < app->workspace_capacity)
    return true;

  size_t new_capacity = app->workspace_capacity == 0
                            ? INITIAL_WORKSPACE_CAPACITY
                            : app->workspace_capacity * 2;
  app->workspaces =
      safe_reallocarray(app->workspaces, new_capacity, sizeof(Workspace));

  memset(&app->workspaces[app->workspace_capacity], 0,
         (new_capacity - app->workspace_capacity) * sizeof(Workspace));

  app->workspace_capacity = new_capacity;
  return true;
}

/* ============================================================================
 * Tab Lifecycle
 * ============================================================================
 */

void tab_init(Tab *tab) {
  if (!tab)
    return;
  static uint64_t next_uid = 1; /* Tabs are created on the main thread only */
  memset(tab, 0, sizeof(Tab));
  tab->uid = next_uid++;
  tab->info_count = -1;
  filters_init(&tab->filters);
}

bool tab_has_subview(const Tab *tab, TabSubView sv) {
  if (!tab || tab->type != TAB_TYPE_TABLE) return false;
  switch (tab->object) {
  case LACE_OBJ_TABLE: return sv < SUBVIEW_COUNT;
  case LACE_OBJ_VIEW: return sv == SUBVIEW_DATA || sv == SUBVIEW_STRUCTURE || sv == SUBVIEW_INFORMATION;
  default: return sv == SUBVIEW_STRUCTURE || sv == SUBVIEW_INFORMATION;
  }
}

void tab_drop_catalog(Tab *tab) {
  if (!tab) return;
  lace_table_info_free(tab->info);
  lace_autoinc_free(tab->autoinc);
  tab->info = NULL;
  tab->autoinc = NULL;
  FREE_NULL(tab->info_error);
  tab->info_count = -1;
  lace_dependents_free(tab->deps, tab->num_deps);
  tab->deps = NULL;
  tab->num_deps = 0;
  tab->deps_loaded = false;
  FREE_NULL(tab->deps_error);
  FREE_NULL(tab->definition);
  FREE_NULL(tab->definition_error);
}

static const char *const SUBVIEW_KEYS[SUBVIEW_COUNT] = {"data", "structure", "indexes", "foreign_keys", "information"};

const char *tab_subview_key(const Tab *tab, TabSubView sv) {
  if (sv == SUBVIEW_STRUCTURE && tab && tab->object != LACE_OBJ_TABLE) return "definition";
  return sv < SUBVIEW_COUNT ? SUBVIEW_KEYS[sv] : "data";
}

bool tab_subview_from_key(const char *key, TabSubView *out) {
  if (!key) return false;
  if (!strcmp(key, "definition")) { *out = SUBVIEW_STRUCTURE; return true; }
  for (int i = 0; i < SUBVIEW_COUNT; i++)
    if (!strcmp(key, SUBVIEW_KEYS[i])) { *out = (TabSubView)i; return true; }
  return false;
}

static const char *const OBJECT_KEYS[] = {"table", "view", "routine", "trigger"};

const char *tab_object_key(LaceObjectKind kind) { return kind <= LACE_OBJ_TRIGGER ? OBJECT_KEYS[kind] : "table"; }

bool tab_object_from_key(const char *key, LaceObjectKind *out) {
  for (int i = 0; key && i < 4; i++)
    if (!strcmp(key, OBJECT_KEYS[i])) { *out = (LaceObjectKind)i; return true; }
  return false;
}

void query_count_stop(Tab *tab) {
  if (!tab || !tab->query_count_op) {
    return;
  }
  AsyncOperation *op = (AsyncOperation *)tab->query_count_op;
  if (op->conn && op->conn->background_op == op) {
    op->conn->background_op = NULL;
  }
  async_cancel_and_wait(op);
  lace_mutex_lock(&op->mutex);
  if (op->result) {
    db_result_free((ResultSet *)op->result);
    op->result = NULL;
  }
  lace_mutex_unlock(&op->mutex);
  async_free(op);
  free(op);
  tab->query_count_op = NULL;
}

void (*tab_form_free_hook)(struct FormTab *form);

void tab_free_data(Tab *tab) {
  if (!tab)
    return;
  if (tab->form && tab_form_free_hook)
    tab_form_free_hook(tab->form);
  tab->form = NULL;
  undo_stack_clear(&tab->undo);

  /* Cancel any pending background operation first to prevent use-after-free */
  if (tab->bg_load_op) {
    AsyncOperation *op = (AsyncOperation *)tab->bg_load_op;
    async_cancel(op);

    /* Wait for operation to complete - important for connection safety */
    async_wait(op, 500);

    /* Spin-wait if still running (shouldn't happen often) */
    while (async_poll(op) == ASYNC_STATE_RUNNING) {
      struct timespec ts = {0, 10000000L}; /* 10ms */
      nanosleep(&ts, NULL);
    }

    /* Free result if any */
    lace_mutex_lock(&op->mutex);
    if (op->result) {
      db_result_free((ResultSet *)op->result);
      op->result = NULL;
    }
    lace_mutex_unlock(&op->mutex);

    async_free(op);
    free(op);
    tab->bg_load_op = NULL;
  }

  query_count_stop(tab);

  /* Free table data */
  FREE_NULL(tab->table_name);
  FREE_NULL(tab->table_error);
  FREE_NULL(tab->object_table);
  FREE_NULL(tab->object_detail);
  tab_drop_catalog(tab);
  db_result_free(tab->data);
  tab->data = NULL;
  db_schema_free(tab->schema);
  tab->schema = NULL;
  FREE_NULL(tab->col_widths);
  tab->num_col_widths = 0;
  filters_free(&tab->filters);


  /* Free query data */
  FREE_NULL(tab->query_text);
  db_result_free(tab->query_results);
  tab->query_results = NULL;
  FREE_NULL(tab->query_error);
  FREE_NULL(tab->query_result_col_widths);

  /* Note: query_result_edit_buf is now in UITabState (TUI layer) */
  FREE_NULL(tab->query_source_table);
  db_schema_free(tab->query_source_schema);
  tab->query_source_schema = NULL;
  FREE_NULL(tab->query_base_sql);

  /* Free row selections */
  FREE_NULL(tab->selected_rows);
  tab->num_selected = 0;
  tab->selected_capacity = 0;
}

Tab *workspace_current_tab(Workspace *ws) {
  if (!ws || ws->num_tabs == 0)
    return NULL;
  if (ws->current_tab >= ws->num_tabs)
    return NULL;
  return &ws->tabs[ws->current_tab];
}

size_t app_tab_table_position(AppState *app, const Tab *tab) {
  if (!tab || tab->type != TAB_TYPE_TABLE || !tab->table_name)
    return SIZE_MAX;
  Connection *conn = app_get_tab_connection(app, (Tab *)tab);
  for (size_t i = 0; conn && conn->tables && i < conn->num_tables; i++) {
    if (conn->tables[i] && strcmp(conn->tables[i], tab->table_name) == 0)
      return i;
  }
  return SIZE_MAX;
}

Tab *workspace_create_table_tab(Workspace *ws, size_t connection_index,
                                const char *table_name) {
  if (!ws || !table_name)
    return NULL;

  if (!workspace_ensure_tab_capacity(ws))
    return NULL;

  size_t new_idx = ws->num_tabs;
  Tab *tab = &ws->tabs[new_idx];
  tab_init(tab);

  tab->active = true;
  tab->type = TAB_TYPE_TABLE;
  tab->connection_index = connection_index;
  tab->table_name = str_dup(table_name);

  ws->num_tabs++;
  ws->current_tab = new_idx;

  return tab;
}

Tab *workspace_create_query_tab(Workspace *ws, size_t connection_index) {
  if (!ws)
    return NULL;

  if (!workspace_ensure_tab_capacity(ws))
    return NULL;

  size_t new_idx = ws->num_tabs;
  Tab *tab = &ws->tabs[new_idx];
  tab_init(tab);

  tab->type = TAB_TYPE_QUERY;
  tab->connection_index = connection_index;
  tab->table_name = str_dup("Query");

  /* Initialize query buffer */
  tab->query_capacity = 1024;
  tab->query_text = safe_malloc(tab->query_capacity);
  tab->query_text[0] = '\0';
  tab->query_len = 0;

  /* All allocations succeeded - now commit the tab */
  tab->active = true;
  ws->num_tabs++;
  ws->current_tab = new_idx;

  return tab;
}

void tab_make_connection(Tab *tab, size_t connection_index, const char *connstr) {
  if (!tab)
    return;
  uint64_t uid = tab->uid;
  tab_free_data(tab);
  tab_init(tab);
  if (uid)
    tab->uid = uid;
  tab->type = TAB_TYPE_CONNECTION;
  tab->connection_index = connection_index;
  tab->active = true;

  /* A short display name: the database (a file's base name), else the
   * host. Parsed, never cut from the raw string, which holds passwords. */
  ConnString *cs = connstr ? connstr_parse(connstr, NULL) : NULL;
  if (cs) {
    const char *name = NULL;
    if (cs->database && cs->database[0]) {
      const char *slash = strrchr(cs->database, '/');
      name = slash && slash[1] ? slash + 1 : cs->database;
    } else if (cs->host && cs->host[0]) {
      name = cs->host;
    }
    if (name)
      tab->table_name = str_dup(name);
    connstr_free(cs);
  }
  if (!tab->table_name)
    tab->table_name = str_dup("Connection");
}

Tab *workspace_create_connection_tab(Workspace *ws, size_t connection_index,
                                     const char *connstr) {
  if (!ws)
    return NULL;

  if (!workspace_ensure_tab_capacity(ws))
    return NULL;

  size_t new_idx = ws->num_tabs;
  Tab *tab = &ws->tabs[new_idx];
  memset(tab, 0, sizeof(Tab));
  tab_make_connection(tab, connection_index, connstr);
  ws->num_tabs++;
  ws->current_tab = new_idx;

  return tab;
}

Tab *workspace_insert_form_tab(Workspace *ws, size_t at) {
  if (!ws || !workspace_ensure_tab_capacity(ws))
    return NULL;
  if (at > ws->num_tabs)
    at = ws->num_tabs;
  memmove(&ws->tabs[at + 1], &ws->tabs[at], (ws->num_tabs - at) * sizeof(Tab));
  Tab *tab = &ws->tabs[at];
  tab_init(tab);
  tab->type = TAB_TYPE_FORM;
  tab->connection_index = TAB_NO_CONNECTION;
  tab->active = true;
  ws->num_tabs++;
  ws->current_tab = at;
  return tab;
}

void tab_make_form(Tab *tab) {
  if (!tab)
    return;
  uint64_t uid = tab->uid;
  tab_free_data(tab);
  tab_init(tab);
  tab->uid = uid;
  tab->type = TAB_TYPE_FORM;
  tab->connection_index = TAB_NO_CONNECTION;
  tab->active = true;
}

bool workspace_close_tab(Workspace *ws, size_t index) {
  if (!ws || index >= ws->num_tabs)
    return false;

  Tab *tab = &ws->tabs[index];

  /* Free tab data */
  tab_free_data(tab);
  memset(tab, 0, sizeof(Tab));

  /* Shift remaining tabs down */
  for (size_t i = index; i < ws->num_tabs - 1; i++) {
    ws->tabs[i] = ws->tabs[i + 1];
  }
  memset(&ws->tabs[ws->num_tabs - 1], 0, sizeof(Tab));

  ws->num_tabs--;

  /* Adjust current tab index */
  if (ws->num_tabs > 0) {
    if (ws->current_tab >= ws->num_tabs) {
      ws->current_tab = ws->num_tabs - 1;
    }
  } else {
    ws->current_tab = 0;
  }

  return true;
}

Tab *workspace_switch_tab(Workspace *ws, size_t index) {
  if (!ws || index >= ws->num_tabs)
    return NULL;
  if (index == ws->current_tab)
    return &ws->tabs[index];

  ws->current_tab = index;
  return &ws->tabs[index];
}

/* ============================================================================
 * Workspace Lifecycle
 * ============================================================================
 */

void workspace_init(Workspace *ws) {
  if (!ws)
    return;
  memset(ws, 0, sizeof(Workspace));

  /* Allocate initial tabs array */
  ws->tabs = safe_calloc(INITIAL_TAB_CAPACITY, sizeof(Tab));
  ws->tab_capacity = INITIAL_TAB_CAPACITY;
}

void workspace_free_data(Workspace *ws) {
  if (!ws)
    return;

  /* Free all tabs */
  if (ws->tabs) {
    for (size_t i = 0; i < ws->num_tabs; i++) {
      tab_free_data(&ws->tabs[i]);
    }
    free(ws->tabs);
  }

  memset(ws, 0, sizeof(Workspace));
}

Workspace *app_current_workspace(AppState *app) {
  if (!app || app->num_workspaces == 0)
    return NULL;
  if (app->current_workspace >= app->num_workspaces)
    return NULL;
  return &app->workspaces[app->current_workspace];
}

Workspace *app_create_workspace(AppState *app) {
  if (!app)
    return NULL;

  if (!app_ensure_workspace_capacity(app))
    return NULL;

  size_t new_idx = app->num_workspaces;
  Workspace *ws = &app->workspaces[new_idx];
  workspace_init(ws);

  ws->active = true;

  app->num_workspaces++;
  app->current_workspace = new_idx;

  return ws;
}

bool app_close_workspace(AppState *app, size_t index) {
  if (!app || index >= app->num_workspaces)
    return false;

  Workspace *ws = &app->workspaces[index];

  /* Free workspace data (including all tabs) */
  workspace_free_data(ws);

  /* Shift remaining workspaces down */
  for (size_t i = index; i < app->num_workspaces - 1; i++) {
    app->workspaces[i] = app->workspaces[i + 1];
  }
  memset(&app->workspaces[app->num_workspaces - 1], 0, sizeof(Workspace));

  app->num_workspaces--;

  /* Adjust current workspace index */
  if (app->num_workspaces > 0) {
    if (app->current_workspace >= app->num_workspaces) {
      app->current_workspace = app->num_workspaces - 1;
    }
  } else {
    app->current_workspace = 0;
  }

  return true;
}

Workspace *app_switch_workspace(AppState *app, size_t index) {
  if (!app || index >= app->num_workspaces)
    return NULL;
  if (index == app->current_workspace)
    return &app->workspaces[index];

  app->current_workspace = index;
  return &app->workspaces[index];
}

/* ============================================================================
 * Connection Pool Management
 * ============================================================================
 */

void connection_init(Connection *conn) {
  if (!conn)
    return;
  memset(conn, 0, sizeof(Connection));
}

void connection_free_data(Connection *conn) {
  if (!conn)
    return;

  /* Free table list */
  if (conn->tables) {
    for (size_t i = 0; i < conn->num_tables; i++) {
      free(conn->tables[i]);
    }
    free(conn->tables);
    conn->tables = NULL;
  }
  conn->num_tables = 0;
  lace_objects_free(conn->objects, conn->num_objects);
  conn->objects = NULL;
  conn->num_objects = 0;

  /* Free connection string and saved connection ID */
  FREE_NULL(conn->connstr);
  FREE_NULL(conn->saved_conn_id);

  /* Free query history */
  history_free(conn->history);
  conn->history = NULL;

  /* Disconnect database */
  if (conn->conn) {
    db_disconnect(conn->conn);
    conn->conn = NULL;
  }

  conn->active = false;
}

/* Record a statement in the history of the connection that ran it.
 * Main thread only (history is not locked). */
void app_record_history(AppState *app, DbConnection *db_conn, const char *sql,
                        int type) {
  if (!app || !db_conn || !sql || !*sql) {
    return;
  }
  if (!app->config || app->config->general.history_mode == HISTORY_MODE_OFF) {
    return;
  }
  for (size_t i = 0; i < app->num_connections; i++) {
    Connection *c = &app->connections[i];
    if (c->active && c->conn == db_conn && c->history) {
      history_add(c->history, sql, (HistoryEntryType)type,
                  (size_t)app->config->general.history_max_size);
      return;
    }
  }
}

static void app_history_hook(void *ctx, DbConnection *db_conn, const char *sql,
                             int type) {
  app_record_history((AppState *)ctx, db_conn, sql, type);
}

Connection *app_add_connection(AppState *app, DbConnection *db_conn,
                               const char *connstr) {
  if (!app)
    return NULL;

  if (!app_ensure_connection_capacity(app))
    return NULL;

  size_t new_idx = app->num_connections;
  Connection *conn = &app->connections[new_idx];
  connection_init(conn);

  conn->active = true;
  conn->conn = db_conn;
  conn->connstr = str_dup(connstr);

  /* Create history object if history tracking is enabled */
  if (app->config && app->config->general.history_mode != HISTORY_MODE_OFF) {
    conn->history = history_create(NULL); /* ID set later when known */
  }

  /* Record data changes made through this connection */
  if (db_conn) {
    db_conn->on_sql = app_history_hook;
    db_conn->on_sql_ctx = app;
  }

  app->num_connections++;

  return conn;
}

Connection *app_get_connection(AppState *app, size_t index) {
  if (!app || index >= app->num_connections)
    return NULL;
  if (!app->connections[index].active)
    return NULL;
  return &app->connections[index];
}

bool app_close_connection(AppState *app, size_t index) {
  if (!app || index >= app->num_connections)
    return false;

  Connection *conn = &app->connections[index];

  /* Save history before closing if in persistent mode */
  if (app->config &&
      app->config->general.history_mode == HISTORY_MODE_PERSISTENT &&
      conn->history && conn->saved_conn_id) {
    /* Update connection ID in history before saving */
    if (!conn->history->connection_id && conn->saved_conn_id) {
      conn->history->connection_id = str_dup(conn->saved_conn_id);
    }
    char *err = NULL;
    if (!history_save(conn->history, &err)) {
      /* Log error but don't fail the close */
      free(err);
    }
  }

  /* First, close all tabs that reference this connection.
   * Iterate backwards to avoid index shifting issues when closing tabs. */
  for (size_t w = 0; w < app->num_workspaces; w++) {
    Workspace *ws = &app->workspaces[w];
    /* Iterate backwards through tabs */
    for (size_t t = ws->num_tabs; t > 0; t--) {
      Tab *tab = &ws->tabs[t - 1];
      if (tab->connection_index == index) {
        /* Close this tab - it references the connection being closed */
        workspace_close_tab(ws, t - 1);
      }
    }
  }

  /* Free connection data */
  connection_free_data(conn);

  /* Shift remaining connections down */
  for (size_t i = index; i < app->num_connections - 1; i++) {
    app->connections[i] = app->connections[i + 1];
  }
  memset(&app->connections[app->num_connections - 1], 0, sizeof(Connection));

  app->num_connections--;

  /* Update connection_index in all remaining tabs that reference connections
   * after this one */
  for (size_t w = 0; w < app->num_workspaces; w++) {
    Workspace *ws = &app->workspaces[w];
    for (size_t t = 0; t < ws->num_tabs; t++) {
      Tab *tab = &ws->tabs[t];
      if (tab->connection_index > index && tab->connection_index != TAB_NO_CONNECTION) {
        tab->connection_index--;
      }
    }
  }

  return true;
}

size_t app_find_connection_index(AppState *app, DbConnection *conn) {
  if (!app || !conn)
    return (size_t)-1;

  for (size_t i = 0; i < app->num_connections; i++) {
    if (app->connections[i].conn == conn)
      return i;
  }
  return (size_t)-1;
}

/* ============================================================================
 * Application State Lifecycle
 * ============================================================================
 */

void app_state_init(AppState *app) {
  if (!app)
    return;

  memset(app, 0, sizeof(AppState));
  app->running = true; /* App is running after init */

  /* Load configuration */
  app->config = config_load(NULL);
  if (app->config) {
    /* Apply config values */
    app->header_visible = app->config->general.show_header;
    app->status_visible = app->config->general.show_status_bar;
  } else {
    /* Fallback defaults if config failed to load */
    app->header_visible = true;
    app->status_visible = true;
  }

  /* Allocate initial dynamic arrays */
  app->connections = safe_calloc(INITIAL_CONNECTION_CAPACITY, sizeof(Connection));
  app->connection_capacity = INITIAL_CONNECTION_CAPACITY;

  app->workspaces = safe_calloc(INITIAL_WORKSPACE_CAPACITY, sizeof(Workspace));
  app->workspace_capacity = INITIAL_WORKSPACE_CAPACITY;
}

void app_state_cleanup(AppState *app) {
  if (!app)
    return;

  /* Free configuration */
  if (app->config) {
    config_free(app->config);
    app->config = NULL;
  }

  /* Close all connections */
  if (app->connections) {
    for (size_t i = 0; i < app->num_connections; i++) {
      connection_free_data(&app->connections[i]);
    }
    free(app->connections);
  }

  /* Free all workspaces */
  if (app->workspaces) {
    for (size_t i = 0; i < app->num_workspaces; i++) {
      workspace_free_data(&app->workspaces[i]);
    }
    free(app->workspaces);
  }

  memset(app, 0, sizeof(AppState));
}

/* ============================================================================
 * Convenience Accessors
 * ============================================================================
 */

Tab *app_current_tab(AppState *app) {
  Workspace *ws = app_current_workspace(app);
  if (!ws)
    return NULL;
  return workspace_current_tab(ws);
}

size_t app_connection_users(const AppState *app, size_t conn_idx) {
  size_t n = 0;
  for (size_t w = 0; app && conn_idx != TAB_NO_CONNECTION && w < app->num_workspaces; w++)
    for (size_t t = 0; t < app->workspaces[w].num_tabs; t++)
      n += app->workspaces[w].tabs[t].type != TAB_TYPE_FORM &&
           app->workspaces[w].tabs[t].connection_index == conn_idx;
  return n;
}

size_t app_find_open_connection(AppState *app, const char *saved_id, const char *connstr) {
  bool by_id = saved_id && *saved_id;
  for (size_t i = 0; app && i < app->num_connections; i++) {
    const Connection *c = &app->connections[i];
    if (!c->active)
      continue;
    if (by_id ? c->saved_conn_id && !strcmp(c->saved_conn_id, saved_id) &&
                    (!connstr || conn_key_matches(NULL, connstr, NULL, c->connstr))
              : conn_key_matches(NULL, connstr, c->saved_conn_id, c->connstr))
      return i;
  }
  return SIZE_MAX;
}

Connection *app_get_tab_connection(AppState *app, Tab *tab) {
  if (!app || !tab)
    return NULL;
  return app_get_connection(app, tab->connection_index);
}

Connection *app_current_tab_connection(AppState *app) {
  Tab *tab = app_current_tab(app);
  if (!tab)
    return NULL;
  return app_get_tab_connection(app, tab);
}

/* ============================================================================
 * Row Selection Operations
 * ============================================================================
 */

/* Toggle selection of a row by global index */
bool tab_toggle_selection(Tab *tab, size_t global_row) {
  if (!tab)
    return false;

  /* Check if already selected - if so, remove */
  if (!tab->selected_rows && tab->num_selected > 0)
    return false; /* Invalid state */
  for (size_t i = 0; i < tab->num_selected; i++) {
    if (tab->selected_rows[i] == global_row) {
      /* Remove by shifting remaining elements */
      for (size_t j = i; j < tab->num_selected - 1; j++) {
        tab->selected_rows[j] = tab->selected_rows[j + 1];
      }
      tab->num_selected--;
      return true;
    }
  }

  /* Not selected - add it */
  /* Ensure capacity (also handle NULL selected_rows) */
  if (tab->num_selected >= tab->selected_capacity || !tab->selected_rows) {
    size_t new_cap = tab->selected_capacity == 0 ? INITIAL_SELECTION_CAPACITY
                                                 : tab->selected_capacity * 2;
    tab->selected_rows =
        safe_reallocarray(tab->selected_rows, new_cap, sizeof(size_t));
    tab->selected_capacity = new_cap;
  }

  tab->selected_rows[tab->num_selected++] = global_row;
  return true;
}

/* Check if a row is selected */
bool tab_is_row_selected(Tab *tab, size_t global_row) {
  if (!tab || !tab->selected_rows)
    return false;

  for (size_t i = 0; i < tab->num_selected; i++) {
    if (tab->selected_rows[i] == global_row)
      return true;
  }
  return false;
}

/* Clear all selections */
void tab_clear_selections(Tab *tab) {
  if (!tab)
    return;
  tab->num_selected = 0;
  /* Keep the array allocated for reuse */
}

/* ============================================================================
 * Data Change Tracking
 * ============================================================================
 */

/* Mark all tabs with the same table as needing refresh (except current tab) */
void app_mark_table_tabs_dirty(AppState *app, size_t connection_index,
                               const char *table_name, Tab *exclude_tab) {
  if (!app || !table_name)
    return;

  /* Iterate through all workspaces and tabs */
  for (size_t ws_idx = 0; ws_idx < app->num_workspaces; ws_idx++) {
    Workspace *ws = &app->workspaces[ws_idx];
    if (!ws->active)
      continue;

    for (size_t tab_idx = 0; tab_idx < ws->num_tabs; tab_idx++) {
      Tab *tab = &ws->tabs[tab_idx];
      if (!tab->active || tab == exclude_tab)
        continue;

      /* Check if this is a table tab for the same table */
      if (tab->type == TAB_TYPE_TABLE && tab->table_name &&
          tab->connection_index == connection_index &&
          strcmp(tab->table_name, table_name) == 0) {
        tab->needs_refresh = true;
      }

      /* Also check query tabs that have results from this table */
      if (tab->type == TAB_TYPE_QUERY && tab->query_source_table &&
          tab->connection_index == connection_index &&
          strcmp(tab->query_source_table, table_name) == 0) {
        tab->needs_refresh = true;
      }
    }
  }
}
