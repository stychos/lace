/*
 * laced - Lace Database Daemon
 * RPC method handler implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "handler.h"
#include "db/db_common.h"
#include "async.h"
#include "json.h"
#include "log.h"
#include "../../liblace/include/error.h"
#include <util/mem.h>
#include <util/str.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

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
#define JSONRPC_INVALID_PARAMS LACE_ERR_INVALID_PARAMS
#define JSONRPC_METHOD_NOT_FOUND LACE_ERR_METHOD_NOT_FOUND
#define JSONRPC_INTERNAL_ERROR LACE_ERR_INTERNAL_ERROR

/* ==========================================================================
 * Connection Handlers
 * ========================================================================== */

/* connect: may take seconds (network, SSH tunnel), so it runs on a worker */
typedef struct {
  char *connstr;
  char *password;
  char owner[LACED_OWNER_MAX];
} ConnectJobArg;

static void connect_job_free(void *p) {
  ConnectJobArg *a = p;
  if (a) {
    str_secure_free(a->connstr);
    str_secure_free(a->password);
    free(a);
  }
}

static cJSON *connect_job(LacedSession *session, void *p, char **err,
                          int *err_code) {
  ConnectJobArg *a = p;
  int conn_id = 0;
  if (!laced_session_connect(session, a->connstr, a->password, a->owner,
                             &conn_id, err)) {
    *err_code = LACE_ERR_CONNECTION_FAILED;
    if (!*err) {
      *err = str_dup("Connection failed");
    }
    return NULL;
  }
  cJSON *result = cJSON_CreateObject();
  cJSON_AddNumberToObject(result, "conn_id", conn_id);
  return result;
}

static LacedHandlerResult handle_connect(const LacedRequest *rq) {
  const char *connstr = NULL;
  const char *password = NULL;

  if (!laced_json_get_string(rq->params, "connstr", &connstr)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'connstr' parameter");
  }

  /* Password is optional */
  laced_json_get_string(rq->params, "password", &password);

  ConnectJobArg *arg = safe_calloc(1, sizeof(ConnectJobArg));
  arg->connstr = str_dup(connstr);
  arg->password = password ? str_dup(password) : NULL;
  snprintf(arg->owner, sizeof(arg->owner), "%s", rq->from.owner);
  AsyncQuery *job = async_job_start(rq->queue, rq->session, rq->id,
                                    &rq->from, connect_job, arg, connect_job_free);
  if (!job) {
    return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Failed to start connect");
  }
  return HANDLER_DEFERRED(job);
}

/* disconnect: closing a connection (and its SSH tunnel) can block */
static void int_job_free(void *p) { free(p); }

static cJSON *disconnect_job(LacedSession *session, void *p, char **err,
                             int *err_code) {
  int conn_id = *(int *)p;
  if (!laced_session_disconnect(session, conn_id, err)) {
    /* Refused: unknown, or still running a statement or a cancel */
    *err_code = laced_session_has_connection(session, conn_id)
                    ? LACE_ERR_CONNECTION_BUSY
                    : LACE_ERR_INVALID_CONN_ID;
    if (!*err) {
      *err = str_dup("Disconnect failed");
    }
    return NULL;
  }
  return cJSON_CreateObject();
}

static LacedHandlerResult handle_disconnect(const LacedRequest *rq) {
  int conn_id = 0;
  if (!laced_json_get_int(rq->params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "'conn_id' must be an integer connection id");
  }

  int *arg = safe_malloc(sizeof(int));
  *arg = conn_id;
  AsyncQuery *job = async_job_start(rq->queue, rq->session, rq->id,
                                    &rq->from, disconnect_job, arg, int_job_free);
  if (!job) {
    return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Failed to start disconnect");
  }
  return HANDLER_DEFERRED(job);
}

