/*
 * laced - Lace Database Daemon
 * Session/connection pool manager implementation
 *
 * Thread-safe connection pool with mutex protection.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "session.h"
#include "log.h"
#include <util/mem.h>
#include <util/str.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Maximum concurrent connections */
#define MAX_CONNECTIONS 64

/* Connection slot */
typedef struct {
  int id;
  DbConnection *conn;
  bool in_use;
  bool reserved;          /* Slot reserved during connection setup */
  void *cancel_handle;    /* Active cancel handle during query execution */
  bool query_active;      /* True while a query is running */
} ConnectionSlot;

/* Session structure */
struct LacedSession {
  pthread_mutex_t mutex;
  ConnectionSlot connections[MAX_CONNECTIONS];
  int next_conn_id;
  bool initialized;
};

/* ==========================================================================
 * Session Lifecycle
 * ========================================================================== */

LacedSession *laced_session_create(void) {
  LacedSession *session = safe_calloc(1, sizeof(LacedSession));
  if (!session) {
    return NULL;
  }

  /* Initialize mutex */
  if (pthread_mutex_init(&session->mutex, NULL) != 0) {
    free(session);
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

  pthread_mutex_lock(&session->mutex);

  /* Close all connections */
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (session->connections[i].in_use && session->connections[i].conn) {
      db_disconnect(session->connections[i].conn);
      session->connections[i].conn = NULL;
      session->connections[i].in_use = false;
    }
  }

  pthread_mutex_unlock(&session->mutex);

  /* Cleanup database subsystem */
  db_cleanup();

  pthread_mutex_destroy(&session->mutex);
  free(session);
}

/* ==========================================================================
 * Connection Management
 * ========================================================================== */

/* Find a free slot (must be called with mutex held) */
static int find_free_slot_locked(LacedSession *session) {
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (!session->connections[i].in_use && !session->connections[i].reserved) {
      return i;
    }
  }
  return -1;
}

/* Find slot by connection ID (must be called with mutex held) */
static int find_slot_by_id_locked(LacedSession *session, int conn_id) {
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

  /* Lock to find and reserve a slot */
  pthread_mutex_lock(&session->mutex);

  int slot = find_free_slot_locked(session);
  if (slot < 0) {
    pthread_mutex_unlock(&session->mutex);
    free(full_connstr);
    err_set(err, "Too many connections");
    return false;
  }

  /* Reserve the slot and assign ID while holding lock */
  int assigned_id = session->next_conn_id++;
  session->connections[slot].reserved = true;
  session->connections[slot].id = assigned_id;

  pthread_mutex_unlock(&session->mutex);

  /* Connect (slow operation, don't hold lock) */
  DbConnection *conn = db_connect(full_connstr, err);
  free(full_connstr);

  /* Lock again to finalize or rollback */
  pthread_mutex_lock(&session->mutex);

  if (!conn) {
    /* Connection failed, release the reserved slot */
    session->connections[slot].reserved = false;
    session->connections[slot].id = 0;
    pthread_mutex_unlock(&session->mutex);
    LOG_WARN("Database connection failed: %s", err && *err ? *err : "unknown error");
    return false;
  }

  /* Store connection in slot */
  session->connections[slot].conn = conn;
  session->connections[slot].in_use = true;
  session->connections[slot].reserved = false;
  session->connections[slot].query_active = false;
  session->connections[slot].cancel_handle = NULL;

  pthread_mutex_unlock(&session->mutex);

  *conn_id = assigned_id;

  LOG_INFO("Database connected: id=%d driver=%s host=%s database=%s user=%s",
           *conn_id,
           conn->driver && conn->driver->name ? conn->driver->name : "unknown",
           conn->host ? conn->host : "local",
           conn->database ? conn->database : "unknown",
           conn->user ? conn->user : "none");

  return true;
}

bool laced_session_disconnect(LacedSession *session, int conn_id, char **err) {
  if (!session) {
    err_set(err, "Invalid parameters");
    return false;
  }

  pthread_mutex_lock(&session->mutex);

  int slot = find_slot_by_id_locked(session, conn_id);
  if (slot < 0) {
    pthread_mutex_unlock(&session->mutex);
    err_set(err, "Connection not found");
    return false;
  }

  ConnectionSlot *cs = &session->connections[slot];

  /* Reject disconnect if query is active */
  if (cs->query_active) {
    pthread_mutex_unlock(&session->mutex);
    err_set(err, "Cannot disconnect: query in progress");
    LOG_WARN("Disconnect rejected for conn_id=%d: query in progress", conn_id);
    return false;
  }

  /* Get connection and mark slot as not in use */
  DbConnection *conn = cs->conn;
  cs->conn = NULL;
  cs->in_use = false;
  cs->id = 0;

  /* Log connection info before we lose it */
  const char *driver_name = conn && conn->driver && conn->driver->name
                                ? conn->driver->name : "unknown";
  const char *database_name = conn && conn->database ? conn->database : "unknown";

  pthread_mutex_unlock(&session->mutex);

  /* Disconnect (slow operation, don't hold lock) */
  if (conn) {
    LOG_INFO("Database disconnecting: id=%d driver=%s database=%s",
             conn_id, driver_name, database_name);
    db_disconnect(conn);
  }

  return true;
}

