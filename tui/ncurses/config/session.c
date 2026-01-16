/*
 * Lace
 * Session Persistence - Save/Restore workspaces and tabs
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "session.h"
#include "../core/history.h"
#include "../util/connstr.h"
#include "../platform/platform.h"
#include "../tui.h"
#include "../util/json_helpers.h"
#include "../util/mem.h"
#include "../util/str.h"
#include "connections.h"
#include <cJSON.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef LACE_OS_WINDOWS
#include <fcntl.h>
#include <unistd.h>
#endif

/* ============================================================================
 * Password Callback
 * ============================================================================
 */

/* Global password callback (set by UI layer before session_restore) */
static SessionPasswordCallback s_password_callback = NULL;
static void *s_password_callback_data = NULL;

void session_set_password_callback(SessionPasswordCallback callback,
                                   void *user_data) {
  s_password_callback = callback;
  s_password_callback_data = user_data;
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

/* Safely convert JSON number to size_t (returns 0 on invalid input) */
static size_t json_to_size_t(cJSON *num) {
  if (!cJSON_IsNumber(num))
    return 0;
  double val = num->valuedouble;
  /* Validate: must be finite, non-negative, and within size_t range */
  if (!isfinite(val) || val < 0 || val > (double)SIZE_MAX)
    return 0;
  return (size_t)val;
}

/* Note: History recording is now handled automatically by the database layer
 * via the history callback set up in app_add_connection(). */

/* Column width constants (from tui_internal.h) */
#define SESSION_MIN_COL_WIDTH 4
#define SESSION_MAX_COL_WIDTH 40

/* Calculate column widths for a Tab based on its data */
static void calculate_tab_column_widths(Tab *tab) {
  if (!tab || !tab->data)
    return;

  ResultSet *data = tab->data;
  if (!data->columns || data->num_columns == 0)
    return;

  free(tab->col_widths);
  tab->num_col_widths = data->num_columns;
  tab->col_widths = safe_calloc(tab->num_col_widths, sizeof(int));

  /* Start with column name widths */
  for (size_t i = 0; i < data->num_columns; i++) {
    const char *name = data->columns[i].name;
    int len = name ? (int)strlen(name) : 0;
    tab->col_widths[i] =
        len < SESSION_MIN_COL_WIDTH ? SESSION_MIN_COL_WIDTH : len;
  }

  /* Check data widths (first 100 rows) */
  for (size_t row = 0; row < data->num_rows && row < 100; row++) {
    Row *r = &data->rows[row];
    if (!r->cells)
      continue;
    for (size_t col = 0; col < data->num_columns && col < r->num_cells; col++) {
      char *str = db_value_to_string(&r->cells[col]);
      if (str) {
        int len = (int)strlen(str);
        if (len > tab->col_widths[col]) {
          tab->col_widths[col] = len;
        }
        free(str);
      }
    }
  }

  /* Apply max width */
  for (size_t i = 0; i < tab->num_col_widths; i++) {
    if (tab->col_widths[i] > SESSION_MAX_COL_WIDTH) {
      tab->col_widths[i] = SESSION_MAX_COL_WIDTH;
    }
  }
}

char *session_get_path(void) {
  const char *config_dir = platform_get_config_dir();
  if (!config_dir)
    return NULL;

  return str_printf("%s%s%s", config_dir, LACE_PATH_SEP_STR, SESSION_FILE);
}

/* Find saved connection by connection string */
static const char *find_connection_id_by_connstr(ConnectionManager *mgr,
                                                 const char *connstr) {
  if (!mgr || !connstr)
    return NULL;

  /* Iterate through all saved connections to find matching connstr */
  size_t visible_count = connmgr_count_visible(mgr);
  for (size_t i = 0; i < visible_count; i++) {
    ConnectionItem *item = connmgr_get_visible_item(mgr, i);
    if (item && connmgr_is_connection(item)) {
      char *saved_connstr = connmgr_build_connstr(&item->connection);
      if (saved_connstr) {
        bool match = str_eq(saved_connstr, connstr);
        str_secure_free(saved_connstr); /* Connection string may contain password */
        if (match) {
          return item->connection.id;
        }
      }
    }
  }

  return NULL;
}

/* Build ORDER BY clause from tab's sort entries (caller must free) */
static char *build_tab_order_clause(Tab *tab, TableSchema *schema,
                                    const char *driver_name) {
  if (!tab || tab->num_sort_entries == 0 || !schema || !driver_name)
    return NULL;

  /* Determine quote character based on driver */
  bool use_backtick = (strcmp(driver_name, "mysql") == 0 ||
                       strcmp(driver_name, "mariadb") == 0);

  /* Build ORDER BY clause */
  StringBuilder *sb = sb_new(128);
  if (!sb)
    return NULL;

  bool first_added = false;
  for (size_t i = 0; i < tab->num_sort_entries; i++) {
    SortEntry *entry = &tab->sort_entries[i];
    if (entry->column >= schema->num_columns)
      continue;

    const char *col_name = schema->columns[entry->column].name;
    if (!col_name)
      continue;

    /* Escape column name */
    char *escaped = use_backtick ? str_escape_identifier_backtick(col_name)
                                 : str_escape_identifier_dquote(col_name);
    if (!escaped) {
      sb_free(sb);
      return NULL;
    }

    /* Add separator if not first valid entry */
    if (first_added) {
      sb_append(sb, ", ");
    }
    first_added = true;

    /* Add column with direction */
    sb_printf(sb, "%s %s", escaped,
              entry->direction == SORT_ASC ? "ASC" : "DESC");
    free(escaped);
  }

  char *result = sb_to_string(sb);
  return result;
}

/* ============================================================================
 * Session Free
 * ============================================================================
 */

static void session_filter_free(SessionFilter *f) {
  if (!f)
    return;
  free(f->column_name);
  free(f->value);
}

static void session_tab_free(SessionTab *tab) {
  if (!tab)
    return;

  free(tab->connection_id);
  free(tab->table_name);
  free(tab->query_text);

  /* Free sort entry column names */
  for (size_t i = 0; i < tab->num_sort_entries; i++) {
    free(tab->sort_entries[i].column_name);
  }
  free(tab->sort_entries);

  for (size_t i = 0; i < tab->num_filters; i++) {
    session_filter_free(&tab->filters[i]);
  }
  free(tab->filters);
}

static void session_workspace_free(SessionWorkspace *ws) {
  if (!ws)
    return;

  free(ws->name);
  for (size_t i = 0; i < ws->num_tabs; i++) {
    session_tab_free(&ws->tabs[i]);
  }
  free(ws->tabs);
}

void session_free(Session *session) {
  if (!session)
    return;

  for (size_t i = 0; i < session->num_workspaces; i++) {
    session_workspace_free(&session->workspaces[i]);
  }
  free(session->workspaces);
  free(session);
}

/* ============================================================================
 * JSON Serialization (Save)
 * ============================================================================
 */

static cJSON *serialize_filter(const ColumnFilter *f,
                               const TableSchema *schema) {
  cJSON *json = cJSON_CreateObject();
  if (!json)
    return NULL;

  /* Get column name from schema */
  const char *col_name = "";
  if (schema && f->column_index < schema->num_columns) {
    col_name = schema->columns[f->column_index].name;
  }

  JSON_ADD_STR(json, "column", col_name);
  JSON_ADD_INT(json, "op", (int)f->op);
  JSON_ADD_STR(json, "value", f->value);

  return json;
}

static cJSON *serialize_filters(const TableFilters *filters,
                                const TableSchema *schema) {
  cJSON *arr = cJSON_CreateArray();
  if (!arr)
    return NULL;

  for (size_t i = 0; i < filters->num_filters; i++) {
    cJSON *f = serialize_filter(&filters->filters[i], schema);
    if (f) {
      cJSON_AddItemToArray(arr, f);
    }
  }

  return arr;
}

static cJSON *serialize_tab_ui(const UITabState *ui, bool save_cursor) {
  cJSON *json = cJSON_CreateObject();
  if (!json)
    return NULL;

  JSON_ADD_BOOL(json, "sidebar_visible", ui ? ui->sidebar_visible : false);
  JSON_ADD_BOOL(json, "sidebar_focused", ui ? ui->sidebar_focused : false);
  JSON_ADD_BOOL(json, "filters_visible", ui ? ui->filters_visible : false);
  JSON_ADD_BOOL(json, "filters_focused", ui ? ui->filters_focused : false);
  JSON_ADD_BOOL(json, "query_focus_results", ui ? ui->query_focus_results : false);

  /* Only save cursor/scroll positions if restore_cursor_position is enabled */
  if (save_cursor) {
    JSON_ADD_INT(json, "sidebar_highlight", ui ? (int)ui->sidebar_highlight : 0);
    JSON_ADD_INT(json, "filters_cursor_row", ui ? (int)ui->filters_cursor_row : 0);
    JSON_ADD_INT(json, "filters_cursor_col", ui ? (int)ui->filters_cursor_col : 0);
    JSON_ADD_INT(json, "filters_scroll", ui ? (int)ui->filters_scroll : 0);
  }

  return json;
}

static cJSON *serialize_tab(TuiState *state, size_t ws_idx, size_t tab_idx,
                            ConnectionManager *connmgr) {
  Workspace *ws = &state->app->workspaces[ws_idx];
  if (tab_idx >= ws->num_tabs)
    return NULL;

  Tab *tab = &ws->tabs[tab_idx];
  UITabState *ui = tui_get_tab_ui(state, ws_idx, tab_idx);

  cJSON *json = cJSON_CreateObject();
  if (!json)
    return NULL;

  /* Tab type */
  const char *type_str = "CONNECTION";
  if (tab->type == TAB_TYPE_TABLE)
    type_str = "TABLE";
  else if (tab->type == TAB_TYPE_QUERY)
    type_str = "QUERY";
  JSON_ADD_STR(json, "type", type_str);

  /* Connection ID - find saved connection matching this tab's connection */
  Connection *conn = app_get_connection(state->app, tab->connection_index);
  const char *conn_id = NULL;
  if (conn && conn->saved_conn_id) {
    /* Use stored connection ID directly */
    conn_id = conn->saved_conn_id;
  } else if (conn && conn->connstr && connmgr) {
    /* Fallback: try to match by connection string */
    conn_id = find_connection_id_by_connstr(connmgr, conn->connstr);
  }
  JSON_ADD_STR(json, "connection_id", conn_id);

  /* Table name (for TABLE tabs) */
  if (tab->type == TAB_TYPE_TABLE && tab->table_name) {
    JSON_ADD_STR(json, "table_name", tab->table_name);
  }

  /* Cursor/scroll state - only save if restore_cursor_position is enabled */
  bool save_cursor = state->app->config &&
                     state->app->config->general.restore_cursor_position;
  if (save_cursor) {
    /* Save as absolute positions (loaded_offset + relative) */
    cJSON *cursor = cJSON_CreateArray();
    size_t abs_cursor_row = tab->loaded_offset + tab->cursor_row;
    cJSON_AddItemToArray(cursor, cJSON_CreateNumber((double)abs_cursor_row));
    cJSON_AddItemToArray(cursor, cJSON_CreateNumber((double)tab->cursor_col));
    cJSON_AddItemToObject(json, "cursor", cursor);

    cJSON *scroll = cJSON_CreateArray();
    size_t abs_scroll_row = tab->loaded_offset + tab->scroll_row;
    cJSON_AddItemToArray(scroll, cJSON_CreateNumber((double)abs_scroll_row));
    cJSON_AddItemToArray(scroll, cJSON_CreateNumber((double)tab->scroll_col));
    cJSON_AddItemToObject(json, "scroll", scroll);
  }

  /* Sort state (for TABLE tabs) - save column names, not indices */
  if (tab->type == TAB_TYPE_TABLE && tab->num_sort_entries > 0 && tab->schema) {
    cJSON *sort_arr = cJSON_CreateArray();
    for (size_t i = 0; i < tab->num_sort_entries; i++) {
      size_t col_idx = tab->sort_entries[i].column;
      if (col_idx >= tab->schema->num_columns)
        continue; /* Skip invalid column indices */
      const char *col_name = tab->schema->columns[col_idx].name;
      if (!col_name)
        continue;
      cJSON *entry = cJSON_CreateObject();
      JSON_ADD_STR(entry, "column", col_name);
      JSON_ADD_INT(entry, "direction", tab->sort_entries[i].direction);
      cJSON_AddItemToArray(sort_arr, entry);
    }
    cJSON_AddItemToObject(json, "sort", sort_arr);
  }

  /* Filters (for TABLE tabs) */
  if (tab->type == TAB_TYPE_TABLE && tab->filters.num_filters > 0) {
    cJSON *filters = serialize_filters(&tab->filters, tab->schema);
    if (filters) {
      cJSON_AddItemToObject(json, "filters", filters);
    }
  }

  /* Query text (for QUERY tabs) */
  if (tab->type == TAB_TYPE_QUERY && tab->query_text) {
    JSON_ADD_STR(json, "query_text", tab->query_text);
    /* Only save cursor position if restore_cursor_position is enabled */
    if (save_cursor) {
      JSON_ADD_NUM(json, "query_cursor", tab->query_cursor);
      JSON_ADD_NUM(json, "query_scroll_line", tab->query_scroll_line);
      JSON_ADD_NUM(json, "query_scroll_col", tab->query_scroll_col);
    }
  }

  /* UI state */
  cJSON *ui_json = serialize_tab_ui(ui, save_cursor);
  if (ui_json) {
    cJSON_AddItemToObject(json, "ui", ui_json);
  }

  return json;
}

static cJSON *serialize_workspace(TuiState *state, size_t ws_idx,
                                  ConnectionManager *connmgr) {
  if (ws_idx >= state->app->num_workspaces)
    return NULL;

  Workspace *ws = &state->app->workspaces[ws_idx];

  cJSON *json = cJSON_CreateObject();
  if (!json)
    return NULL;

  JSON_ADD_STR(json, "name", ws->name[0] ? ws->name : "");
  JSON_ADD_NUM(json, "current_tab", ws->current_tab);

  cJSON *tabs = cJSON_CreateArray();
  if (!tabs) {
    cJSON_Delete(json);
    return NULL;
  }

  for (size_t i = 0; i < ws->num_tabs; i++) {
    cJSON *tab = serialize_tab(state, ws_idx, i, connmgr);
    if (tab) {
      cJSON_AddItemToArray(tabs, tab);
    }
  }

  cJSON_AddItemToObject(json, "tabs", tabs);

  return json;
}

/* ============================================================================
 * Save Session
 * ============================================================================
 */

bool session_save(struct TuiState *state, char **error) {
  if (!state || !state->app) {
    err_setf(error, "Invalid state");
    return false;
  }

  /* Don't save if no connections - delete session file if it exists */
  if (state->app->num_connections == 0) {
    char *path = session_get_path();
    if (path) {
      unlink(path); /* Delete old session file */
      free(path);
    }
    return true;
  }

  /* Don't save if no workspaces */
  if (state->app->num_workspaces == 0) {
    return true;
  }

  /* Sync current workspace state before saving */
  tui_sync_to_workspace(state);

  /* Load connection manager to map connections to saved connection IDs */
  ConnectionManager *connmgr = connmgr_load(NULL);

  /* Ensure config directory exists */
  const char *config_dir = platform_get_config_dir();
  if (!config_dir) {
    err_setf(error, "Failed to get config directory");
    if (connmgr)
      connmgr_free(connmgr);
    return false;
  }

  if (!platform_dir_exists(config_dir)) {
    if (!platform_mkdir(config_dir)) {
      err_setf(error, "Failed to create config directory");
      if (connmgr)
        connmgr_free(connmgr);
      return false;
    }
  }

  /* Build JSON */
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    err_setf(error, "Out of memory");
    if (connmgr)
      connmgr_free(connmgr);
    return false;
  }

  /* Settings */
  cJSON *settings = cJSON_CreateObject();
  JSON_ADD_BOOL(settings, "header_visible", state->app->header_visible);
  JSON_ADD_BOOL(settings, "status_visible", state->app->status_visible);
  JSON_ADD_NUM(settings, "page_size", state->app->page_size);
  cJSON_AddItemToObject(json, "settings", settings);

  /* Workspaces */
  cJSON *workspaces = cJSON_CreateArray();
  for (size_t i = 0; i < state->app->num_workspaces; i++) {
    cJSON *ws = serialize_workspace(state, i, connmgr);
    if (ws) {
      cJSON_AddItemToArray(workspaces, ws);
    }
  }
  cJSON_AddItemToObject(json, "workspaces", workspaces);

  JSON_ADD_NUM(json, "current_workspace", state->app->current_workspace);

  if (connmgr)
    connmgr_free(connmgr);

  /* Write to file */
  char *path = session_get_path();
  if (!path) {
    cJSON_Delete(json);
    err_setf(error, "Failed to get session path");
    return false;
  }

  char *content = cJSON_Print(json);
  cJSON_Delete(json);

  if (!content) {
    free(path);
    err_setf(error, "Failed to serialize JSON");
    return false;
  }

  /* Open file with secure permissions atomically (0600 = owner read/write only)
   */
  FILE *f = NULL;
#ifndef LACE_OS_WINDOWS
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    str_secure_free(content); /* Content may contain connection strings */
    err_setf(error, "Failed to open %s for writing: %s", path,
              strerror(errno));
    free(path);
    return false;
  }
  f = fdopen(fd, "w");
  if (!f) {
    close(fd);
    str_secure_free(content); /* Content may contain connection strings */
    err_setf(error, "Failed to open %s for writing: %s", path,
              strerror(errno));
    free(path);
    return false;
  }
