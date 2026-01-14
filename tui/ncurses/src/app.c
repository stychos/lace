/*
 * Lace ncurses frontend
 * Application state management implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Default page size for data loading */
#define PAGE_SIZE 500

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

static char *safe_strdup(const char *s) {
  return s ? strdup(s) : NULL;
}

static void tab_free(Tab *tab) {
  if (!tab) return;

  free(tab->title);
  free(tab->table_name);
  free(tab->query_text);

  if (tab->data) {
    lace_result_free(tab->data);
  }
  if (tab->schema) {
    lace_schema_free(tab->schema);
  }
  if (tab->filters) {
    for (size_t i = 0; i < tab->num_filters; i++) {
      lace_filter_free(&tab->filters[i]);
    }
    free(tab->filters);
  }
  free(tab->sorts);

  memset(tab, 0, sizeof(Tab));
}

static void connection_free(Connection *conn) {
  if (!conn) return;

  free(conn->connstr);
  free(conn->display_name);
  if (conn->tables) {
    lace_tables_free(conn->tables, conn->num_tables);
  }

  memset(conn, 0, sizeof(Connection));
}

/* ==========================================================================
 * Application Lifecycle
 * ========================================================================== */

AppState *app_create(void) {
  AppState *app = calloc(1, sizeof(AppState));
  if (!app) {
    return NULL;
  }

  /* Create client (spawns daemon) */
  app->client = lace_client_create(NULL);
  if (!app->client || !lace_client_connected(app->client)) {
    fprintf(stderr, "Failed to connect to daemon: %s\n",
            app->client ? lace_client_error(app->client) : "NULL");
    app_destroy(app);
    return NULL;
  }

  /* Initialize state */
  app->sidebar_visible = true;
  app->active_connection = -1;
  app->running = true;
  app->needs_redraw = true;

  return app;
}

void app_destroy(AppState *app) {
  if (!app) return;

  /* Close all tabs */
  if (app->tabs) {
    for (size_t i = 0; i < app->num_tabs; i++) {
      tab_free(&app->tabs[i]);
    }
    free(app->tabs);
  }

  /* Close all connections */
  if (app->connections) {
    for (size_t i = 0; i < app->num_connections; i++) {
      if (app->client && app->connections[i].conn_id > 0) {
        lace_disconnect(app->client, app->connections[i].conn_id);
      }
      connection_free(&app->connections[i]);
    }
    free(app->connections);
  }

  /* Free other state */
  free(app->sidebar_filter);
  free(app->status_message);

  /* Destroy client (terminates daemon) */
  if (app->client) {
    lace_client_destroy(app->client);
  }

  free(app);
}

/* ==========================================================================
 * Connection Management
 * ========================================================================== */

int app_connect(AppState *app, const char *connstr, const char *password) {
  if (!app || !app->client || !connstr) {
    return -1;
  }

  /* Connect via daemon */
  int conn_id = 0;
  int err = lace_connect(app->client, connstr, password, &conn_id);
  if (err != LACE_OK) {
    app_set_error(app, lace_client_error(app->client));
    return -1;
  }

  /* Grow connection array if needed */
  if (app->num_connections >= app->connection_capacity) {
    size_t new_cap = app->connection_capacity == 0 ? 4 : app->connection_capacity * 2;
    Connection *new_conns = realloc(app->connections, new_cap * sizeof(Connection));
    if (!new_conns) {
      lace_disconnect(app->client, conn_id);
      app_set_error(app, "Out of memory");
      return -1;
    }
    app->connections = new_conns;
    app->connection_capacity = new_cap;
  }

  /* Add connection */
  int idx = (int)app->num_connections;
  Connection *conn = &app->connections[idx];
  memset(conn, 0, sizeof(Connection));

  conn->conn_id = conn_id;
  conn->connstr = safe_strdup(connstr);

  /* Extract display name from connection string */
  const char *name = strrchr(connstr, '/');
  if (name) {
    conn->display_name = safe_strdup(name + 1);
  } else {
    conn->display_name = safe_strdup(connstr);
  }

  app->num_connections++;
  app->active_connection = idx;

  /* Load table list */
  app_refresh_tables(app, idx);

  app_set_status(app, "Connected");
  app->needs_redraw = true;

  return idx;
}

