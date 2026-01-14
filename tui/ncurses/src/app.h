/*
 * Lace ncurses frontend
 * Application state management
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FRONTEND_APP_H
#define LACE_FRONTEND_APP_H

#include <lace.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* ==========================================================================
 * Tab Types
 * ========================================================================== */

typedef enum {
  TAB_TYPE_TABLE,      /* Table data view */
  TAB_TYPE_QUERY,      /* SQL query editor */
  TAB_TYPE_CONNECTION  /* Connection placeholder (no table loaded) */
} TabType;

/* ==========================================================================
 * Tab State
 * ========================================================================== */

typedef struct {
  TabType type;
  char *title;           /* Tab display title */

  /* Connection info */
  int conn_id;           /* Connection ID in laced */
  char *table_name;      /* Current table (NULL for query tab) */

  /* Data state */
  LaceResult *data;      /* Current data (owned) */
  LaceSchema *schema;    /* Table schema (owned) */
  size_t total_rows;     /* Total rows in table */

  /* View state (UI owns this) */
  size_t cursor_row;     /* Current row (in data) */
  size_t cursor_col;     /* Current column */
  size_t scroll_row;     /* First visible row offset in data */
  size_t scroll_col;     /* First visible column */
  size_t data_offset;    /* Offset of loaded data in full table */

  /* Filters and sorts */
  LaceFilter *filters;
  size_t num_filters;
  LaceSort *sorts;
  size_t num_sorts;

  /* Query tab specific */
  char *query_text;      /* SQL query text */
  size_t query_cursor;   /* Cursor position in query */

  /* Flags */
  bool needs_refresh;    /* Data needs to be reloaded */
  bool is_modified;      /* Has unsaved changes */
} Tab;

/* ==========================================================================
 * Connection State
 * ========================================================================== */

typedef struct {
  int conn_id;           /* Connection ID in laced */
  char *connstr;         /* Connection string */
  char *display_name;    /* Display name for connection */
  char **tables;         /* List of tables */
  size_t num_tables;     /* Number of tables */
} Connection;

/* ==========================================================================
 * Application State
 * ========================================================================== */

typedef struct {
  /* Client connection to daemon */
  lace_client_t *client;

  /* Connections */
  Connection *connections;
  size_t num_connections;
  size_t connection_capacity;
  int active_connection;  /* Index of active connection, -1 if none */

  /* Tabs */
  Tab *tabs;
  size_t num_tabs;
  size_t tab_capacity;
  size_t active_tab;      /* Index of active tab */

  /* UI State */
  bool sidebar_visible;
  size_t sidebar_scroll;
  size_t sidebar_selected;
  char *sidebar_filter;   /* Table name filter */

  /* Status */
  char *status_message;
  bool status_is_error;
  time_t status_time;

  /* Application flags */
  bool running;
  bool needs_redraw;
} AppState;

/* ==========================================================================
 * Application Lifecycle
 * ========================================================================== */

/*
 * Create a new application state.
 * Spawns the laced daemon.
 *
 * @return  AppState pointer, or NULL on failure
 */
AppState *app_create(void);

/*
 * Destroy application state.
 * Closes all connections and terminates daemon.
 *
 * @param app  Application state (NULL is safe)
 */
void app_destroy(AppState *app);

/* ==========================================================================
 * Connection Management
 * ========================================================================== */

/*
 * Open a database connection.
 *
 * @param app       Application state
 * @param connstr   Connection string
 * @param password  Password (NULL if not needed)
 * @return          Connection index, or -1 on failure
 */
int app_connect(AppState *app, const char *connstr, const char *password);

/*
 * Close a database connection.
 *
 * @param app    Application state
 * @param index  Connection index
 */
void app_disconnect(AppState *app, int index);

/*
 * Refresh table list for a connection.
 *
 * @param app    Application state
 * @param index  Connection index
 * @return       true on success
 */
bool app_refresh_tables(AppState *app, int index);

/* ==========================================================================
 * Tab Management
 * ========================================================================== */

/*
 * Create a new table tab.
 *
 * @param app        Application state
 * @param conn_idx   Connection index
 * @param table      Table name
 * @return           Tab index, or -1 on failure
 */
int app_open_table(AppState *app, int conn_idx, const char *table);

/*
 * Create a new query tab.
 *
 * @param app       Application state
 * @param conn_idx  Connection index
 * @return          Tab index, or -1 on failure
 */
int app_open_query_tab(AppState *app, int conn_idx);

/*
 * Close a tab.
 *
 * @param app    Application state
 * @param index  Tab index
 */
void app_close_tab(AppState *app, size_t index);

/*
 * Switch to a tab.
 *
 * @param app    Application state
 * @param index  Tab index
 */
void app_switch_tab(AppState *app, size_t index);

/* ==========================================================================
 * Data Operations
 * ========================================================================== */

/*
 * Load/refresh data for current tab.
 *
 * @param app  Application state
 * @return     true on success
 */
bool app_refresh_data(AppState *app);

/*
 * Load more data (pagination).
 *
 * @param app       Application state
 * @param forward   true for next page, false for previous
 * @return          true on success
 */
bool app_load_more(AppState *app, bool forward);

/* ==========================================================================
 * Status Messages
 * ========================================================================== */

/*
 * Set status message.
 *
 * @param app      Application state
 * @param message  Message text
 */
void app_set_status(AppState *app, const char *message);

/*
 * Set error status message.
 *
 * @param app      Application state
 * @param message  Error message
 */
void app_set_error(AppState *app, const char *message);

/*
 * Clear status message.
 *
 * @param app  Application state
 */
void app_clear_status(AppState *app);

/* ==========================================================================
 * Accessors
 * ========================================================================== */

/* Get current tab (NULL if no tabs) */
Tab *app_current_tab(AppState *app);

/* Get current connection (NULL if no connection) */
Connection *app_current_connection(AppState *app);

#endif /* LACE_FRONTEND_APP_H */
