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
#include "../db_compat.h"
#include "constants.h"
#include "history.h"
#include "undo_stack.h"
#include <stdbool.h>
#include <stddef.h>

/* Initial capacities and limits are in constants.h:
 * INITIAL_CONNECTION_CAPACITY, INITIAL_WORKSPACE_CAPACITY, INITIAL_TAB_CAPACITY,
 * MAX_SORT_COLUMNS */

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
  FILTER_OP_BETWEEN,      /* BETWEEN value AND value2 */
  FILTER_OP_IS_EMPTY,     /* = '' */
  FILTER_OP_IS_NOT_EMPTY, /* <> '' */
  FILTER_OP_IS_NULL,      /* IS NULL */
  FILTER_OP_IS_NOT_NULL,  /* IS NOT NULL */
  FILTER_OP_RAW,          /* Raw SQL condition */
  /* Appended after RAW: session.json version 1 stores the integer */
  FILTER_OP_STARTS,       /* LIKE 'value%' */
  FILTER_OP_ENDS,         /* LIKE '%value' */
} FilterOperator;

#define FILTER_OP_COUNT 17

/* Single column filter */
typedef struct {
  size_t column_index; /* Index into schema columns */
  FilterOperator op;   /* Operator type */
  char value[256];     /* Filter value (for ops that need it) */
  char value2[256];    /* Second value (for BETWEEN) */
} ColumnFilter;

/* Table filters collection */
typedef struct {
  ColumnFilter *filters; /* Array of column filters */
  size_t num_filters;    /* Number of active filters */
  size_t filters_cap;    /* Capacity */
} TableFilters;

/* ============================================================================
 * Sort Direction
 * ============================================================================
 */

typedef enum {
  SORT_NONE, /* No sorting (default order) */
  SORT_ASC,  /* Ascending */
  SORT_DESC  /* Descending */
} SortDirection;

/* Single sort entry for multi-column sorting */
typedef struct {
  size_t column;           /* Column index */
  SortDirection direction; /* Sort direction */
} SortEntry;

/* MAX_SORT_COLUMNS is in constants.h */

/* ============================================================================
 * Connection - Database connection (pool entry)
 * ============================================================================
 */

/* Connection - a database connection in the pool */
typedef struct {
  bool active;         /* Is this connection slot used */
  DbConnection *conn;  /* Database connection handle (wrapper for liblace) */
  char *connstr;       /* Connection string (for display/reconnect) */
  char *saved_conn_id; /* UUID of saved connection (for session persistence) */
  char *database;      /* the server's current database, for the header (NULL: SQLite) */

  /* Tables list (from this connection) */
  char **tables;
  size_t num_tables;
  /* Views, routines and triggers (lace_list_objects), with the tables */
  LaceObject *objects;
  size_t num_objects;

  /* Query history for this connection */
  QueryHistory *history;
} Connection;

/* ============================================================================
 * Tab - Individual table view or query editor
 * ============================================================================
 */

/* Tab type */
typedef enum {
  TAB_TYPE_TABLE,     /* Table data view */
  TAB_TYPE_QUERY,     /* SQL query editor */
  TAB_TYPE_CONNECTION, /* Connected, no table loaded: the sidebar to pick one */
  TAB_TYPE_FORM        /* A connection form: not connected (views/form_tab.c) */
} TabType;

/* A form's connection_index: it has none */
#define TAB_NO_CONNECTION SIZE_MAX

struct FormTab; /* the form's UI state (views/form_tab.c) */

/* The sub-views of a table tab (core/catalog_rows.c builds their rows): a
 * table has all five; a database view Data, Definition (the Structure slot)
 * and Information; a routine or trigger Definition and Information */
typedef enum { SUBVIEW_DATA, SUBVIEW_STRUCTURE, SUBVIEW_INDEXES, SUBVIEW_FOREIGN_KEYS, SUBVIEW_INFORMATION,
               SUBVIEW_COUNT } TabSubView;

