/*
 * laced - Lace Database Daemon
 * RPC method handler implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "handler.h"
#include "json.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Helper Macros
 * ========================================================================== */

#define HANDLER_OK(json_result) \
  (LacedHandlerResult) { .result = (json_result), .error_code = 0, .error_message = NULL }

#define HANDLER_ERROR(code, msg) \
  (LacedHandlerResult) { .result = NULL, .error_code = (code), .error_message = str_dup(msg) }

#define HANDLER_ERROR_DYN(code, msg) \
  (LacedHandlerResult) { .result = NULL, .error_code = (code), .error_message = (msg) }

/* JSON-RPC error codes */
#define JSONRPC_INVALID_PARAMS -32602
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INTERNAL_ERROR -32603

/* ==========================================================================
 * Connection Handlers
 * ========================================================================== */

/* connect: Open a database connection */
static LacedHandlerResult handle_connect(LacedSession *session, cJSON *params) {
  const char *connstr = NULL;
  const char *password = NULL;

  if (!laced_json_get_string(params, "connstr", &connstr)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'connstr' parameter");
  }

  /* Password is optional */
  laced_json_get_string(params, "password", &password);

  int conn_id = 0;
  char *err = NULL;
  if (!laced_session_connect(session, connstr, password, &conn_id, &err)) {
    return HANDLER_ERROR_DYN(JSONRPC_INTERNAL_ERROR,
                             err ? err : str_dup("Connection failed"));
  }

  cJSON *result = cJSON_CreateObject();
  if (result) {
    cJSON_AddNumberToObject(result, "conn_id", conn_id);
  }
  return HANDLER_OK(result);
}