#else
  f = fopen(path, "w");
  if (!f) {
    str_secure_free(content); /* Content may contain connection strings */
    err_setf(error, "Failed to open %s for writing: %s", path,
              strerror(errno));
    free(path);
    return false;
  }
#endif

  size_t len = strlen(content);
  size_t written = fwrite(content, 1, len, f);
  fclose(f);
  str_secure_free(content); /* Content may contain connection strings */
  free(path);

  if (written != len) {
    err_setf(error, "Failed to write all data");
    return false;
  }

  return true;
}

/* ============================================================================
 * JSON Parsing (Load)
 * ============================================================================
 */

static bool parse_filter(cJSON *json, SessionFilter *f) {
  f->column_name = json_dup_string_or(json, "column", "");
  if (!f->column_name)
    return false;

  int op = json_get_int(json, "op", 0);
  f->op = (op >= 0) ? op : 0;

  f->value = json_dup_string_or(json, "value", "");
  if (!f->value) {
    free(f->column_name);
    f->column_name = NULL;
    return false;
  }

  return true;
}

static bool parse_tab_ui(cJSON *json, SessionTabUI *ui) {
  if (!json) {
    /* Default values */
    ui->sidebar_visible = true;
    ui->sidebar_focused = false;
    ui->sidebar_highlight = 0;
    ui->filters_visible = false;
    ui->filters_focused = false;
    ui->filters_cursor_row = 0;
    ui->filters_cursor_col = 0;
    ui->filters_scroll = 0;
    ui->query_focus_results = false;
    return true;
  }

  ui->sidebar_visible = json_get_bool(json, "sidebar_visible", true);
  ui->sidebar_focused = json_get_bool(json, "sidebar_focused", false);
  ui->sidebar_highlight = json_get_size(json, "sidebar_highlight", 0);
  ui->filters_visible = json_get_bool(json, "filters_visible", false);
  ui->filters_focused = json_get_bool(json, "filters_focused", false);
  ui->filters_cursor_row = json_get_size(json, "filters_cursor_row", 0);
  ui->filters_cursor_col = json_get_size(json, "filters_cursor_col", 0);
  ui->filters_scroll = json_get_size(json, "filters_scroll", 0);
  ui->query_focus_results = json_get_bool(json, "query_focus_results", false);

  return true;
}

