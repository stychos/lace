/*
 * Lace
 * Core application state (platform-independent)
 *
 * Hierarchy: AppState contains both Connections (pool) and Workspaces
 * (independent) Each Tab references which Connection it uses.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_APP_STATE_H
#define LACE_APP_STATE_H

#include "../config/config.h"
#include "../db/db.h"
#include <stdbool.h>
#include <stddef.h>

/* Initial capacities for dynamic arrays */
#define INITIAL_CONNECTION_CAPACITY 4
#define INITIAL_WORKSPACE_CAPACITY 4
#define INITIAL_TAB_CAPACITY 8

/* ============================================================================
 * Filter Types
 * ============================================================================
 */

/* Filter operators */
typedef enum {
  FILTER_OP_EQ,           /* = */
  FILTER_OP_NE,           /* <> */
  FILTER_OP_GT,           /* > */
  FILTER_OP_GE,           /* >= */
  FILTER_OP_LT,           /* < */
  FILTER_OP_LE,           /* <= */
  FILTER_OP_IN,           /* IN (value list) */
  FILTER_OP_CONTAINS,     /* LIKE '%value%' */
  FILTER_OP_REGEX,        /* REGEXP/~ (driver-specific) */
  FILTER_OP_IS_EMPTY,     /* = '' */
  FILTER_OP_IS_NOT_EMPTY, /* <> '' */
  FILTER_OP_IS_NULL,      /* IS NULL */
  FILTER_OP_IS_NOT_NULL,  /* IS NOT NULL */
  FILTER_OP_RAW,          /* Raw SQL condition */
} FilterOperator;

#define FILTER_OP_COUNT 14

/* Single column filter */
typedef struct {
  size_t column_index; /* Index into schema columns */
  FilterOperator op;   /* Operator type */
  char value[256];     /* Filter value (for ops that need it) */
} ColumnFilter;

/* Table filters collection */
typedef struct {
  ColumnFilter *filters; /* Array of column filters */
  size_t num_filters;    /* Number of active filters */
  size_t filters_cap;    /* Capacity */
} TableFilters;

/* ============================================================================
 * Connection - Database connection (pool entry)
 * ============================================================================
 */

/* Connection - a database connection in the pool */
typedef struct {
  bool active;        /* Is this connection slot used */
  DbConnection *conn; /* Database connection handle */
  char *connstr;      /* Connection string (for display/reconnect) */

  /* Tables list (from this connection) */
  char **tables;
  size_t num_tables;
} Connection;

/* ============================================================================
 * Tab - Individual table view or query editor
 * ============================================================================
 */

/* Tab type */
typedef enum {
  TAB_TYPE_TABLE,      /* Table data view */
  TAB_TYPE_QUERY,      /* SQL query editor */
  TAB_TYPE_CONNECTION  /* Connection placeholder (no table loaded) */
} TabType;

/* Tab - holds per-tab state (table data or query) */
typedef struct {
  TabType type; /* Type of tab content */
  bool active;  /* Is this tab active/used */

  /* Connection reference - which connection this tab uses */
  size_t connection_index; /* Index into app->connections[] */

  /* Table identification */
  size_t table_index; /* Index into connection's tables array */
  char *table_name;   /* Table name (for display) */

  /* Table data */
  ResultSet *data;
  TableSchema *schema;

  /* View state - cursor and scroll positions */
  size_t cursor_row;
  size_t cursor_col;
  size_t scroll_row;
  size_t scroll_col;

  /* Pagination state */
  size_t total_rows;
  size_t loaded_offset;
  size_t loaded_count;
  bool row_count_approximate;   /* True if total_rows is approximate */
  size_t unfiltered_total_rows; /* Original row count before filtering */

  /* Column widths (computed for display) */
  int *col_widths;
  size_t num_col_widths;

  /* Filters (per-table) */
  TableFilters filters;

  /* Query mode fields */
  char *query_text;
  size_t query_len;
  size_t query_capacity;
  size_t query_cursor;
  size_t query_scroll_line;
  size_t query_scroll_col;
  ResultSet *query_results;
  int64_t query_affected;
  char *query_error;
  size_t query_result_row;
  size_t query_result_col;
  size_t query_result_scroll_row;
  size_t query_result_scroll_col;
  int *query_result_col_widths;
  size_t query_result_num_cols;

  /* Query results editing - source table tracking */
  char *query_source_table;
  TableSchema *query_source_schema;

  /* Query results pagination */
  char *query_base_sql;
  size_t query_total_rows;
  size_t query_loaded_offset;
  size_t query_loaded_count;
  bool query_paginated;

  /* Background pagination state */
  void *bg_load_op;             /* AsyncOperation* - current background load */
  bool bg_load_forward;         /* Direction: true=forward, false=backward */
  size_t bg_load_target_offset; /* Target offset being loaded */
} Tab;

