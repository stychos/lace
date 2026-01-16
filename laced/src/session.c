/*
 * laced - Lace Database Daemon
 * Session/connection pool manager implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "session.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

/* Maximum concurrent connections */
#define MAX_CONNECTIONS 64

/* Connection slot */
typedef struct {
  int id;
  DbConnection *conn;
  bool in_use;
  void *cancel_handle;  /* Active cancel handle during query execution */
  bool query_active;    /* True while a query is running */
} ConnectionSlot;

/* Session structure */
struct LacedSession {
  ConnectionSlot connections[MAX_CONNECTIONS];
  int next_conn_id;
  bool initialized;
};

/* ==========================================================================
 * Session Lifecycle
 * ========================================================================== */

LacedSession *laced_session_create(void) {
  LacedSession *session = calloc(1, sizeof(LacedSession));
  if (!session) {
    return NULL;
  }

  /* Initialize database drivers */
  db_init();

  session->next_conn_id = 1;
  session->initialized = true;

  return session;
}

void laced_session_destroy(LacedSession *session) {
  if (!session) {
    return;
  }

  /* Close all connections */
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (session->connections[i].in_use && session->connections[i].conn) {
      db_disconnect(session->connections[i].conn);
    }
  }

  /* Cleanup database subsystem */
  db_cleanup();

  free(session);
}

/* ==========================================================================
 * Connection Management
 * ========================================================================== */

/* Find a free slot */
static int find_free_slot(LacedSession *session) {
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (!session->connections[i].in_use) {
      return i;
    }
  }
  return -1;
}

/* Find slot by connection ID */
static int find_slot_by_id(LacedSession *session, int conn_id) {
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (session->connections[i].in_use &&
        session->connections[i].id == conn_id) {
      return i;
    }
  }
  return -1;
}

bool laced_session_connect(LacedSession *session, const char *connstr,
                           const char *password, int *conn_id, char **err) {
  if (!session || !connstr || !conn_id) {
    err_set(err, "Invalid parameters");
    return false;
  }

  /* Find a free slot */
  int slot = find_free_slot(session);
  if (slot < 0) {
    err_set(err, "Too many connections");
    return false;
  }

  /* Build connection string with password if provided */
  char *full_connstr = NULL;
  if (password && *password) {
    /* TODO: Properly inject password into connection string */
    full_connstr = str_dup(connstr);
  } else {
    full_connstr = str_dup(connstr);
  }

  if (!full_connstr) {
    err_set(err, "Memory allocation failed");
    return false;
  }

  /* Connect */
  DbConnection *conn = db_connect(full_connstr, err);
  free(full_connstr);

  if (!conn) {
    return false;
  }

  /* Store in slot */
  session->connections[slot].id = session->next_conn_id++;
  session->connections[slot].conn = conn;
  session->connections[slot].in_use = true;

  *conn_id = session->connections[slot].id;
  return true;
}

bool laced_session_disconnect(LacedSession *session, int conn_id, char **err) {
  if (!session) {
    err_set(err, "Invalid parameters");
    return false;
  }

  int slot = find_slot_by_id(session, conn_id);
  if (slot < 0) {
    err_set(err, "Connection not found");
    return false;
  }

  /* Disconnect and clear slot */
  if (session->connections[slot].conn) {
    db_disconnect(session->connections[slot].conn);
  }
  session->connections[slot].conn = NULL;
  session->connections[slot].in_use = false;
  session->connections[slot].id = 0;

  return true;
}

DbConnection *laced_session_get_connection(LacedSession *session, int conn_id) {
  if (!session) {
    return NULL;
  }

  int slot = find_slot_by_id(session, conn_id);
  if (slot < 0) {
    return NULL;
  }

  return session->connections[slot].conn;
}

size_t laced_session_connection_count(LacedSession *session) {
  if (!session) {
    return 0;
  }

  size_t count = 0;
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (session->connections[i].in_use) {
      count++;
    }
  }
  return count;
}

/* ==========================================================================
 * Connection Info
 * ========================================================================== */

bool laced_session_list_connections(LacedSession *session, LacedConnInfo **info,
                                    size_t *count) {
  if (!session || !info || !count) {
    return false;
  }

  size_t num_conns = laced_session_connection_count(session);
  if (num_conns == 0) {
    *info = NULL;
    *count = 0;
    return true;
  }

  LacedConnInfo *result = calloc(num_conns, sizeof(LacedConnInfo));
  if (!result) {
    return false;
  }

  size_t idx = 0;
  for (int i = 0; i < MAX_CONNECTIONS && idx < num_conns; i++) {
    if (!session->connections[i].in_use) {
      continue;
    }

    DbConnection *conn = session->connections[i].conn;
    result[idx].id = session->connections[i].id;

    if (conn) {
      if (conn->driver && conn->driver->name) {
        result[idx].driver = str_dup(conn->driver->name);
      }
      if (conn->database) {
        result[idx].database = str_dup(conn->database);
      }
      if (conn->host) {
        result[idx].host = str_dup(conn->host);
      }
      result[idx].port = conn->port;
      if (conn->user) {
        result[idx].user = str_dup(conn->user);
      }
    }

    idx++;
  }

  *info = result;
  *count = num_conns;
  return true;
}

void laced_conn_info_array_free(LacedConnInfo *info, size_t count) {
  if (!info) {
    return;
  }

  for (size_t i = 0; i < count; i++) {
    free(info[i].driver);
    free(info[i].database);
    free(info[i].host);
    free(info[i].user);
  }
  free(info);
}

/* ==========================================================================
 * Query Cancellation
 * ========================================================================== */

bool laced_session_prepare_cancel(LacedSession *session, int conn_id) {
  if (!session) {
    return false;
  }

  int slot = find_slot_by_id(session, conn_id);
  if (slot < 0) {
    return false;
  }

  ConnectionSlot *cs = &session->connections[slot];
  if (!cs->conn || !cs->conn->driver || !cs->conn->driver->prepare_cancel) {
    return false;
  }

  /* Free any existing cancel handle */
  if (cs->cancel_handle && cs->conn->driver->free_cancel_handle) {
    cs->conn->driver->free_cancel_handle(cs->cancel_handle);
  }

  cs->cancel_handle = cs->conn->driver->prepare_cancel(cs->conn);
  cs->query_active = true;
  return cs->cancel_handle != NULL;
}

bool laced_session_cancel_query(LacedSession *session, int conn_id, char **err) {
  if (!session) {
    err_set(err, "Invalid session");
    return false;
  }

  int slot = find_slot_by_id(session, conn_id);
  if (slot < 0) {
    err_set(err, "Invalid connection ID");
    return false;
  }

  ConnectionSlot *cs = &session->connections[slot];
  if (!cs->query_active) {
    /* No query running - not an error, just nothing to cancel */
    return true;
  }

  if (!cs->cancel_handle || !cs->conn || !cs->conn->driver ||
      !cs->conn->driver->cancel_query) {
    err_set(err, "Cancellation not supported for this connection");
    return false;
  }

  return cs->conn->driver->cancel_query(cs->conn, cs->cancel_handle, err);
}

void laced_session_finish_query(LacedSession *session, int conn_id) {
  if (!session) {
    return;
  }

  int slot = find_slot_by_id(session, conn_id);
  if (slot < 0) {
    return;
  }

  ConnectionSlot *cs = &session->connections[slot];
  if (cs->cancel_handle && cs->conn && cs->conn->driver &&
      cs->conn->driver->free_cancel_handle) {
    cs->conn->driver->free_cancel_handle(cs->cancel_handle);
  }
  cs->cancel_handle = NULL;
  cs->query_active = false;
}