static bool parse_tab(cJSON *json, SessionTab *tab) {
  memset(tab, 0, sizeof(SessionTab));

  const char *type_str = json_get_string(json, "type", "");
  if (str_eq(type_str, "TABLE"))
    tab->type = TAB_TYPE_TABLE;
  else if (str_eq(type_str, "QUERY"))
    tab->type = TAB_TYPE_QUERY;
  else
    tab->type = TAB_TYPE_CONNECTION;

  tab->connection_id = json_dup_string_or(json, "connection_id", "");
  tab->table_name = json_dup_string_or(json, "table_name", "");

  /* Cursor/scroll - safely convert to size_t */
  cJSON *cursor = json_get_array(json, "cursor");
  if (cursor && json_array_size(cursor) >= 2) {
    tab->cursor_row = json_to_size_t(json_get_array_item(cursor, 0));
    tab->cursor_col = json_to_size_t(json_get_array_item(cursor, 1));
  }

  cJSON *scroll = json_get_array(json, "scroll");
  if (scroll && json_array_size(scroll) >= 2) {
    tab->scroll_row = json_to_size_t(json_get_array_item(scroll, 0));
    tab->scroll_col = json_to_size_t(json_get_array_item(scroll, 1));
  }

  /* Sort state (multi-column) - loads column names */
  cJSON *sort_arr = json_get_array(json, "sort");
  if (sort_arr) {
    size_t count = (size_t)json_array_size(sort_arr);
    if (count > 0 && count <= MAX_SORT_COLUMNS) {
      tab->sort_entries = safe_calloc(count, sizeof(SessionSortEntry));
      cJSON *entry;
      cJSON_ArrayForEach(entry, sort_arr) {
        if (tab->num_sort_entries >= count)
          break;
        const char *col_str = json_get_string(entry, "column", NULL);
        int dir = json_get_int(entry, "direction", -1);
        if (col_str && dir >= 0) {
          tab->sort_entries[tab->num_sort_entries].column_name = str_dup(col_str);
          tab->sort_entries[tab->num_sort_entries].direction = dir;
          tab->num_sort_entries++;
        }
      }
    }
  }

  /* Filters */
  cJSON *filters = json_get_array(json, "filters");
  if (filters) {
    size_t count = (size_t)json_array_size(filters);
    if (count > 0) {
      tab->filters = safe_calloc(count, sizeof(SessionFilter));
      cJSON *f;
      cJSON_ArrayForEach(f, filters) {
        if (tab->num_filters >= count)
          break; /* Safety check: don't exceed allocated capacity */
        if (parse_filter(f, &tab->filters[tab->num_filters])) {
          tab->num_filters++;
        }
      }
    }
  }

  /* Query state */
  tab->query_text = json_dup_string_or(json, "query_text", "");
  tab->query_cursor = json_get_size(json, "query_cursor", 0);
  tab->query_scroll_line = json_get_size(json, "query_scroll_line", 0);
  tab->query_scroll_col = json_get_size(json, "query_scroll_col", 0);

  /* UI state */
  cJSON *ui = json_get_object(json, "ui");
  parse_tab_ui(ui, &tab->ui);

  /* Require non-empty connection_id for valid tab */
  return tab->connection_id != NULL && tab->connection_id[0] != '\0';
}