/* disconnect: Close a database connection */
static LacedHandlerResult handle_disconnect(LacedSession *session, cJSON *params) {
  int conn_id = 0;
  if (!laced_json_get_int(params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'conn_id' parameter");
  }

  char *err = NULL;
  if (!laced_session_disconnect(session, conn_id, &err)) {
    return HANDLER_ERROR_DYN(JSONRPC_INTERNAL_ERROR,
                             err ? err : str_dup("Disconnect failed"));
  }

  return HANDLER_OK(cJSON_CreateObject());
}

/* connections: List active connections */
static LacedHandlerResult handle_connections(LacedSession *session,
                                             cJSON *params) {
  (void)params;

  LacedConnInfo *info = NULL;
  size_t count = 0;
  if (!laced_session_list_connections(session, &info, &count)) {
    return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Failed to list connections");
  }

  cJSON *result = cJSON_CreateArray();
  if (result) {
    for (size_t i = 0; i < count; i++) {
      cJSON *conn = cJSON_CreateObject();
      if (conn) {
        cJSON_AddNumberToObject(conn, "id", info[i].id);
        if (info[i].driver) {
          cJSON_AddStringToObject(conn, "driver", info[i].driver);
        }
        if (info[i].database) {
          cJSON_AddStringToObject(conn, "database", info[i].database);
        }
        if (info[i].host) {
          cJSON_AddStringToObject(conn, "host", info[i].host);
        }
        if (info[i].port > 0) {
          cJSON_AddNumberToObject(conn, "port", info[i].port);
        }
        if (info[i].user) {
          cJSON_AddStringToObject(conn, "user", info[i].user);
        }
        cJSON_AddItemToArray(result, conn);
      }
    }
  }

  laced_conn_info_array_free(info, count);
  return HANDLER_OK(result);
}

/* ==========================================================================
 * Schema Handlers
 * ========================================================================== */

/* tables: List tables in database */
static LacedHandlerResult handle_tables(LacedSession *session, cJSON *params) {
  int conn_id = 0;
  if (!laced_json_get_int(params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'conn_id' parameter");
  }

  DbConnection *conn = laced_session_get_connection(session, conn_id);
  if (!conn) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Invalid connection ID");
  }

  char *err = NULL;
  size_t count = 0;
  char **tables = db_list_tables(conn, &count, &err);
  if (!tables && count > 0) {
    return HANDLER_ERROR_DYN(JSONRPC_INTERNAL_ERROR,
                             err ? err : str_dup("Failed to list tables"));
  }
  free(err);

  cJSON *result = cJSON_CreateArray();
  if (result && tables) {
    for (size_t i = 0; i < count; i++) {
      if (tables[i]) {
        cJSON_AddItemToArray(result, cJSON_CreateString(tables[i]));
      }
    }
  }

  /* Free table list */
  if (tables) {
    for (size_t i = 0; i < count; i++) {
      free(tables[i]);
    }
    free(tables);
  }

  return HANDLER_OK(result);
}

/* schema: Get table schema */
static LacedHandlerResult handle_schema(LacedSession *session, cJSON *params) {
  int conn_id = 0;
  const char *table = NULL;

  if (!laced_json_get_int(params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'conn_id' parameter");
  }
  if (!laced_json_get_string(params, "table", &table)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'table' parameter");
  }

  DbConnection *conn = laced_session_get_connection(session, conn_id);
  if (!conn) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Invalid connection ID");
  }

  char *err = NULL;
  TableSchema *schema = db_get_table_schema(conn, table, &err);
  if (!schema) {
    return HANDLER_ERROR_DYN(JSONRPC_INTERNAL_ERROR,
                             err ? err : str_dup("Failed to get schema"));
  }

  cJSON *result = laced_json_from_schema(schema);
  db_schema_free(schema);

  return HANDLER_OK(result);
}

/* ==========================================================================
 * Query Handlers
 * ========================================================================== */

/* query: Execute paginated table query */
static LacedHandlerResult handle_query(LacedSession *session, cJSON *params) {
  int conn_id = 0;
  const char *table = NULL;
  size_t offset = 0;
  size_t limit = 500;

  if (!laced_json_get_int(params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'conn_id' parameter");
  }
  if (!laced_json_get_string(params, "table", &table)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'table' parameter");
  }

  /* Optional pagination parameters */
  laced_json_get_size(params, "offset", &offset);
  laced_json_get_size(params, "limit", &limit);

  /* Cap limit to prevent DoS */
  if (limit > 10000) {
    limit = 10000;
  }

  DbConnection *conn = laced_session_get_connection(session, conn_id);
  if (!conn) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Invalid connection ID");
  }

  /* TODO: Handle filters and sorts from params */

  char *err = NULL;
  ResultSet *rs = db_query_page(conn, table, offset, limit, NULL, false, &err);
  if (!rs) {
    return HANDLER_ERROR_DYN(JSONRPC_INTERNAL_ERROR,
                             err ? err : str_dup("Query failed"));
  }

  /* Get total count */
  int64_t total = db_count_rows(conn, table, NULL);
  if (total >= 0) {
    rs->total_rows = (size_t)total;
  }

  cJSON *result = laced_json_from_result(rs);
  db_result_free(rs);

  return HANDLER_OK(result);
}

/* count: Count rows in table */
static LacedHandlerResult handle_count(LacedSession *session, cJSON *params) {
  int conn_id = 0;
  const char *table = NULL;

  if (!laced_json_get_int(params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'conn_id' parameter");
  }
  if (!laced_json_get_string(params, "table", &table)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'table' parameter");
  }

  DbConnection *conn = laced_session_get_connection(session, conn_id);
  if (!conn) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Invalid connection ID");
  }

  /* TODO: Handle filters from params */

  bool approximate = false;
  char *err = NULL;
  int64_t count = db_count_rows_fast(conn, table, true, &approximate, &err);
  if (count < 0) {
    return HANDLER_ERROR_DYN(JSONRPC_INTERNAL_ERROR,
                             err ? err : str_dup("Count failed"));
  }
  free(err);

  cJSON *result = cJSON_CreateObject();
  if (result) {
    cJSON_AddNumberToObject(result, "count", (double)count);
    cJSON_AddBoolToObject(result, "approximate", approximate);
  }

  return HANDLER_OK(result);
}

/* exec: Execute raw SQL */
static LacedHandlerResult handle_exec(LacedSession *session, cJSON *params) {
  int conn_id = 0;
  const char *sql = NULL;

  if (!laced_json_get_int(params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'conn_id' parameter");
  }
  if (!laced_json_get_string(params, "sql", &sql)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'sql' parameter");
  }

  DbConnection *conn = laced_session_get_connection(session, conn_id);
  if (!conn) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Invalid connection ID");
  }

  /* Check if it's a SELECT statement */
  const char *p = sql;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

  bool is_select = (strncasecmp(p, "SELECT", 6) == 0 ||
                    strncasecmp(p, "PRAGMA", 6) == 0 ||
                    strncasecmp(p, "SHOW", 4) == 0 ||
                    strncasecmp(p, "DESCRIBE", 8) == 0 ||
                    strncasecmp(p, "EXPLAIN", 7) == 0);

  cJSON *result = cJSON_CreateObject();
  if (!result) {
    return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Memory allocation failed");
  }

  char *err = NULL;
  if (is_select) {
    ResultSet *rs = db_query(conn, sql, &err);
    if (!rs) {
      cJSON_Delete(result);
      return HANDLER_ERROR_DYN(JSONRPC_INTERNAL_ERROR,
                               err ? err : str_dup("Query failed"));
    }

    cJSON_AddStringToObject(result, "type", "select");
    cJSON *data = laced_json_from_result(rs);
    if (data) {
      cJSON_AddItemToObject(result, "data", data);
    }
    db_result_free(rs);
  } else {
    int64_t affected = db_exec(conn, sql, &err);
    if (affected < 0) {
      cJSON_Delete(result);
      return HANDLER_ERROR_DYN(JSONRPC_INTERNAL_ERROR,
                               err ? err : str_dup("Execution failed"));
    }

    cJSON_AddStringToObject(result, "type", "exec");
    cJSON_AddNumberToObject(result, "affected", (double)affected);
  }
  free(err);

  return HANDLER_OK(result);
}

/* ==========================================================================
 * Mutation Handlers
 * ========================================================================== */

/* update: Update a cell value */
static LacedHandlerResult handle_update(LacedSession *session, cJSON *params) {
  int conn_id = 0;
  const char *table = NULL;
  const char *column = NULL;

  if (!laced_json_get_int(params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'conn_id' parameter");
  }
  if (!laced_json_get_string(params, "table", &table)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'table' parameter");
  }
  if (!laced_json_get_string(params, "column", &column)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'column' parameter");
  }

  DbConnection *conn = laced_session_get_connection(session, conn_id);
  if (!conn) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Invalid connection ID");
  }

  /* Get value */
  cJSON *value_json = cJSON_GetObjectItem(params, "value");
  DbValue value;
  if (!laced_json_to_value(value_json, &value)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Invalid 'value' parameter");
  }

  /* Get primary key */
  cJSON *pk_json = cJSON_GetObjectItem(params, "pk");
  if (!pk_json || !cJSON_IsArray(pk_json)) {
    db_value_free(&value);
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'pk' array parameter");
  }

  int pk_size = cJSON_GetArraySize(pk_json);
  if (pk_size == 0) {
    db_value_free(&value);
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Empty 'pk' array");
  }

  /* Parse PK columns and values */
  const char **pk_cols = calloc((size_t)pk_size, sizeof(char *));
  DbValue *pk_vals = calloc((size_t)pk_size, sizeof(DbValue));
  if (!pk_cols || !pk_vals) {
    free(pk_cols);
    free(pk_vals);
    db_value_free(&value);
    return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Memory allocation failed");
  }

  bool pk_valid = true;
  for (int i = 0; i < pk_size && pk_valid; i++) {
    cJSON *pk_item = cJSON_GetArrayItem(pk_json, i);
    if (!pk_item || !cJSON_IsObject(pk_item)) {
      pk_valid = false;
      break;
    }
    cJSON *col_json = cJSON_GetObjectItem(pk_item, "column");
    cJSON *val_json = cJSON_GetObjectItem(pk_item, "value");
    if (!col_json || !cJSON_IsString(col_json)) {
      pk_valid = false;
      break;
    }
    pk_cols[i] = col_json->valuestring;
    if (!laced_json_to_value(val_json, &pk_vals[i])) {
      pk_valid = false;
      break;
    }
  }

  if (!pk_valid) {
    for (int i = 0; i < pk_size; i++) {
      db_value_free(&pk_vals[i]);
    }
    free(pk_cols);
    free(pk_vals);
    db_value_free(&value);
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Invalid 'pk' format");
  }

  /* Perform update */
  char *err = NULL;
  bool success = db_update_cell(conn, table, pk_cols, pk_vals, (size_t)pk_size,
                                column, &value, &err);

  /* Cleanup */
  for (int i = 0; i < pk_size; i++) {
    db_value_free(&pk_vals[i]);
  }
  free(pk_cols);
  free(pk_vals);
  db_value_free(&value);

  if (!success) {
    return HANDLER_ERROR_DYN(JSONRPC_INTERNAL_ERROR,
                             err ? err : str_dup("Update failed"));
  }
  free(err);

  return HANDLER_OK(cJSON_CreateObject());
}

/* delete: Delete a row */
static LacedHandlerResult handle_delete(LacedSession *session, cJSON *params) {
  int conn_id = 0;
  const char *table = NULL;

  if (!laced_json_get_int(params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'conn_id' parameter");
  }
  if (!laced_json_get_string(params, "table", &table)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'table' parameter");
  }

  DbConnection *conn = laced_session_get_connection(session, conn_id);
  if (!conn) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Invalid connection ID");
  }

  /* Get primary key */
  cJSON *pk_json = cJSON_GetObjectItem(params, "pk");
  if (!pk_json || !cJSON_IsArray(pk_json)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'pk' array parameter");
  }

  int pk_size = cJSON_GetArraySize(pk_json);
  if (pk_size == 0) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Empty 'pk' array");
  }

  /* Parse PK columns and values */
  const char **pk_cols = calloc((size_t)pk_size, sizeof(char *));
  DbValue *pk_vals = calloc((size_t)pk_size, sizeof(DbValue));
  if (!pk_cols || !pk_vals) {
    free(pk_cols);
    free(pk_vals);
    return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Memory allocation failed");
  }

  bool pk_valid = true;
  for (int i = 0; i < pk_size && pk_valid; i++) {
    cJSON *pk_item = cJSON_GetArrayItem(pk_json, i);
    if (!pk_item || !cJSON_IsObject(pk_item)) {
      pk_valid = false;
      break;
    }
    cJSON *col_json = cJSON_GetObjectItem(pk_item, "column");
    cJSON *val_json = cJSON_GetObjectItem(pk_item, "value");
    if (!col_json || !cJSON_IsString(col_json)) {
      pk_valid = false;
      break;
    }
    pk_cols[i] = col_json->valuestring;
    if (!laced_json_to_value(val_json, &pk_vals[i])) {
      pk_valid = false;
      break;
    }
  }

  if (!pk_valid) {
    for (int i = 0; i < pk_size; i++) {
      db_value_free(&pk_vals[i]);
    }
    free(pk_cols);
    free(pk_vals);
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Invalid 'pk' format");
  }

  /* Perform delete */
  char *err = NULL;
  bool success = db_delete_row(conn, table, pk_cols, pk_vals, (size_t)pk_size, &err);

  /* Cleanup */
  for (int i = 0; i < pk_size; i++) {
    db_value_free(&pk_vals[i]);
  }
  free(pk_cols);
  free(pk_vals);

  if (!success) {
    return HANDLER_ERROR_DYN(JSONRPC_INTERNAL_ERROR,
                             err ? err : str_dup("Delete failed"));
  }
  free(err);

  return HANDLER_OK(cJSON_CreateObject());
}