/* connections: List active connections */
static LacedHandlerResult handle_connections(const LacedRequest *rq) {
  LacedConnInfo *info = NULL;
  size_t count = 0;
  if (!laced_session_list_connections(rq->session, &info, &count)) {
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
static LacedHandlerResult handle_query(const LacedRequest *rq) {
  int conn_id = 0;
  const char *sql = NULL;

  if (!laced_json_get_int(rq->params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "'conn_id' must be an integer connection id");
  }
  if (!laced_json_get_string(rq->params, "sql", &sql)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "Missing 'sql' parameter");
  }

  if (!laced_session_has_connection(rq->session, conn_id)) {
    return HANDLER_ERROR(LACE_ERR_INVALID_CONN_ID, "Invalid connection ID");
  }

  /* Always on a worker, also for a notification (no id): the old inline
   * fallback ran on the main thread and ignored a busy connection, so it
   * could use a DbConnection a worker was already using. The reply of a
   * notification is dropped. */
  AsyncQuery *query = async_exec_start(rq->queue, rq->session, conn_id, sql,
                                       rq->id, &rq->from);
  if (!query) {
    return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Failed to start async exec");
  }
  return HANDLER_DEFERRED(query);
}

/* ==========================================================================
 * Utility Handlers
 * ========================================================================== */

/* ping: Check if daemon is alive */
static LacedHandlerResult handle_ping(const LacedRequest *rq) {
  (void)rq;
  cJSON *result = cJSON_CreateObject();
  if (result) {
    cJSON_AddStringToObject(result, "status", "ok");
  }
  return HANDLER_OK(result);
}

/* version: Get daemon version */
static LacedHandlerResult handle_version(const LacedRequest *rq) {
  (void)rq;
  cJSON *result = cJSON_CreateObject();
  if (result) {
    cJSON_AddStringToObject(result, "daemon_version", LACED_VERSION);
    /* Unique per daemon process: clients compare it after reconnecting to
     * learn whether their connection ids still refer to this daemon. */
    static char instance[64];
    if (!instance[0]) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      snprintf(instance, sizeof(instance), "%ld-%lld-%ld", (long)getpid(),
               (long long)ts.tv_sec, (long)ts.tv_nsec);
    }
    cJSON_AddStringToObject(result, "instance", instance);
    cJSON_AddStringToObject(result, "protocol_version", LACED_PROTOCOL_VERSION);

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

/* shutdown: the reply is still sent; the event loop then stops */
static LacedHandlerResult handle_shutdown(const LacedRequest *rq) {
  LOG_INFO("Shutdown requested via RPC");
  if (rq->shutdown_flag)
    *rq->shutdown_flag = 1;
  return HANDLER_OK(cJSON_CreateObject());
}

/* connection: Get connection statistics */
static LacedHandlerResult handle_connection(const LacedRequest *rq) {
  int conn_id = 0;

  if (!laced_json_get_int(rq->params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "'conn_id' must be an integer connection id");
  }

  LacedConnStats stats;
  if (!laced_session_get_conn_stats(rq->session, conn_id, &stats)) {
    return HANDLER_ERROR(LACE_ERR_INVALID_CONN_ID, "Connection not found");
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
    if (stats.tunnel) {
      cJSON *tunnel = cJSON_AddObjectToObject(result, "tunnel");
      cJSON_AddStringToObject(tunnel, "ssh_host", stats.tunnel_host);
      cJSON_AddStringToObject(tunnel, "local_socket", stats.tunnel_socket);
    }
  }
  return HANDLER_OK(result);
}

/* cancel: the driver cancel opens a connection to the server (PostgreSQL
 * PQcancel, MySQL KILL side connection), so it runs on a worker too */
typedef struct {
  AsyncQueue *queue;
  int conn_id;
  char owner[LACED_OWNER_MAX]; /* Requesting client: may cancel only its statements */
} CancelJobArg;

static cJSON *cancel_job(LacedSession *session, void *p, char **err,
                         int *err_code) {
  CancelJobArg *a = p;
  bool running = false;
  if (!async_cancel_by_conn_id(a->queue, session, a->conn_id, a->owner,
                               &running, err)) {
    /* An unknown conn_id, or a driver that could not send the cancel */
    *err_code = LACE_ERR_QUERY_FAILED;
    return NULL;
  }
  /* "cancelled": a statement was running and the cancel was sent to it */
  cJSON *result = cJSON_CreateObject();
  cJSON_AddBoolToObject(result, "cancelled", running);
  return result;
}

static LacedHandlerResult handle_cancel(const LacedRequest *rq) {
  int conn_id = 0;

  if (!laced_json_get_int(rq->params, "conn_id", &conn_id)) {
    return HANDLER_ERROR(JSONRPC_INVALID_PARAMS, "'conn_id' must be an integer connection id");
  }

  CancelJobArg *arg = safe_malloc(sizeof(CancelJobArg));
  arg->queue = rq->queue;
  snprintf(arg->owner, sizeof(arg->owner), "%s", rq->from.owner);
  arg->conn_id = conn_id;
  AsyncQuery *job = async_job_start(rq->queue, rq->session, rq->id,
                                    &rq->from, cancel_job, arg, int_job_free);
  if (!job) {
    return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Failed to start cancel");
  }
  return HANDLER_DEFERRED(job);
}

/* ==========================================================================
 * Method Dispatch
 * ========================================================================== */

/* Method handler function type */
typedef LacedHandlerResult (*MethodHandler)(const LacedRequest *rq);

/* Method dispatch table */
static const struct {
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

LacedHandlerResult laced_handler_dispatch(const LacedRequest *rq,
                                          const char *method) {
  if (!rq || !rq->session || !method) {
    return HANDLER_ERROR(JSONRPC_INTERNAL_ERROR, "Invalid handler state");
  }
  LOG_DEBUG("RPC method: %s", method);

  for (int i = 0; g_methods[i].name != NULL; i++) {
    if (strcmp(g_methods[i].name, method) == 0) {
      return g_methods[i].handler(rq);
    }
  }

  LOG_WARN("RPC method not found: %s", method);
  return HANDLER_ERROR(JSONRPC_METHOD_NOT_FOUND, "Method not found");
}