static bool parse_workspace(cJSON *json, SessionWorkspace *ws) {
  memset(ws, 0, sizeof(SessionWorkspace));

  ws->name = json_dup_string_or(json, "name", "");
  ws->current_tab = json_get_size(json, "current_tab", 0);

  cJSON *tabs = json_get_array(json, "tabs");
  if (tabs) {
    size_t count = (size_t)json_array_size(tabs);
    if (count > 0) {
      ws->tabs = safe_calloc(count, sizeof(SessionTab));
      cJSON *t;
      cJSON_ArrayForEach(t, tabs) {
        if (parse_tab(t, &ws->tabs[ws->num_tabs])) {
          ws->num_tabs++;
        }
      }
    }
  }

  return ws->name != NULL;
}

/* ============================================================================
 * Load Session
 * ============================================================================
 */

Session *session_load(char **error) {
  char *path = session_get_path();
  if (!path) {
    err_setf(error, "Failed to get config directory");
    return NULL;
  }

  /* Check if file exists */
  if (!platform_file_exists(path)) {
    /* No session file - not an error, just no session to restore */
    free(path);
    return NULL;
  }

  /* Read file */
  FILE *f = fopen(path, "r");
  if (!f) {
    err_setf(error, "Failed to open %s: %s", path, strerror(errno));
    free(path);
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0 || size > 10 * 1024 * 1024) { /* Max 10MB */
    fclose(f);
    err_setf(error, "Invalid file size");
    free(path);
    return NULL;
  }

  char *content = safe_malloc((size_t)size + 1);

  size_t read_bytes = fread(content, 1, (size_t)size, f);
  fclose(f);

  if (read_bytes != (size_t)size) {
    str_secure_free(content); /* Content may contain connection strings */
    err_setf(error, "Failed to read complete file (got %zu of %ld bytes)",
              read_bytes, size);
    free(path);
    return NULL;
  }

  content[read_bytes] = '\0';
  free(path);

  /* Parse JSON */
  cJSON *json = cJSON_Parse(content);
  str_secure_free(content); /* Content may contain connection strings */

  if (!json) {
    const char *err_ptr = cJSON_GetErrorPtr();
    err_setf(error, "JSON parse error: %s", err_ptr ? err_ptr : "unknown");
    return NULL;
  }

  /* Create session */
  Session *session = safe_calloc(1, sizeof(Session));

  /* Parse settings */
  cJSON *settings = json_get_object(json, "settings");
  if (settings) {
    session->header_visible = json_get_bool(settings, "header_visible", true);
    session->status_visible = json_get_bool(settings, "status_visible", true);
    session->page_size = json_get_size(settings, "page_size", 500);
    if (session->page_size == 0)
      session->page_size = 500;
  } else {
    session->header_visible = true;
    session->status_visible = true;
    session->page_size = 500;
  }

  /* Parse workspaces */
  cJSON *workspaces = json_get_array(json, "workspaces");
  if (workspaces) {
    size_t count = (size_t)json_array_size(workspaces);
    if (count > 0) {
      session->workspaces = safe_calloc(count, sizeof(SessionWorkspace));
      cJSON *ws;
      cJSON_ArrayForEach(ws, workspaces) {
        if (session->num_workspaces >= count)
          break; /* Safety check: don't exceed allocated capacity */
        if (parse_workspace(ws,
                            &session->workspaces[session->num_workspaces])) {
          session->num_workspaces++;
        }
      }
    }
  }

  session->current_workspace = json_get_size(json, "current_workspace", 0);

  cJSON_Delete(json);
  return session;
}