void app_disconnect(AppState *app, int index) {
  if (!app || index < 0 || (size_t)index >= app->num_connections) {
    return;
  }

  Connection *conn = &app->connections[index];

  /* Close tabs for this connection */
  for (size_t i = app->num_tabs; i > 0; i--) {
    if (app->tabs[i-1].conn_id == conn->conn_id) {
      app_close_tab(app, i-1);
    }
  }

  /* Disconnect */
  if (app->client && conn->conn_id > 0) {
    lace_disconnect(app->client, conn->conn_id);
  }
  connection_free(conn);

  /* Remove from array */
  for (size_t i = (size_t)index; i < app->num_connections - 1; i++) {
    app->connections[i] = app->connections[i + 1];
  }
  app->num_connections--;

  /* Update active connection */
  if (app->active_connection == index) {
    app->active_connection = app->num_connections > 0 ? 0 : -1;
  } else if (app->active_connection > index) {
    app->active_connection--;
  }

  app->needs_redraw = true;
}

bool app_refresh_tables(AppState *app, int index) {
  if (!app || !app->client || index < 0 || (size_t)index >= app->num_connections) {
    return false;
  }

  Connection *conn = &app->connections[index];

  /* Free old table list */
  if (conn->tables) {
    lace_tables_free(conn->tables, conn->num_tables);
    conn->tables = NULL;
    conn->num_tables = 0;
  }

  /* Get new table list */
  int err = lace_list_tables(app->client, conn->conn_id, &conn->tables, &conn->num_tables);
  if (err != LACE_OK) {
    app_set_error(app, lace_client_error(app->client));
    return false;
  }

  app->needs_redraw = true;
  return true;
}

/* ==========================================================================
 * Tab Management
 * ========================================================================== */

static int add_tab(AppState *app) {
  /* Grow tab array if needed */
  if (app->num_tabs >= app->tab_capacity) {
    size_t new_cap = app->tab_capacity == 0 ? 8 : app->tab_capacity * 2;
    Tab *new_tabs = realloc(app->tabs, new_cap * sizeof(Tab));
    if (!new_tabs) {
      app_set_error(app, "Out of memory");
      return -1;
    }
    app->tabs = new_tabs;
    app->tab_capacity = new_cap;
  }

  int idx = (int)app->num_tabs;
  memset(&app->tabs[idx], 0, sizeof(Tab));
  app->num_tabs++;

  return idx;
}

int app_open_table(AppState *app, int conn_idx, const char *table) {
  if (!app || !app->client || conn_idx < 0 ||
      (size_t)conn_idx >= app->num_connections || !table) {
    return -1;
  }

  Connection *conn = &app->connections[conn_idx];

  /* Check if tab already exists */
  for (size_t i = 0; i < app->num_tabs; i++) {
    if (app->tabs[i].type == TAB_TYPE_TABLE &&
        app->tabs[i].conn_id == conn->conn_id &&
        app->tabs[i].table_name &&
        strcmp(app->tabs[i].table_name, table) == 0) {
      app_switch_tab(app, i);
      return (int)i;
    }
  }

  /* Create new tab */
  int idx = add_tab(app);
  if (idx < 0) {
    return -1;
  }

  Tab *tab = &app->tabs[idx];
  tab->type = TAB_TYPE_TABLE;
  tab->conn_id = conn->conn_id;
  tab->table_name = safe_strdup(table);
  tab->title = safe_strdup(table);
  tab->needs_refresh = true;

  /* Load schema */
  int err = lace_get_schema(app->client, conn->conn_id, table, &tab->schema);
  if (err != LACE_OK) {
    app_set_error(app, lace_client_error(app->client));
  }

  /* Load initial data */
  app->active_tab = (size_t)idx;
  app_refresh_data(app);

  app->needs_redraw = true;
  return idx;
}

int app_open_query_tab(AppState *app, int conn_idx) {
  if (!app || conn_idx < 0 || (size_t)conn_idx >= app->num_connections) {
    return -1;
  }

  Connection *conn = &app->connections[conn_idx];

  int idx = add_tab(app);
  if (idx < 0) {
    return -1;
  }

  Tab *tab = &app->tabs[idx];
  tab->type = TAB_TYPE_QUERY;
  tab->conn_id = conn->conn_id;
  tab->title = safe_strdup("Query");
  tab->query_text = safe_strdup("");

  app->active_tab = (size_t)idx;
  app->needs_redraw = true;

  return idx;
}