/* ==========================================================================
 * Utility Handlers
 * ========================================================================== */

/* ping: Check if daemon is alive */
static LacedHandlerResult handle_ping(LacedSession *session, cJSON *params) {
  (void)session;
  (void)params;

  cJSON *result = cJSON_CreateObject();
  if (result) {
    cJSON_AddStringToObject(result, "status", "ok");
  }
  return HANDLER_OK(result);
}

/* version: Get daemon version */
static LacedHandlerResult handle_version(LacedSession *session, cJSON *params) {
  (void)session;
  (void)params;

  cJSON *result = cJSON_CreateObject();
  if (result) {
    cJSON_AddStringToObject(result, "daemon_version", "0.1.0");
    cJSON_AddStringToObject(result, "protocol_version", "1.0");

    /* List available drivers */
    cJSON *drivers = cJSON_CreateArray();
    if (drivers) {
      size_t count = 0;
      DbDriver **all_drivers = db_get_all_drivers(&count);
      for (size_t i = 0; i < count; i++) {
        if (all_drivers[i] && all_drivers[i]->display_name) {
          cJSON_AddItemToArray(drivers,
                               cJSON_CreateString(all_drivers[i]->display_name));
        }
      }
      cJSON_AddItemToObject(result, "drivers", drivers);
    }
  }
  return HANDLER_OK(result);
}