/* ============================================================================
 * Authentication Helpers
 * ============================================================================
 */

/* Check if error message indicates authentication failure */
static bool is_auth_error(const char *err) {
  if (!err)
    return false;

  /* PostgreSQL auth errors */
  if (strstr(err, "password authentication failed"))
    return true;
  if (strstr(err, "authentication failed"))
    return true;
  if (strstr(err, "no password supplied"))
    return true;
  if (strstr(err, "FATAL:  password"))
    return true;

  /* MySQL/MariaDB auth errors */
  if (strstr(err, "Access denied"))
    return true;

  return false;
}

/* ============================================================================
 * Restore Session
 * ============================================================================
 */

/* Find or create connection by saved connection ID */
static size_t restore_connection(TuiState *state, const char *conn_id,
                                 ConnectionManager *connmgr, char **error) {

  if (!conn_id || !conn_id[0]) {
    err_setf(error, "Empty connection ID");
    return (size_t)-1;
  }

  /* Find saved connection by ID */
  ConnectionItem *item = connmgr_find_by_id(connmgr, conn_id);
  if (!item || !connmgr_is_connection(item)) {
    err_setf(error, "Connection not found: %s", conn_id);
    return (size_t)-1;
  }

  /* Build connection string */
  char *connstr = connmgr_build_connstr(&item->connection);
  if (!connstr) {
    err_setf(error, "Failed to build connection string");
    return (size_t)-1;
  }

  /* Check if we already have this connection in the pool */
  for (size_t i = 0; i < state->app->num_connections; i++) {
    Connection *conn = &state->app->connections[i];
    if (conn->active && conn->connstr && str_eq(conn->connstr, connstr)) {
      str_secure_free(connstr); /* Connection string may contain password */
      return i;
    }
  }

  /* Need to establish new connection */
  char *conn_err = NULL;
  SavedConnection *saved = &item->connection;

  DbConnection *db_conn = db_connect(state->app->client, connstr, &conn_err);

  /*
   * Handle connection failures for network databases:
   * - Auth error: prompt for password, retry, loop until success or cancel
   * - Other errors: fail immediately
   */
  bool is_network_db =
      saved->driver && saved->driver[0] && strcmp(saved->driver, "sqlite") != 0;

  while (!db_conn && is_network_db && is_auth_error(conn_err)) {
    /* Auth error - prompt for password via callback */
    if (!s_password_callback) {
      /* No callback set - cannot prompt for password */
      break;
    }

    char title[128];
    snprintf(title, sizeof(title), "Password for %s",
             saved->name && saved->name[0] ? saved->name : "connection");

    char *password = s_password_callback(s_password_callback_data, title,
                                         "Enter password:", conn_err);
    if (!password) {
      /* User cancelled - stop trying */
      break;
    }

    /* Rebuild connection string with password */
    str_secure_free(connstr); /* Connection string may contain password */
    free(conn_err);
    conn_err = NULL;

    connstr = connstr_build(
        saved->driver, (saved->user && saved->user[0]) ? saved->user : NULL,
        password, (saved->host && saved->host[0]) ? saved->host : NULL,
        saved->port,
        (saved->database && saved->database[0]) ? saved->database : NULL, NULL,
        NULL, 0);

    str_secure_free(password);

    if (!connstr) {
      err_setf(error, "Failed to build connection string");
      return (size_t)-1;
    }

    /* Retry connection */
    db_conn = db_connect(state->app->client, connstr, &conn_err);
    /* Loop continues if this also fails with auth error */
  }

  if (!db_conn) {
    err_setf(error, "Connection failed: %s",
              conn_err ? conn_err : "Unknown error");
    free(conn_err);
    str_secure_free(connstr); /* Connection string may contain password */
    return (size_t)-1;
  }

  /* Add to connection pool */
  Connection *conn = app_add_connection(state->app, db_conn, connstr);
  str_secure_free(connstr); /* Connection string may contain password */

  if (!conn) {
    db_disconnect(db_conn);
    err_setf(error, "Failed to add connection to pool");
    return (size_t)-1;
  }

  /* Store saved connection ID for session persistence */
  conn->saved_conn_id = str_dup(conn_id);

  /* Load history from file if persistent mode is enabled */
  if (state->app->config &&
      state->app->config->general.history_mode == HISTORY_MODE_PERSISTENT &&
      conn->history) {
    /* Set connection ID in history */
    if (!conn->history->connection_id) {
      conn->history->connection_id = str_dup(conn_id);
    }
    /* Load existing history from file */
    char *hist_err = NULL;
    history_load(conn->history, &hist_err);
    free(hist_err); /* Ignore errors - file may not exist */
  }

  /* Load tables for this connection */
  char *tables_err = NULL;
  conn->tables = db_list_tables(db_conn, &conn->num_tables, &tables_err);
  free(tables_err);

  return app_find_connection_index(state->app, db_conn);
}

