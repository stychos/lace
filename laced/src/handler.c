/*
 * laced - Lace Database Daemon
 * RPC method handler implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "handler.h"
#include "async.h"
#include "json.h"
#include "log.h"
#include <util/mem.h>
#include <util/str.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Helper Macros
 * ========================================================================== */

#define HANDLER_OK(json_result) \
  (LacedHandlerResult) { .result = (json_result), .error_code = 0, .error_message = NULL, .deferred = false, .deferred_query = NULL }

#define HANDLER_ERROR(code, msg) \
  (LacedHandlerResult) { .result = NULL, .error_code = (code), .error_message = str_dup(msg), .deferred = false, .deferred_query = NULL }

#define HANDLER_ERROR_DYN(code, msg) \
  (LacedHandlerResult) { .result = NULL, .error_code = (code), .error_message = (msg), .deferred = false, .deferred_query = NULL }

#define HANDLER_DEFERRED(q) \
  (LacedHandlerResult) { .result = NULL, .error_code = 0, .error_message = NULL, .deferred = true, .deferred_query = (q) }

/* JSON-RPC error codes */
#define JSONRPC_INVALID_PARAMS -32602
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INTERNAL_ERROR -32603

/* ==========================================================================
 * Connection Handlers
 * ========================================================================== */

/* connect: Open a database connection */
static LacedHandlerResult handle_connect(LacedSession *session,
                                         AsyncQueue *async_queue,
                                         cJSON *params,
                                         cJSON *request_id) {
  (void)async_queue;
  (void)request_id;
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
static LacedHandlerResult handle_disconnect(LacedSession *session,
                                            AsyncQueue *async_queue,
                                            cJSON *params,
                                            cJSON *request_id) {
  (void)async_queue;
  (void)request_id;
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
                                             AsyncQueue *async_queue,
                                             cJSON *params,
                                             cJSON *request_id) {
  (void)async_queue;
  (void)request_id;
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
 * Query Handlers
 * ========================================================================== */

/* query: Execute raw SQL (async) */
static LacedHandlerResult handle_query(LacedSession *session,
                                       AsyncQueue *async_queue,
                                       cJSON *params,
                                       cJSON *request_id) {
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

  /* Dispatch async exec - response will be sent when exec completes */
  if (async_queue && request_id) {
    AsyncQuery *query = async_exec_start(async_queue, session, conn_id,
                                         sql, request_id);
    if (!query) {
      return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Failed to start async exec");
    }
    return HANDLER_DEFERRED(query);
  }

  /* Fallback: synchronous execution if no async queue */

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

  laced_session_prepare_cancel(session, conn_id);

  char *err = NULL;
  if (is_select) {
    ResultSet *rs = db_query(conn, sql, &err);
    bool success = (rs != NULL);

    laced_session_finish_query(session, conn_id, success);

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
    bool success = (affected >= 0);

    laced_session_finish_query(session, conn_id, success);

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
 * Utility Handlers
 * ========================================================================== */

/* ping: Check if daemon is alive */
static LacedHandlerResult handle_ping(LacedSession *session,
                                      AsyncQueue *async_queue,
                                      cJSON *params,
                                      cJSON *request_id) {
  (void)session;
  (void)async_queue;
  (void)params;
  (void)request_id;

  cJSON *result = cJSON_CreateObject();
  if (result) {
    cJSON_AddStringToObject(result, "status", "ok");
  }
  return HANDLER_OK(result);
}

/* version: Get daemon version */
static LacedHandlerResult handle_version(LacedSession *session,
                                         AsyncQueue *async_queue,
                                         cJSON *params,
                                         cJSON *request_id) {
  (void)session;
  (void)async_queue;
  (void)params;
  (void)request_id;

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
static LacedHandlerResult handle_shutdown(LacedSession *session,
                                          AsyncQueue *async_queue,
                                          cJSON *params,
                                          cJSON *request_id) {
  (void)session;
  (void)async_queue;
  (void)params;
  (void)request_id;

  /* TODO: Signal main loop to exit gracefully */
  return HANDLER_OK(cJSON_CreateObject());
}

/* connection: Get connection statistics */
static LacedHandlerResult handle_connection(LacedSession *session,
                                            AsyncQueue *async_queue,
                                            cJSON *params,
                                            cJSON *request_id) {
  (void)async_queue;
  (void)request_id;
  int conn_id = 0;

  if (!laced_json_get_int(params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'conn_id' parameter");
  }

  LacedConnStats stats;
  if (!laced_session_get_conn_stats(session, conn_id, &stats)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Connection not found");
  }

  cJSON *result = cJSON_CreateObject();
  if (result) {
    cJSON_AddNumberToObject(result, "uptime_ms", (double)stats.uptime_ms);
    cJSON_AddNumberToObject(result, "query_count", (double)stats.query_count);
    cJSON_AddNumberToObject(result, "total_query_time_ms", (double)stats.total_query_time_ms);
    cJSON_AddNumberToObject(result, "failed_query_count", (double)stats.failed_query_count);
    cJSON_AddBoolToObject(result, "query_running", stats.query_running);
    if (stats.query_running) {
      cJSON_AddNumberToObject(result, "current_query_time_ms", (double)stats.current_query_time_ms);
    }
  }
  return HANDLER_OK(result);
}

/* cancel: Cancel a running query on a connection */
static LacedHandlerResult handle_cancel(LacedSession *session,
                                        AsyncQueue *async_queue,
                                        cJSON *params,
                                        cJSON *request_id) {
  (void)request_id;
  int conn_id = 0;

  if (!laced_json_get_int(params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'conn_id' parameter");
  }

  /* Cancel via async queue if available */
  if (async_queue) {
    (void)async_cancel_by_conn_id(async_queue, session, conn_id);
  }

  /* Also call session cancel directly (for synchronous operations) */
  char *err = NULL;
  laced_session_cancel_query(session, conn_id, &err);
  free(err);

  cJSON *result = cJSON_CreateObject();
  if (result) {
    cJSON_AddBoolToObject(result, "cancelled", true);
  }
  return HANDLER_OK(result);
}

/* ==========================================================================
 * Method Dispatch
 * ========================================================================== */

/* Method handler function type */
typedef LacedHandlerResult (*MethodHandler)(LacedSession *session,
                                            AsyncQueue *async_queue,
                                            cJSON *params,
                                            cJSON *request_id);

/* Method dispatch table */
static struct {
  const char *name;
  MethodHandler handler;
} g_methods[] = {
    /* Connection management */
    {"connect", handle_connect},
    {"disconnect", handle_disconnect},
    {"connections", handle_connections},

    /* Query execution */
    {"query", handle_query},

    /* Utilities */
    {"ping", handle_ping},
    {"version", handle_version},
    {"shutdown", handle_shutdown},
    {"connection", handle_connection},
    {"cancel", handle_cancel},

    {NULL, NULL}  /* Sentinel */
};

LacedHandlerResult laced_handler_dispatch(LacedSession *session,
                                          AsyncQueue *async_queue,
                                          const char *method,
                                          cJSON *params,
                                          cJSON *request_id) {
  if (!session || !method) {
    return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Invalid handler state");
  }

  LOG_DEBUG("RPC method: %s", method);

  /* Find method handler */
  for (int i = 0; g_methods[i].name != NULL; i++) {
    if (strcmp(g_methods[i].name, method) == 0) {
      return g_methods[i].handler(session, async_queue, params, request_id);
    }
  }

  LOG_WARN("RPC method not found: %s", method);
  return HANDLER_ERROR(JSONRPC_METHOD_NOT_FOUND, "Method not found");
}