/* shutdown: Request daemon shutdown */
static LacedHandlerResult handle_shutdown(LacedSession *session, cJSON *params) {
  (void)session;
  (void)params;

  /* TODO: Signal main loop to exit gracefully */
  return HANDLER_OK(cJSON_CreateObject());
}

/* ==========================================================================
 * Method Dispatch
 * ========================================================================== */

/* Method handler function type */
typedef LacedHandlerResult (*MethodHandler)(LacedSession *session, cJSON *params);

/* Method dispatch table */
static struct {
  const char *name;
  MethodHandler handler;
} g_methods[] = {
    /* Connection management */
    {"connect", handle_connect},
    {"disconnect", handle_disconnect},
    {"connections", handle_connections},

    /* Schema discovery */
    {"tables", handle_tables},
    {"schema", handle_schema},

    /* Data queries */
    {"query", handle_query},
    {"count", handle_count},
    {"exec", handle_exec},

    /* Data mutations */
    {"update", handle_update},
    {"delete", handle_delete},
    /* TODO: {"insert", handle_insert}, */

    /* Utilities */
    {"ping", handle_ping},
    {"version", handle_version},
    {"shutdown", handle_shutdown},

    {NULL, NULL}  /* Sentinel */
};

LacedHandlerResult laced_handler_dispatch(LacedSession *session,
                                          const char *method,
                                          cJSON *params) {
  if (!session || !method) {
    return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Invalid handler state");
  }

  /* Find method handler */
  for (int i = 0; g_methods[i].name != NULL; i++) {
    if (strcmp(g_methods[i].name, method) == 0) {
      return g_methods[i].handler(session, params);
    }
  }

  return HANDLER_ERROR(JSONRPC_METHOD_NOT_FOUND, "Method not found");
}