DbConnection *laced_session_get_connection(LacedSession *session, int conn_id) {
  if (!session) {
    return NULL;
  }

  pthread_mutex_lock(&session->mutex);

  int slot = find_slot_by_id_locked(session, conn_id);
  DbConnection *conn = (slot >= 0) ? session->connections[slot].conn : NULL;

  pthread_mutex_unlock(&session->mutex);

  return conn;
}

size_t laced_session_connection_count(LacedSession *session) {
  if (!session) {
    return 0;
  }

  pthread_mutex_lock(&session->mutex);

  size_t count = 0;
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (session->connections[i].in_use) {
      count++;
    }
  }

  pthread_mutex_unlock(&session->mutex);

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

  pthread_mutex_lock(&session->mutex);

  /* Count connections */
  size_t num_conns = 0;
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (session->connections[i].in_use) {
      num_conns++;
    }
  }

  if (num_conns == 0) {
    pthread_mutex_unlock(&session->mutex);
    *info = NULL;
    *count = 0;
    return true;
  }

  LacedConnInfo *result = safe_calloc(num_conns, sizeof(LacedConnInfo));
  if (!result) {
    pthread_mutex_unlock(&session->mutex);
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

  pthread_mutex_unlock(&session->mutex);

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
 * Query State Management
 * ========================================================================== */

bool laced_session_is_conn_busy(LacedSession *session, int conn_id) {
  if (!session) {
    return false;
  }

  pthread_mutex_lock(&session->mutex);

  int slot = find_slot_by_id_locked(session, conn_id);
  bool busy = (slot >= 0) && session->connections[slot].query_active;

  pthread_mutex_unlock(&session->mutex);

  return busy;
}

bool laced_session_prepare_cancel(LacedSession *session, int conn_id) {
  if (!session) {
    return false;
  }

  pthread_mutex_lock(&session->mutex);

  int slot = find_slot_by_id_locked(session, conn_id);
  if (slot < 0) {
    pthread_mutex_unlock(&session->mutex);
    return false;
  }

  ConnectionSlot *cs = &session->connections[slot];

  /* Check if connection is already busy */
  if (cs->query_active) {
    pthread_mutex_unlock(&session->mutex);
    LOG_WARN("Rejected query on conn_id=%d: another query already active", conn_id);
    return false;
  }

  if (!cs->conn || !cs->conn->driver || !cs->conn->driver->prepare_cancel) {
    /* Mark as active even without cancel support */
    cs->query_active = true;
    pthread_mutex_unlock(&session->mutex);
    return true;
  }

  /* Free any existing cancel handle */
  if (cs->cancel_handle && cs->conn->driver->free_cancel_handle) {
    cs->conn->driver->free_cancel_handle(cs->cancel_handle);
  }

  cs->cancel_handle = cs->conn->driver->prepare_cancel(cs->conn);
  cs->query_active = true;

  pthread_mutex_unlock(&session->mutex);

  return true;
}

bool laced_session_cancel_query(LacedSession *session, int conn_id, char **err) {
  if (!session) {
    err_set(err, "Invalid session");
    return false;
  }

  pthread_mutex_lock(&session->mutex);

  int slot = find_slot_by_id_locked(session, conn_id);
  if (slot < 0) {
    pthread_mutex_unlock(&session->mutex);
    err_set(err, "Invalid connection ID");
    return false;
  }

  ConnectionSlot *cs = &session->connections[slot];
  if (!cs->query_active) {
    pthread_mutex_unlock(&session->mutex);
    /* No query running - not an error, just nothing to cancel */
    return true;
  }

  if (!cs->cancel_handle || !cs->conn || !cs->conn->driver ||
      !cs->conn->driver->cancel_query) {
    pthread_mutex_unlock(&session->mutex);
    err_set(err, "Cancellation not supported for this connection");
    return false;
  }

  /* Get what we need for cancel call */
  DbConnection *conn = cs->conn;
  void *cancel_handle = cs->cancel_handle;

  pthread_mutex_unlock(&session->mutex);

  /* Cancel query (may involve network I/O, don't hold lock) */
  return conn->driver->cancel_query(conn, cancel_handle, err);
}

void laced_session_finish_query(LacedSession *session, int conn_id) {
  if (!session) {
    return;
  }

  pthread_mutex_lock(&session->mutex);

  int slot = find_slot_by_id_locked(session, conn_id);
  if (slot < 0) {
    pthread_mutex_unlock(&session->mutex);
    return;
  }

  ConnectionSlot *cs = &session->connections[slot];
  if (cs->cancel_handle && cs->conn && cs->conn->driver &&
      cs->conn->driver->free_cancel_handle) {
    cs->conn->driver->free_cancel_handle(cs->cancel_handle);
  }
  cs->cancel_handle = NULL;
  cs->query_active = false;

  pthread_mutex_unlock(&session->mutex);
}