/* Find column index by name in schema */
static size_t find_column_index(const TableSchema *schema, const char *name) {
  if (!schema || !name)
    return (size_t)-1;

  for (size_t i = 0; i < schema->num_columns; i++) {
    if (schema->columns[i].name && str_eq(schema->columns[i].name, name)) {
      return i;
    }
  }

  return (size_t)-1;
}

/* Restore a single tab */
static bool restore_tab(TuiState *state, SessionTab *stab, size_t conn_idx,
                        size_t ws_idx, char **error) {
  Workspace *ws = &state->app->workspaces[ws_idx];
  Tab *tab = NULL;

  Connection *conn = app_get_connection(state->app, conn_idx);
  if (!conn || !conn->conn) {
    err_setf(error, "Invalid connection");
    return false;
  }

  /* Find table index if this is a TABLE tab */
  size_t table_idx = 0;
  if (stab->type == TAB_TYPE_TABLE && stab->table_name && stab->table_name[0]) {
    for (size_t i = 0; i < conn->num_tables; i++) {
      if (conn->tables[i] && str_eq(conn->tables[i], stab->table_name)) {
        table_idx = i;
        break;
      }
    }
  }

  /* Create the appropriate tab type */
  if (stab->type == TAB_TYPE_TABLE) {
    tab = workspace_create_table_tab(ws, conn_idx, table_idx, stab->table_name);
  } else if (stab->type == TAB_TYPE_QUERY) {
    tab = workspace_create_query_tab(ws, conn_idx);
  } else {
    tab = workspace_create_connection_tab(ws, conn_idx, conn->connstr);
  }

  if (!tab) {
    err_setf(error, "Failed to create tab");
    return false;
  }

  /* Check if we should restore cursor positions */
  bool restore_cursor = state->app->config &&
                        state->app->config->general.restore_cursor_position;

  /* Store absolute cursor/scroll positions - will convert to relative after
   * loading. If restore_cursor is disabled, default to 0. */
  size_t abs_cursor_row = restore_cursor ? stab->cursor_row : 0;
  size_t abs_scroll_row = restore_cursor ? stab->scroll_row : 0;
  tab->cursor_col = restore_cursor ? stab->cursor_col : 0;
  tab->scroll_col = restore_cursor ? stab->scroll_col : 0;

  /* Note: Sort state is restored later, after schema is loaded */

  /* Restore query text for QUERY tabs */
  if (stab->type == TAB_TYPE_QUERY && stab->query_text && stab->query_text[0]) {
    free(tab->query_text);
    tab->query_text = str_dup(stab->query_text);
    tab->query_len = tab->query_text ? strlen(tab->query_text) : 0;
    tab->query_capacity = tab->query_len + 1;

    /* Only restore query cursor position if enabled */
    if (restore_cursor) {
      /* Clamp query cursor to text length */
      tab->query_cursor = (stab->query_cursor <= tab->query_len)
                              ? stab->query_cursor
                              : tab->query_len;
      tab->query_scroll_line = stab->query_scroll_line;
      tab->query_scroll_col = stab->query_scroll_col;
    } else {
      tab->query_cursor = 0;
      tab->query_scroll_line = 0;
      tab->query_scroll_col = 0;
    }
  }

  /* Load table data for TABLE tabs */
  if (stab->type == TAB_TYPE_TABLE && stab->table_name && stab->table_name[0]) {
    char *err = NULL;

    /* Get schema first */
    tab->schema = db_get_table_schema(conn->conn, stab->table_name, &err);
    if (!tab->schema) {
      /* Table doesn't exist or can't be accessed - store error for display */
      tab->table_error = err ? err : str_dup("Table does not exist");
      return true; /* Tab created but shows error */
    }
    if (err) {
      free(err);
      err = NULL;
    }

    /* Restore filters (need schema to resolve column names) */
    if (tab->schema && stab->num_filters > 0 && stab->filters) {
      for (size_t i = 0; i < stab->num_filters; i++) {
        SessionFilter *sf = &stab->filters[i];
        size_t col_idx = find_column_index(tab->schema, sf->column_name);
        if (col_idx != (size_t)-1) {
          filters_add(&tab->filters, col_idx, (FilterOperator)sf->op,
                      sf->value);
        }
      }
    }

    /* Restore sort state (need schema to resolve column names) */
    if (tab->schema && stab->num_sort_entries > 0 && stab->sort_entries) {
      for (size_t i = 0; i < stab->num_sort_entries; i++) {
        if (tab->num_sort_entries >= MAX_SORT_COLUMNS)
          break;
        SessionSortEntry *se = &stab->sort_entries[i];
        if (!se->column_name)
          continue;
        size_t col_idx = find_column_index(tab->schema, se->column_name);
        if (col_idx != (size_t)-1) {
          tab->sort_entries[tab->num_sort_entries].column = col_idx;
          tab->sort_entries[tab->num_sort_entries].direction = se->direction;
          tab->num_sort_entries++;
        }
        /* Columns that no longer exist are silently skipped */
      }
    }

    /* Build WHERE clause from filters */
    char *where = NULL;
    if (tab->filters.num_filters > 0 && tab->schema) {
      char *filter_err = NULL;
      where = filters_build_where(&tab->filters, tab->schema,
                                  conn->conn->driver->name, &filter_err);
      if (!where && filter_err) {
        /* Filter build failed - clear filters to avoid inconsistent state */
        filters_clear(&tab->filters);
        free(filter_err);
      }
    }

    /* Get unfiltered row count first */
    bool is_approx = false;
    size_t unfiltered_count = 0;
    if (db_count_rows(conn->conn, stab->table_name, NULL, 0,
                      &unfiltered_count, &is_approx, &err)) {
      tab->unfiltered_total_rows = unfiltered_count;
    } else {
      tab->unfiltered_total_rows = 0;
    }
    if (err) {
      free(err);
      err = NULL;
    }

    /* Get filtered row count if we have filters */
    size_t count = 0;
    if (where) {
      if (!db_count_rows_where(conn->conn, stab->table_name, where, &count, &err)) {
        count = 0;
      }
      if (err) {
        free(err);
        err = NULL;
      }
    } else {
      count = unfiltered_count;
    }
    tab->total_rows = count;
    tab->row_count_approximate = is_approx;

    /* Clamp absolute positions to total_rows (table may have shrunk) */
    if (tab->total_rows > 0) {
      if (abs_cursor_row >= tab->total_rows)
        abs_cursor_row = tab->total_rows - 1;
      if (abs_scroll_row >= tab->total_rows)
        abs_scroll_row = tab->total_rows - 1;
    } else {
      abs_cursor_row = 0;
      abs_scroll_row = 0;
    }

    /* Calculate offset to load - center data window around cursor position */
    size_t page_size = state->app->page_size;
    size_t load_offset = 0;
    if (abs_cursor_row >= page_size / 2) {
      load_offset = abs_cursor_row - page_size / 2;
    }
    /* Ensure we don't load past end of data */
    if (tab->total_rows > 0 && load_offset + page_size > tab->total_rows) {
      if (tab->total_rows > page_size) {
        load_offset = tab->total_rows - page_size;
      } else {
        load_offset = 0;
      }
    }

    /* Build ORDER BY clause from restored sort entries */
    char *order_by = NULL;
    if (tab->schema && conn->conn->driver) {
      order_by =
          build_tab_order_clause(tab, tab->schema, conn->conn->driver->name);
    }

    /* Load data at the calculated offset (near saved cursor position) */
    if (where) {
      tab->data = db_query_page_where(conn->conn, stab->table_name, load_offset,
                                      page_size, where, order_by, false, &err);
    } else {
      tab->data = db_query_page(conn->conn, stab->table_name, load_offset,
                                page_size, order_by, false, &err);
    }
    /* History is recorded automatically by database layer */

    free(order_by);
    free(where);

    if (!tab->data && err) {
      /* Query failed - store error for display */
      tab->table_error = err;
      return true; /* Tab created but shows error */
    }
    if (err) {
      free(err);
    }

    if (tab->data) {
      tab->loaded_offset = load_offset;
      tab->loaded_count = tab->data->num_rows;

      /* Calculate column widths based on loaded data */
      calculate_tab_column_widths(tab);

      /* Convert absolute positions to relative (within loaded window) */
      if (abs_cursor_row >= load_offset) {
        tab->cursor_row = abs_cursor_row - load_offset;
        /* Clamp to loaded data if somehow beyond */
        if (tab->cursor_row >= tab->loaded_count && tab->loaded_count > 0) {
          tab->cursor_row = tab->loaded_count - 1;
        }
      } else {
        tab->cursor_row = 0;
      }

      if (abs_scroll_row >= load_offset) {
        tab->scroll_row = abs_scroll_row - load_offset;
        if (tab->scroll_row >= tab->loaded_count && tab->loaded_count > 0) {
          tab->scroll_row = tab->loaded_count - 1;
        }
      } else {
        tab->scroll_row = 0;
      }
    } else {
      /* Load failed - reset to beginning */
      tab->loaded_offset = 0;
      tab->loaded_count = 0;
      tab->cursor_row = 0;
      tab->scroll_row = 0;
    }

    if (tab->schema && tab->schema->num_columns > 0) {
      if (tab->cursor_col >= tab->schema->num_columns)
        tab->cursor_col = tab->schema->num_columns - 1;
      if (tab->scroll_col >= tab->schema->num_columns)
        tab->scroll_col = tab->schema->num_columns - 1;
    } else {
      tab->cursor_col = 0;
      tab->scroll_col = 0;
    }
  }

  /* Ensure UITabState capacity and restore UI state */
  size_t tab_idx = ws->num_tabs - 1;
  if (tui_ensure_tab_ui_capacity(state, ws_idx, tab_idx)) {
    UITabState *ui = tui_get_tab_ui(state, ws_idx, tab_idx);
    if (ui) {
      /* Always restore visibility state */
      ui->sidebar_visible = stab->ui.sidebar_visible;
      ui->sidebar_focused = stab->ui.sidebar_focused;
      ui->filters_visible = stab->ui.filters_visible;
      ui->filters_focused = stab->ui.filters_focused;

      /* Only restore cursor positions if enabled */
      if (restore_cursor) {
        ui->sidebar_highlight = stab->ui.sidebar_highlight;
        ui->filters_cursor_row = stab->ui.filters_cursor_row;
        ui->filters_cursor_col = stab->ui.filters_cursor_col;
        ui->filters_scroll = stab->ui.filters_scroll;
      } else {
        ui->sidebar_highlight = 0;
        ui->filters_cursor_row = 0;
        ui->filters_cursor_col = 0;
        ui->filters_scroll = 0;
      }

      /* Query tabs: focus editor (not results) since we don't execute on
       * restore */
      ui->query_focus_results = false;
    }
  }

  return true;
}