/* ============================================================================
 * Workspace - Container for tabs
 * ============================================================================
 */

/* Workspace - container for tabs */
typedef struct {
  bool active;   /* Is this workspace active/used */
  char name[64]; /* Optional workspace name for display */

  /* Tabs (dynamic array) */
  Tab *tabs;
  size_t num_tabs;
  size_t tab_capacity;
  size_t current_tab;
} Workspace;

/* ============================================================================
 * AppState - Top-level application state
 * ============================================================================
 */

/* Core application state (platform-independent) */
typedef struct {
  /* Configuration (loaded from config.json) */
  Config *config;

  /* Global UI state */
  bool header_visible;
  bool status_visible;

  /* Application running flag (set false to exit main loop) */
  bool running;

  /* Page size for data loading */
  size_t page_size;

  /* Connection pool (dynamic array) */
  Connection *connections;
  size_t num_connections;
  size_t connection_capacity;

  /* Workspaces (dynamic array) */
  Workspace *workspaces;
  size_t num_workspaces;
  size_t workspace_capacity;
  size_t current_workspace;
} AppState;

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================
 */

/* Application state */
void app_state_init(AppState *app);
void app_state_cleanup(AppState *app);

/* Connection pool management */
void connection_init(Connection *conn);
void connection_free_data(Connection *conn);
Connection *app_add_connection(AppState *app, DbConnection *db_conn,
                               const char *connstr);
Connection *app_get_connection(AppState *app, size_t index);
bool app_close_connection(AppState *app, size_t index);
size_t app_find_connection_index(AppState *app, DbConnection *conn);

/* Workspace management */
void workspace_init(Workspace *ws);
void workspace_free_data(Workspace *ws);
Workspace *app_current_workspace(AppState *app);
Workspace *app_create_workspace(AppState *app);
bool app_close_workspace(AppState *app, size_t index);
Workspace *app_switch_workspace(AppState *app, size_t index);

/* Tab management */
void tab_init(Tab *tab);
void tab_free_data(Tab *tab);
Tab *workspace_current_tab(Workspace *ws);
Tab *workspace_create_table_tab(Workspace *ws, size_t connection_index,
                                size_t table_index, const char *table_name);
Tab *workspace_create_query_tab(Workspace *ws, size_t connection_index);
Tab *workspace_create_connection_tab(Workspace *ws, size_t connection_index,
                                     const char *connstr);
bool workspace_close_tab(Workspace *ws, size_t index);
Tab *workspace_switch_tab(Workspace *ws, size_t index);

/* ============================================================================
 * Convenience Accessors
 * ============================================================================
 */

/* Get current tab from app */
Tab *app_current_tab(AppState *app);

/* Get connection for a specific tab */
Connection *app_get_tab_connection(AppState *app, Tab *tab);

/* Get connection for current tab */
Connection *app_current_tab_connection(AppState *app);

/* ============================================================================
 * Filter Operations (platform-independent)
 * ============================================================================
 */

void filters_init(TableFilters *f);
void filters_free(TableFilters *f);
void filters_clear(TableFilters *f);
bool filters_add(TableFilters *f, size_t col_idx, FilterOperator op,
                 const char *value);
void filters_remove(TableFilters *f, size_t index);
const char *filter_op_name(FilterOperator op);
const char *filter_op_sql(FilterOperator op);
bool filter_op_needs_value(FilterOperator op);
char *filters_parse_in_values(const char *input, char **err);
char *filters_build_where(TableFilters *f, TableSchema *schema,
                          const char *driver_name, char **err);

#endif /* LACE_APP_STATE_H */
