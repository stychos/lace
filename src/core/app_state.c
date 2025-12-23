/*
 * Lace
 * Core application state implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "app_state.h"
#include "../util/str.h"
#include <stdlib.h>
#include <string.h>

/* Default page size for data loading */
#define DEFAULT_PAGE_SIZE 500

/* ============================================================================
 * Tab Lifecycle
 * ============================================================================
 */

void tab_init(Tab *tab) {
  if (!tab)
    return;
  memset(tab, 0, sizeof(Tab));
  filters_init(&tab->filters);
}

void tab_free_data(Tab *tab) {
  if (!tab)
    return;

  /* Free table data */
  free(tab->table_name);
  tab->table_name = NULL;

  db_result_free(tab->data);
  tab->data = NULL;

  db_schema_free(tab->schema);
  tab->schema = NULL;

  free(tab->col_widths);
  tab->col_widths = NULL;
  tab->num_col_widths = 0;

  filters_free(&tab->filters);

  /* Free query data */
  free(tab->query_text);
  tab->query_text = NULL;

  db_result_free(tab->query_results);
  tab->query_results = NULL;

  free(tab->query_error);
  tab->query_error = NULL;

  free(tab->query_result_col_widths);
  tab->query_result_col_widths = NULL;

  free(tab->query_result_edit_buf);
  tab->query_result_edit_buf = NULL;

  free(tab->query_source_table);
  tab->query_source_table = NULL;

  db_schema_free(tab->query_source_schema);
  tab->query_source_schema = NULL;

  free(tab->query_base_sql);
  tab->query_base_sql = NULL;
}

Tab *workspace_current_tab(Workspace *ws) {
  if (!ws || ws->num_tabs == 0)
    return NULL;
  if (ws->current_tab >= ws->num_tabs)
    return NULL;
  return &ws->tabs[ws->current_tab];
}

Tab *workspace_create_table_tab(Workspace *ws, size_t connection_index,
                                size_t table_index, const char *table_name) {
  if (!ws || ws->num_tabs >= MAX_TABS || !table_name)
    return NULL;

  size_t new_idx = ws->num_tabs;
  Tab *tab = &ws->tabs[new_idx];
  tab_init(tab);

  tab->active = true;
  tab->type = TAB_TYPE_TABLE;
  tab->connection_index = connection_index;
  tab->table_index = table_index;
  tab->table_name = str_dup(table_name);
  if (!tab->table_name) {
    /* str_dup failed - reset tab and return NULL */
    memset(tab, 0, sizeof(Tab));
    return NULL;
  }

  ws->num_tabs++;
  ws->current_tab = new_idx;

  return tab;
}

Tab *workspace_create_query_tab(Workspace *ws, size_t connection_index) {
  if (!ws || ws->num_tabs >= MAX_TABS)
    return NULL;

  size_t new_idx = ws->num_tabs;
  Tab *tab = &ws->tabs[new_idx];
  tab_init(tab);

  tab->active = true;
  tab->type = TAB_TYPE_QUERY;
  tab->connection_index = connection_index;
  tab->table_name = str_dup("Query");

  /* Initialize query buffer */
  tab->query_capacity = 1024;
  tab->query_text = malloc(tab->query_capacity);
  if (tab->query_text) {
    tab->query_text[0] = '\0';
    tab->query_len = 0;
  }

  ws->num_tabs++;
  ws->current_tab = new_idx;

  return tab;
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
}

void workspace_free_data(Workspace *ws) {
  if (!ws)
    return;

  /* Free all tabs */
  for (size_t i = 0; i < ws->num_tabs; i++) {
    tab_free_data(&ws->tabs[i]);
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
  if (!app || app->num_workspaces >= MAX_WORKSPACES)
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

  /* Free connection string */
  free(conn->connstr);
  conn->connstr = NULL;

  /* Disconnect database */
  if (conn->conn) {
    db_disconnect(conn->conn);
    conn->conn = NULL;
  }

  conn->active = false;
}

Connection *app_add_connection(AppState *app, DbConnection *db_conn,
                               const char *connstr) {
  if (!app || app->num_connections >= MAX_CONNECTIONS)
    return NULL;

  size_t new_idx = app->num_connections;
  Connection *conn = &app->connections[new_idx];
  connection_init(conn);

  conn->active = true;
  conn->conn = db_conn;
  conn->connstr = str_dup(connstr);

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
      if (tab->connection_index > index) {
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
  app->page_size = DEFAULT_PAGE_SIZE;
  app->header_visible = true;
  app->status_visible = true;
}

void app_state_cleanup(AppState *app) {
  if (!app)
    return;

  /* Close all connections */
  for (size_t i = 0; i < app->num_connections; i++) {
    connection_free_data(&app->connections[i]);
  }

  /* Free all workspaces */
  for (size_t i = 0; i < app->num_workspaces; i++) {
    workspace_free_data(&app->workspaces[i]);
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