/* Tab - holds per-tab state (table data or query) */
typedef struct {
  TabType type; /* Type of tab content */
  bool active;  /* Is this tab active/used */
  bool preview; /* the sidebar's single click shows tables here, replacing each other;
                 * a sort, a filter or an edit keeps it (runtime only) */
  /* A TABLE tab shows a table, or a database view, routine or trigger (as
   * lace_list_objects names them; a trigger's table, a routine's kind) */
  LaceObjectKind object;
  char *object_table;
  char *object_detail;
  TabSubView subview;                     /* saved in the session */
  size_t sv_cursor[SUBVIEW_COUNT], sv_scroll[SUBVIEW_COUNT]; /* the lists' positions */
  /* What the sub-views besides Data show, read when first shown (subviews.c);
   * a refresh frees them */
  LaceTableInfo *info;
  LaceAutoInc *autoinc;
  char *info_error;
  int64_t info_count; /* the exact count asked for from Information, -1 unknown */
  LaceDependent *deps;
  size_t num_deps;
  bool deps_loaded;
  char *deps_error;
  char *definition;
  char *definition_error;

  /* Connection reference - which connection this tab uses */
  size_t connection_index; /* Index into app->connections[]; TAB_NO_CONNECTION for a form */

  /* A form tab's state, made by the UI when first drawn; freed through
   * tab_form_free_hook, so it moves with the tab when tabs[] shifts */
  struct FormTab *form;

  /* Table identification */
  char *table_name;   /* Table name (for display) */
  char *table_error;  /* Error message if table failed to load */

  /* Table data */
  ResultSet *data;
  TableSchema *schema;

  /* View state. Tab is the single source of truth: the table viewmodel and
   * the draw code read these through the current tab, never a copy. */
  size_t cursor_row; /* Within the loaded window */
  size_t cursor_col;
  size_t scroll_row;
  size_t scroll_col;

  /* Pagination: a sliding window of rows [loaded_offset, +loaded_count) */
  size_t total_rows;
  size_t loaded_offset;
  size_t loaded_count;
  bool row_count_approximate;   /* total_rows is a catalog estimate */
  size_t unfiltered_total_rows; /* Original row count before filtering */

  /* Column widths restored from the session; the viewmodel measures its own */
  int *col_widths;
  size_t num_col_widths;

  /* Filters (per-table) */
  TableFilters filters;

  /* Sort state (per-table) - multi-column sorting */
  SortEntry sort_entries[MAX_SORT_COLUMNS]; /* Sort columns in priority order */
  int *user_widths;       /* column widths set by column_narrower/wider (0: measured) */
  int query_editor_rows;  /* a query tab's editor height set by split_up/down (0: a third) */
  size_t num_user_widths;
  size_t num_sort_entries;                  /* Number of active sort columns */

  /* Query tab: editor text, results and their view state */
  char *query_text;
  size_t query_len;
  size_t query_capacity;
  size_t query_cursor; /* Byte offset in query_text */
  size_t query_scroll_line;
  size_t query_scroll_col;
  ResultSet *query_results;
  int64_t query_affected;     /* Affected rows of the last non-SELECT */
  bool query_exec_success;    /* True if last exec (non-SELECT) succeeded */
  char *query_error;
  size_t query_result_row;    /* Results cursor, within the loaded window */
  size_t query_result_col;
  size_t query_result_scroll_row;
  size_t query_result_scroll_col;
  int *query_result_col_widths;
  size_t query_result_num_cols;

  /* Query results editing - source table tracking */
  char *query_source_table;
  TableSchema *query_source_schema;

  /* Query results pagination. query_base_sql has no trailing ';'/comment;
   * pages are base + LIMIT/OFFSET, or a subquery wrap of it when the
   * statement has its own top-level LIMIT (query_page_wrap). */
  char *query_base_sql;
  bool query_page_wrap;
  bool query_total_known;   /* query_total_rows is exact (count or end seen) */
  void *query_count_op;     /* Background COUNT(*) (AsyncOperation*) or NULL */
  size_t query_total_rows;
  size_t query_loaded_offset;
  size_t query_loaded_count;
  bool query_paginated;

  /* Data generation: bumped by tab_data_replaced() whenever tab->data is
   * replaced wholesale (table switch, sort, filter, refresh, jump). A
   * background page load remembers the generation it started under and is
   * discarded if it changed, so rows from an older view are never merged. */
  uint64_t load_generation;
  uint64_t bg_load_generation;

  /* A loaded row's key or sort column was edited: the window's edge rows
   * no longer say where the window ends in the table's order, so the next
   * neighbour page uses OFFSET instead of keyset. Cleared with the data. */
  bool keyset_edge_stale;

  /* Unique for the life of the process (tabs[] shifts, so indices and
   * addresses do not identify a tab). Assigned by tab_init. */
  uint64_t uid;

  /* Background pagination state */
  void *bg_load_op;             /* AsyncOperation* - current background load */
  bool bg_load_forward;         /* Direction: true=forward, false=backward */
  size_t bg_load_target_offset; /* Target offset being loaded */

  /* Row selection (for bulk operations) */
  size_t *selected_rows;    /* Array of selected global row indices */
  size_t num_selected;      /* Number of selected rows */
  size_t selected_capacity; /* Capacity of selected_rows array */

  /* Data change tracking */
  bool needs_refresh; /* True if data was modified in another tab */

  /* The writes that succeeded on this tab's table, newest last, for `undo`
   * (undo.c; edit.c records them). Runtime only; cleared when the tab shows
   * another table and when it closes. */
  UndoStack undo;
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

  /* liblace client (communicates with daemon) */
  lace_client_t *client;

  /* Global UI state */
  bool header_visible;
  bool status_visible;

  /* Application running flag (set false to exit main loop) */
  bool running;

  /* Page size for data loading */

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
/* The tabs, in every workspace, that use the connection (forms never) */
size_t app_connection_users(const AppState *app, size_t conn_idx);
/* An open connection to the same database: with a saved id the one opened
 * from it (and, when connstr is given, still pointing where it does: a saved
 * connection edited since is connected anew); else one with the same
 * password-less string and no saved id. SIZE_MAX when none. */
size_t app_find_open_connection(AppState *app, const char *saved_id, const char *connstr);

/* Add a statement to the query history of the connection that ran it
 * (type is a HistoryEntryType). Main thread only. */
void app_record_history(AppState *app, DbConnection *db_conn, const char *sql,
                        int type);

/* Workspace management */
void workspace_init(Workspace *ws);
void workspace_free_data(Workspace *ws);
Workspace *app_current_workspace(AppState *app);
Workspace *app_create_workspace(AppState *app);
bool app_close_workspace(AppState *app, size_t index);
Workspace *app_switch_workspace(AppState *app, size_t index);

/* Tab management */
void tab_init(Tab *tab);

/* Stop and free a query tab's background row count, if any */
void query_count_stop(Tab *tab);
void tab_free_data(Tab *tab);
Tab *workspace_current_tab(Workspace *ws);
Tab *workspace_create_table_tab(Workspace *ws, size_t connection_index,
                                const char *table_name);

/* Position of a table tab's table in its connection's table list, found by
 * name (the list can be reloaded or reordered); SIZE_MAX if it is not
 * there (dropped, or not a table tab) */
size_t app_tab_table_position(AppState *app, const Tab *tab);
Tab *workspace_create_query_tab(Workspace *ws, size_t connection_index);
Tab *workspace_create_connection_tab(Workspace *ws, size_t connection_index,
                                     const char *connstr);
bool workspace_close_tab(Workspace *ws, size_t index);
/* A connection form at position at (clamped), made current; NULL on failure */
Tab *workspace_insert_form_tab(Workspace *ws, size_t at);
/* The tab's data freed and the tab its connection's tab with no table open,
 * named after the database (uid kept) */
void tab_make_connection(Tab *tab, size_t connection_index, const char *connstr);
/* The tab's data freed and the tab a form in its place (uid kept) */
void tab_make_form(Tab *tab);
/* Frees a tab's form state (installed by the UI) */
extern void (*tab_form_free_hook)(struct FormTab *form);
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
char *filters_parse_in_values(const char *input, const char *driver_name,
                              char **err);
/* "col1 ASC, col2 DESC" for the sort entries, quoted for the driver; NULL
 * when no entry names a column of the schema. Caller frees. */
char *filters_build_order_by(const SortEntry *entries, size_t num_entries,
                             const TableSchema *schema,
                             const char *driver_name);
char *filters_build_where(TableFilters *f, TableSchema *schema,
                          const char *driver_name, char **err);

/* The keyset ordering of a table tab (its sort columns, then the primary
 * key), or NULL to page with OFFSET: keyset_paging off, no schema or
 * primary key, or a column that cannot be a keyset boundary. Free with
 * lace_keyset_free(). */
LaceKeysetState *tab_page_keyset(const Tab *tab, const char *driver_name,
                                 bool keyset_paging);

/* ORDER BY body for every page of a table tab (NULL: none). With keyset
 * paging the primary key follows the sort columns, so OFFSET pages and
 * keyset pages agree on every row's position. Every page query of a table
 * tab, session restore included, must use this. Caller frees. */
char *tab_page_order_by(const Tab *tab, const char *driver_name,
                        bool keyset_paging);

/* ============================================================================
 * Row Selection Operations
 * ============================================================================
 */

/* Toggle selection of a row by global index */
bool tab_toggle_selection(Tab *tab, size_t global_row);

/* Check if a row is selected */
bool tab_is_row_selected(Tab *tab, size_t global_row);

/* Clear all selections */
void tab_clear_selections(Tab *tab);

/* ============================================================================
 * Data Change Tracking
 * ============================================================================
 */

/* Mark all tabs with the same table as needing refresh (except current tab) */
void app_mark_table_tabs_dirty(AppState *app, size_t connection_index,
                               const char *table_name, Tab *exclude_tab);

/* Call whenever tab->data is replaced (not merged): invalidates in-flight
 * background page loads for this tab and clears row selections, which are
 * stored as row positions and would otherwise point at different rows
 * after a sort, filter, refresh or table switch (and `x` would delete
 * those). */
/* Whether the tab's object has that sub-view */
bool tab_has_subview(const Tab *tab, TabSubView sv);

/* A schema column's sort on the tab: off, ascending, descending, off
 * again (a column added last while there is room); false when nothing
 * changed. The caller reloads. */
bool tab_cycle_sort(Tab *tab, size_t col);

/* Column widths the user set (column_narrower/wider), kept over the
 * measured ones until the tab shows other columns */
void tab_set_user_width(Tab *tab, size_t col, int width);
void tab_clear_user_widths(Tab *tab);
void tab_apply_user_widths(const Tab *tab, int *widths, size_t n);
/* Drop what the sub-views read (info, dependents, definition): a refresh */
void tab_drop_catalog(Tab *tab);
/* The session's names: "data", "structure", ...; "definition" for an object
 * whose Structure slot is its SQL; "table", "view", "routine", "trigger" */
const char *tab_subview_key(const Tab *tab, TabSubView sv);
bool tab_subview_from_key(const char *key, TabSubView *out);
const char *tab_object_key(LaceObjectKind kind);
bool tab_object_from_key(const char *key, LaceObjectKind *out);

static inline void tab_data_replaced(Tab *tab) {
  if (tab) {
    tab->load_generation++;
    tab->keyset_edge_stale = false;
    tab_clear_selections(tab);
  }
}

#endif /* LACE_APP_STATE_H */
