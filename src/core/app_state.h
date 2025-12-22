/*
 * lace - Database Viewer and Manager
 * Core application state (platform-independent)
 */

#ifndef LACE_APP_STATE_H
#define LACE_APP_STATE_H

#include "../db/db.h"
#include <stdbool.h>
#include <stddef.h>

/* Maximum number of workspaces/tabs */
#define MAX_WORKSPACES 10

/* Workspace type */
typedef enum {
  WORKSPACE_TYPE_TABLE, /* Table data view */
  WORKSPACE_TYPE_QUERY  /* SQL query editor */
} WorkspaceType;

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

/* Workspace - holds per-tab state (both data and view state for now) */
typedef struct {
  WorkspaceType type; /* Type of workspace content */
  bool active;        /* Is this workspace active/used */

  /* Table identification */
  size_t table_index; /* Index into tables array */
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
  bool row_count_approximate; /* True if total_rows is approximate */

  /* Column widths (computed for display) */
  int *col_widths;
  size_t num_col_widths;

  /* Filters (per-table) */
  TableFilters filters;

  /* Filter panel UI state */
  bool filters_visible;
  bool filters_focused;
  size_t filters_cursor_row;
  size_t filters_cursor_col;
  size_t filters_scroll;

  /* Sidebar state (per-workspace) */
  bool sidebar_visible;
  bool sidebar_focused;
  size_t sidebar_highlight;
  size_t sidebar_scroll;
  char sidebar_filter[64];
  size_t sidebar_filter_len;

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
  bool query_focus_results;
  size_t query_result_row;
  size_t query_result_col;
  size_t query_result_scroll_row;
  size_t query_result_scroll_col;
  int *query_result_col_widths;
  size_t query_result_num_cols;

  /* Query results editing */
  bool query_result_editing;
  char *query_result_edit_buf;
  size_t query_result_edit_pos;
  char *query_source_table;
  TableSchema *query_source_schema;

  /* Query results pagination */
  char *query_base_sql;
  size_t query_total_rows;
  size_t query_loaded_offset;
  size_t query_loaded_count;
  bool query_paginated;
} Workspace;

/* Core application state (platform-independent) */
typedef struct {
  /* Database connection */
  DbConnection *conn;

  /* Table list */
  char **tables;
  size_t num_tables;

  /* Workspaces/Tabs */
  Workspace workspaces[MAX_WORKSPACES];
  size_t num_workspaces;
  size_t current_workspace;

  /* Page size for data loading */
  size_t page_size;
} AppState;

/* Initialize application state */
void app_state_init(AppState *app);

/* Cleanup application state */
void app_state_cleanup(AppState *app);

/* Get current workspace (convenience) */
Workspace *app_current_workspace(AppState *app);

/* Filter operations (platform-independent) */
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