bool session_restore(struct TuiState *state, Session *session, char **error) {

  if (!state || !session) {
    err_setf(error, "Invalid arguments");
    return false;
  }

  if (session->num_workspaces == 0) {
    return false; /* Nothing to restore - show connect dialog */
  }

  /* Load connection manager */
  ConnectionManager *connmgr = connmgr_load(NULL);
  if (!connmgr) {
    err_setf(error, "Failed to load saved connections");
    return false;
  }

  /* Restore settings */
  state->app->header_visible = session->header_visible;
  state->app->status_visible = session->status_visible;
  state->app->page_size = session->page_size;
  state->header_visible = session->header_visible;
  state->status_visible = session->status_visible;

  size_t restored_workspaces = 0;

  /* Restore each workspace */
  for (size_t ws_i = 0; ws_i < session->num_workspaces; ws_i++) {
    SessionWorkspace *sws = &session->workspaces[ws_i];

    if (sws->num_tabs == 0) {
      continue; /* Skip empty workspaces */
    }

    /* Create workspace */
    Workspace *ws = app_create_workspace(state->app);
    if (!ws) {
      continue;
    }

    size_t ws_idx = state->app->num_workspaces - 1;

    /* Set workspace name */
    if (sws->name && sws->name[0]) {
      snprintf(ws->name, sizeof(ws->name), "%s", sws->name);
    }

    size_t restored_tabs = 0;

    /* Restore each tab */
    for (size_t tab_i = 0; tab_i < sws->num_tabs; tab_i++) {
      SessionTab *stab = &sws->tabs[tab_i];

      /* Get or create connection */
      char *conn_err = NULL;
      size_t conn_idx =
          restore_connection(state, stab->connection_id, connmgr, &conn_err);

      if (conn_idx == (size_t)-1) {
        /* Connection failed - skip this tab */
        free(conn_err);
        continue;
      }

      /* Restore the tab */
      char *tab_err = NULL;
      if (restore_tab(state, stab, conn_idx, ws_idx, &tab_err)) {
        restored_tabs++;
      }
      free(tab_err);
    }

    if (restored_tabs > 0) {
      /* Set current tab (clamped to valid range) */
      if (sws->current_tab < ws->num_tabs) {
        ws->current_tab = sws->current_tab;
      }
      restored_workspaces++;
    } else {
      /* No tabs restored - remove the empty workspace */
      app_close_workspace(state->app, ws_idx);
    }
  }

  connmgr_free(connmgr);

  if (restored_workspaces == 0) {
    err_setf(error, "No workspaces could be restored");
    return false;
  }

  /* Set current workspace (clamped to valid range) */
  if (session->current_workspace < state->app->num_workspaces) {
    state->app->current_workspace = session->current_workspace;
  }

  /* Sync TUI state from restored app state */
  tui_sync_from_app(state);

  /* If current tab has a table error, show and focus sidebar */
  Tab *current_tab = app_current_tab(state->app);
  if (current_tab && current_tab->table_error) {
    if (!state->sidebar_visible) {
      state->sidebar_visible = true;
      tui_recreate_windows(state);
    }
    state->sidebar_focused = true;
  }

  return true;
}