void app_close_tab(AppState *app, size_t index) {
  if (!app || index >= app->num_tabs) {
    return;
  }

  tab_free(&app->tabs[index]);

  /* Remove from array */
  for (size_t i = index; i < app->num_tabs - 1; i++) {
    app->tabs[i] = app->tabs[i + 1];
  }
  app->num_tabs--;

  /* Update active tab */
  if (app->num_tabs == 0) {
    app->active_tab = 0;
  } else if (app->active_tab >= app->num_tabs) {
    app->active_tab = app->num_tabs - 1;
  } else if (app->active_tab > index) {
    app->active_tab--;
  }

  app->needs_redraw = true;
}

void app_switch_tab(AppState *app, size_t index) {
  if (!app || index >= app->num_tabs) {
    return;
  }

  app->active_tab = index;
  app->needs_redraw = true;
}

/* ==========================================================================
 * Data Operations
 * ========================================================================== */

bool app_refresh_data(AppState *app) {
  Tab *tab = app_current_tab(app);
  if (!tab || tab->type != TAB_TYPE_TABLE || !tab->table_name) {
    return false;
  }

  /* Free old data */
  if (tab->data) {
    lace_result_free(tab->data);
    tab->data = NULL;
  }

  /* Get row count */
  bool approximate = false;
  lace_count(app->client, tab->conn_id, tab->table_name,
             tab->filters, tab->num_filters, &tab->total_rows, &approximate);

  /* Load data */
  int err = lace_query(app->client, tab->conn_id, tab->table_name,
                       tab->filters, tab->num_filters,
                       tab->sorts, tab->num_sorts,
                       tab->data_offset, PAGE_SIZE, &tab->data);

  if (err != LACE_OK) {
    app_set_error(app, lace_client_error(app->client));
    return false;
  }

  tab->needs_refresh = false;
  app->needs_redraw = true;

  char msg[128];
  snprintf(msg, sizeof(msg), "Loaded %zu rows (total: %zu)",
           tab->data ? tab->data->num_rows : 0, tab->total_rows);
  app_set_status(app, msg);

  return true;
}

bool app_load_more(AppState *app, bool forward) {
  Tab *tab = app_current_tab(app);
  if (!tab || tab->type != TAB_TYPE_TABLE || !tab->table_name) {
    return false;
  }

  if (forward) {
    /* Check if more data available */
    if (tab->data_offset + (tab->data ? tab->data->num_rows : 0) >= tab->total_rows) {
      return false; /* Already at end */
    }
    tab->data_offset += PAGE_SIZE;
  } else {
    /* Check if can go back */
    if (tab->data_offset == 0) {
      return false; /* Already at start */
    }
    if (tab->data_offset >= PAGE_SIZE) {
      tab->data_offset -= PAGE_SIZE;
    } else {
      tab->data_offset = 0;
    }
  }

  /* Reset cursor */
  tab->cursor_row = 0;
  tab->scroll_row = 0;

  return app_refresh_data(app);
}

/* ==========================================================================
 * Status Messages
 * ========================================================================== */

void app_set_status(AppState *app, const char *message) {
  if (!app) return;

  free(app->status_message);
  app->status_message = safe_strdup(message);
  app->status_is_error = false;
  app->status_time = time(NULL);
  app->needs_redraw = true;
}

void app_set_error(AppState *app, const char *message) {
  if (!app) return;

  free(app->status_message);
  app->status_message = safe_strdup(message);
  app->status_is_error = true;
  app->status_time = time(NULL);
  app->needs_redraw = true;
}

void app_clear_status(AppState *app) {
  if (!app) return;

  free(app->status_message);
  app->status_message = NULL;
  app->needs_redraw = true;
}

/* ==========================================================================
 * Accessors
 * ========================================================================== */

Tab *app_current_tab(AppState *app) {
  if (!app || app->num_tabs == 0 || app->active_tab >= app->num_tabs) {
    return NULL;
  }
  return &app->tabs[app->active_tab];
}

Connection *app_current_connection(AppState *app) {
  if (!app || app->active_connection < 0 ||
      (size_t)app->active_connection >= app->num_connections) {
    return NULL;
  }
  return &app->connections[app->active_connection];
}
