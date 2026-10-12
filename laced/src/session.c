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
#include "ssh_tunnel.h"
#include <util/connstr.h>
#include <util/mem.h>
#include <util/str.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <stdio.h>
#include <time.h>

/* Maximum concurrent connections */
#define MAX_CONNECTIONS LACED_MAX_CONNECTIONS

/* Connection slot */
typedef struct {
  int id;
  DbConnection *conn;
  bool in_use;
  bool reserved;          /* Slot reserved during connection setup */
  void *cancel_handle;    /* Active cancel handle during query execution */
  int64_t armed_query;    /* The query that cancel_handle belongs to */
  bool query_active;      /* True while a query is running */
  int cancel_refs;        /* Cancels in flight using cancel_handle/conn */
  void **orphans;         /* Handles finish_query released while cancels
                             were using them; freed when none is left */
  size_t num_orphans;
  SshTunnel *ssh_tunnel;  /* SSH tunnel for this connection (NULL if none) */
  /* Statistics */
  uint64_t connect_time_ms;       /* When connection was established */
  uint64_t query_count;           /* Total queries executed */
  uint64_t total_query_time_ms;   /* Cumulative query execution time */
  uint64_t failed_query_count;    /* Queries that failed */
  uint64_t current_query_start_ms; /* When current query started (0 if none) */
  /* Owner: the client that opened it (liblace "hello" id, else its socket) */
  char owner[LACED_OWNER_MAX];
  time_t orphaned_since;  /* When the owner was last seen gone (0 = present) */
  bool reaping;           /* A reaper job is closing this connection */
} ConnectionSlot;

/* Session structure */
struct LacedSession {
  pthread_mutex_t mutex;
  pthread_cond_t cancel_done; /* A connection's cancel_refs dropped to 0 */
  ConnectionSlot connections[MAX_CONNECTIONS];
  int next_conn_id;
  bool initialized;
};

/* ==========================================================================
 * Helper Functions
 * ========================================================================== */

/* Get current time in milliseconds since epoch */
static uint64_t get_time_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

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
  if (pthread_cond_init(&session->cancel_done, NULL) != 0) {
    pthread_cond_destroy(&session->cancel_done);
  pthread_mutex_destroy(&session->mutex);
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

  /* Close all connections and destroy tunnels */
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (session->connections[i].in_use && session->connections[i].conn) {
      db_disconnect(session->connections[i].conn);
      session->connections[i].conn = NULL;
      session->connections[i].in_use = false;
    }
    if (session->connections[i].ssh_tunnel) {
      ssh_tunnel_destroy(session->connections[i].ssh_tunnel);
      session->connections[i].ssh_tunnel = NULL;
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
                           const char *password, const char *owner,
                           int *conn_id, char **err) {
  if (!session || !connstr || !conn_id) {
    err_set(err, "Invalid parameters");
    return false;
  }

  char *full_connstr = str_dup(connstr);

  if (!full_connstr) {
    err_set(err, "Memory allocation failed");
    return false;
  }

  /* Parse connection string to check for SSH tunnel */
  ConnString *cs = connstr_parse(full_connstr, err);
  if (!cs) {
    str_secure_free(full_connstr); /* Holds DB/SSH passwords */
    return false;
  }
  /* lace_socket is how the daemon points a driver at its own SSH tunnel;
   * from a client it would bypass the tunnel, or reach any local socket
   * the daemon's user can (a server trusting peer authentication) */
  if (connstr_has_option(cs, "lace_socket")) {
    connstr_free(cs);
    str_secure_free(full_connstr);
    err_set(err, "The option lace_socket is internal to the daemon");
    return false;
  }

  /* A password passed separately replaces the one in the string (the
   * database password; an SSH password stays in the string) */
  bool password_override = false;
  if (password && *password && cs->driver && !str_eq(cs->driver, "sqlite")) {
    if (!cs->user || !*cs->user) {
      connstr_free(cs);
      str_secure_free(full_connstr);
      err_set(err, "A password needs a user name in the connection string");
      return false;
    }
    str_secure_free(cs->password);
    cs->password = str_dup(password);
    password_override = true;
  }

  SshTunnel *tunnel = NULL;
  char *db_connstr = NULL;

  if (cs->ssh) {
    /* Create SSH tunnel -- this is a slow operation */
    tunnel = ssh_tunnel_create(cs, err);
    if (!tunnel) {
      connstr_free(cs);
      str_secure_free(full_connstr); /* Holds DB/SSH passwords */
      return false;
    }

    /* A plain connection string through the tunnel: the driver connects to
     * its socket (option lace_socket), the port stays the database's */
    size_t n = cs->num_options;
    const char **keys = safe_calloc(n + 1, sizeof(char *));
    const char **vals = safe_calloc(n + 1, sizeof(char *));
    for (size_t i = 0; i < n; i++) {
      keys[i] = cs->option_keys[i];
      vals[i] = cs->option_values[i];
    }
    keys[n] = "lace_socket";
    vals[n] = ssh_tunnel_socket_path(tunnel);
    db_connstr = connstr_build(cs->driver, cs->user, cs->password, "localhost",
                               cs->port > 0 ? cs->port : connstr_get_port(cs),
                               cs->database, keys, vals, n + 1);
    free(keys);
    free(vals);
    connstr_free(cs);
    str_secure_free(full_connstr); /* Holds DB/SSH passwords */
    full_connstr = NULL;

    if (!db_connstr) {
      ssh_tunnel_destroy(tunnel);
      err_set(err, "Failed to build tunneled connection string");
      return false;
    }
  } else if (password_override) {
    db_connstr = connstr_build(cs->driver, cs->user, cs->password, cs->host,
                               cs->port, cs->database,
                               (const char **)cs->option_keys,
                               (const char **)cs->option_values,
                               cs->num_options);
    connstr_free(cs);
    str_secure_free(full_connstr);
    full_connstr = NULL;
    if (!db_connstr) {
      err_set(err, "Failed to build connection string");
      return false;
    }
  } else {
    connstr_free(cs);
    db_connstr = full_connstr;
    full_connstr = NULL; /* Prevent double-free */
  }

  /* Lock to find and reserve a slot */
  pthread_mutex_lock(&session->mutex);

  int slot = find_free_slot_locked(session);
  if (slot < 0) {
    pthread_mutex_unlock(&session->mutex);
    str_secure_free(db_connstr);
    if (tunnel)
      ssh_tunnel_destroy(tunnel);
    err_set(err, "Too many connections");
    return false;
  }

  /* Reserve the slot and assign ID while holding lock */
  int assigned_id = session->next_conn_id++;
  session->connections[slot].reserved = true;
  session->connections[slot].id = assigned_id;

  pthread_mutex_unlock(&session->mutex);

  /* Connect (slow operation, don't hold lock) */
  DbConnection *conn = db_connect(db_connstr, err);
  str_secure_free(db_connstr);

  /* Lock again to finalize or rollback */
  pthread_mutex_lock(&session->mutex);

  if (!conn) {
    /* Connection failed, release the reserved slot */
    session->connections[slot].reserved = false;
    session->connections[slot].id = 0;
    pthread_mutex_unlock(&session->mutex);
    if (tunnel)
      ssh_tunnel_destroy(tunnel);
    LOG_WARN("Database connection failed: %s", err && *err ? *err : "unknown error");
    return false;
  }

  /* Store connection in slot */
  session->connections[slot].conn = conn;
  session->connections[slot].in_use = true;
  session->connections[slot].reserved = false;
  session->connections[slot].query_active = false;
  session->connections[slot].cancel_handle = NULL;
  session->connections[slot].ssh_tunnel = tunnel;
  snprintf(session->connections[slot].owner,
           sizeof(session->connections[slot].owner), "%s", owner ? owner : "");
  session->connections[slot].orphaned_since = 0;
  session->connections[slot].reaping = false;
  /* Initialize statistics */
  session->connections[slot].connect_time_ms = get_time_ms();
  session->connections[slot].query_count = 0;
  session->connections[slot].total_query_time_ms = 0;
  session->connections[slot].failed_query_count = 0;
  session->connections[slot].current_query_start_ms = 0;

  pthread_mutex_unlock(&session->mutex);

  *conn_id = assigned_id;

  LOG_INFO("Database connected: id=%d driver=%s host=%s database=%s user=%s%s",
           *conn_id,
           conn->driver && conn->driver->name ? conn->driver->name : "unknown",
           conn->host ? conn->host : "local",
           conn->database ? conn->database : "unknown",
           conn->user ? conn->user : "none",
           tunnel ? " (via SSH tunnel)" : "");

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

  /* Reject disconnect if a query or a cancel still uses the connection */
  if (cs->query_active || cs->cancel_refs > 0) {
    pthread_mutex_unlock(&session->mutex);
    err_set(err, "Cannot disconnect: query in progress");
    LOG_WARN("Disconnect rejected for conn_id=%d: query in progress", conn_id);
    return false;
  }

  /* Get connection and tunnel, mark slot as not in use */
  DbConnection *conn = cs->conn;
  SshTunnel *tunnel = cs->ssh_tunnel;
  cs->conn = NULL;
  cs->ssh_tunnel = NULL;
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

  /* Destroy SSH tunnel after database disconnect */
  if (tunnel) {
    LOG_INFO("Destroying SSH tunnel for conn_id=%d", conn_id);
    ssh_tunnel_destroy(tunnel);
  }

  return true;
}

bool laced_session_check_tunnel(LacedSession *session, int conn_id,
                                char **err) {
  if (!session)
    return true;
  pthread_mutex_lock(&session->mutex);
  int slot = find_slot_by_id_locked(session, conn_id);
  SshTunnel *tunnel = slot >= 0 ? session->connections[slot].ssh_tunnel : NULL;
  DbConnection *conn = slot >= 0 ? session->connections[slot].conn : NULL;
  pthread_mutex_unlock(&session->mutex);
  /* The caller owns the connection (it is busy), so disconnect cannot
   * free the tunnel meanwhile */
  if (!tunnel || ssh_tunnel_is_alive(tunnel))
    return true;
  if (conn && conn->driver && conn->driver->connection_lost)
    conn->driver->connection_lost(conn);
  if (!ssh_tunnel_restart(tunnel, err)) {
    LOG_WARN("SSH tunnel for conn_id=%d could not be restarted: %s", conn_id,
             err && *err ? *err : "unknown error");
    return false;
  }
  return true;
}

bool laced_session_has_connection(LacedSession *session, int conn_id) {
  if (!session) {
    return false;
  }
  pthread_mutex_lock(&session->mutex);
  bool found = find_slot_by_id_locked(session, conn_id) >= 0;
  pthread_mutex_unlock(&session->mutex);
  return found;
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

DbConnection *laced_session_begin_query(LacedSession *session, int conn_id,
                                        bool *busy) {
  *busy = false;
  if (!session) {
    return NULL;
  }

  pthread_mutex_lock(&session->mutex);

  int slot = find_slot_by_id_locked(session, conn_id);
  if (slot < 0) {
    pthread_mutex_unlock(&session->mutex);
    return NULL;
  }

  ConnectionSlot *cs = &session->connections[slot];
  /* A cancel of the previous statement may still be on its way to the
   * server (PQcancel, sqlite3_interrupt run outside the lock): starting now
   * would let it stop this statement instead. Wait for it, a little. */
  if (!cs->query_active && cs->cancel_refs > 0) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += LACED_CANCEL_SETTLE_S;
    while (cs->cancel_refs > 0 &&
           pthread_cond_timedwait(&session->cancel_done, &session->mutex,
                                  &until) == 0) {
    }
    slot = find_slot_by_id_locked(session, conn_id); /* It may be gone */
    if (slot < 0) {
      pthread_mutex_unlock(&session->mutex);
      return NULL;
    }
    cs = &session->connections[slot];
  }
  if (cs->query_active || cs->cancel_refs > 0) {
    pthread_mutex_unlock(&session->mutex);
    LOG_WARN("Rejected query on conn_id=%d: another query already active", conn_id);
    *busy = true;
    return NULL;
  }

  cs->current_query_start_ms = get_time_ms();
  cs->query_active = true;
  DbConnection *conn = cs->conn;

  pthread_mutex_unlock(&session->mutex);
  return conn;
}

void laced_session_arm_cancel(LacedSession *session, int conn_id,
                              int64_t query_id) {
  if (!session) {
    return;
  }
  pthread_mutex_lock(&session->mutex);
  int slot = find_slot_by_id_locked(session, conn_id);
  ConnectionSlot *cs = slot >= 0 ? &session->connections[slot] : NULL;
  if (cs && cs->query_active && cs->conn && cs->conn->driver &&
      cs->conn->driver->prepare_cancel && !cs->cancel_handle) {
    cs->cancel_handle = cs->conn->driver->prepare_cancel(cs->conn);
    cs->armed_query = query_id;
  }
  pthread_mutex_unlock(&session->mutex);
}

bool laced_session_cancel_query(LacedSession *session, int conn_id,
                                int64_t query_id, char **err) {
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
  if (!cs->cancel_handle) {
    /* The statement is about to start (the connection is being checked):
     * the worker sees its cancel flag and does not run it */
    pthread_mutex_unlock(&session->mutex);
    return true;
  }
  if (query_id && cs->armed_query != query_id) {
    /* That statement has finished; the handle is the next one's (perhaps
     * another client's): leave it alone */
    pthread_mutex_unlock(&session->mutex);
    return true;
  }

  if (!cs->cancel_handle || !cs->conn || !cs->conn->driver ||
      !cs->conn->driver->cancel_query) {
    pthread_mutex_unlock(&session->mutex);
    err_set(err, "Cancellation not supported for this connection");
    return false;
  }

  /* Get what we need for cancel call. The reference keeps finish_query from
   * freeing the handle and disconnect from freeing conn while we use them
   * outside the lock (the query worker runs on another thread). */
  DbConnection *conn = cs->conn;
  void *cancel_handle = cs->cancel_handle;
  cs->cancel_refs++;

  pthread_mutex_unlock(&session->mutex);

  /* Cancel query (may involve network I/O, don't hold lock) */
  bool ok = conn->driver->cancel_query(conn, cancel_handle, err);

  pthread_mutex_lock(&session->mutex);
  cs->cancel_refs--;
  if (cs->cancel_refs == 0) {
    pthread_cond_broadcast(&session->cancel_done);
    for (size_t i = 0; i < cs->num_orphans; i++) {
      if (conn->driver->free_cancel_handle)
        conn->driver->free_cancel_handle(cs->orphans[i]);
    }
    free(cs->orphans);
    cs->orphans = NULL;
    cs->num_orphans = 0;
  }
  pthread_mutex_unlock(&session->mutex);

  return ok;
}

void laced_session_finish_query(LacedSession *session, int conn_id, bool success) {
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

  /* Update statistics */
  if (cs->current_query_start_ms > 0) {
    uint64_t duration = get_time_ms() - cs->current_query_start_ms;
    cs->total_query_time_ms += duration;
    cs->query_count++;
    if (!success) {
      cs->failed_query_count++;
    }
  }
  cs->current_query_start_ms = 0;

  if (cs->cancel_handle && cs->cancel_refs > 0) {
    /* A cancel is using this handle right now; it frees it when done */
    /* Overlapping slow cancels can leave several: keep them all */
    cs->orphans = safe_reallocarray(cs->orphans, cs->num_orphans + 1, sizeof(void *));
    cs->orphans[cs->num_orphans++] = cs->cancel_handle;
    if (cs->conn && cs->conn->driver &&
        cs->conn->driver->cancel_handle_finished) {
      cs->conn->driver->cancel_handle_finished(cs->cancel_handle);
    }
  } else if (cs->cancel_handle && cs->conn && cs->conn->driver &&
             cs->conn->driver->free_cancel_handle) {
    cs->conn->driver->free_cancel_handle(cs->cancel_handle);
  }
  cs->cancel_handle = NULL;
  cs->query_active = false;

  pthread_mutex_unlock(&session->mutex);
}

size_t laced_session_collect_orphans(LacedSession *session,
                                     const char *const *live_owners,
                                     size_t num_live, int grace_seconds,
                                     int *out_ids, size_t max_ids) {
  if (!session) {
    return 0;
  }
  time_t now = time(NULL);
  size_t n = 0;
  pthread_mutex_lock(&session->mutex);
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    ConnectionSlot *cs = &session->connections[i];
    if (!cs->in_use || !cs->owner[0]) {
      continue;
    }
    bool live = false;
    for (size_t k = 0; k < num_live && !live; k++) {
      live = live_owners[k] && strcmp(live_owners[k], cs->owner) == 0;
    }
    if (live) {
      cs->orphaned_since = 0;
      continue;
    }
    if (cs->orphaned_since == 0) {
      cs->orphaned_since = now;
      LOG_INFO("conn_id=%d: owner gone, closing in %ds unless it returns",
               cs->id, grace_seconds);
    }
    if (!cs->reaping && now - cs->orphaned_since >= grace_seconds &&
        n < max_ids) {
      cs->reaping = true;
      out_ids[n++] = cs->id;
    }
  }
  pthread_mutex_unlock(&session->mutex);
  return n;
}

void laced_session_reap_failed(LacedSession *session, int conn_id) {
  if (!session) {
    return;
  }
  pthread_mutex_lock(&session->mutex);
  int slot = find_slot_by_id_locked(session, conn_id);
  if (slot >= 0) {
    session->connections[slot].reaping = false; /* Retry on a later pass */
  }
  pthread_mutex_unlock(&session->mutex);
}

bool laced_session_reap_continue(LacedSession *session, int conn_id) {
  if (!session) {
    return false;
  }
  pthread_mutex_lock(&session->mutex);
  int slot = find_slot_by_id_locked(session, conn_id);
  bool go = slot >= 0 && session->connections[slot].orphaned_since != 0;
  if (slot >= 0 && !go) {
    session->connections[slot].reaping = false;
  }
  pthread_mutex_unlock(&session->mutex);
  return go;
}

bool laced_session_get_conn_stats(LacedSession *session, int conn_id,
                                  LacedConnStats *stats) {
  if (!session || !stats) {
    return false;
  }

  pthread_mutex_lock(&session->mutex);

  int slot = find_slot_by_id_locked(session, conn_id);
  if (slot < 0) {
    pthread_mutex_unlock(&session->mutex);
    return false;
  }

  ConnectionSlot *cs = &session->connections[slot];
  uint64_t now = get_time_ms();

  stats->uptime_ms = now - cs->connect_time_ms;
  stats->query_count = cs->query_count;
  stats->total_query_time_ms = cs->total_query_time_ms;
  stats->failed_query_count = cs->failed_query_count;
  stats->query_running = cs->query_active;

  if (cs->query_active && cs->current_query_start_ms > 0) {
    stats->current_query_time_ms = now - cs->current_query_start_ms;
  } else {
    stats->current_query_time_ms = 0;
  }

  stats->tunnel = cs->ssh_tunnel != NULL;
  /* Host and port only: whether ssh is alive is checked (and reaped,
   * restarted) by the worker that owns the connection, outside this lock */
  const char *sock = ssh_tunnel_socket_path(cs->ssh_tunnel);
  snprintf(stats->tunnel_socket, sizeof(stats->tunnel_socket), "%s",
           sock ? sock : "");
  const char *host = ssh_tunnel_host(cs->ssh_tunnel);
  snprintf(stats->tunnel_host, sizeof(stats->tunnel_host), "%s",
           host ? host : "");

  pthread_mutex_unlock(&session->mutex);
  return true;
}

void laced_session_stop_tunnels(LacedSession *session) {
  if (!session)
    return;
  /* No lock: a stuck worker may hold it, and the process is exiting */
  for (int i = 0; i < LACED_MAX_CONNECTIONS; i++) {
    if (session->connections[i].ssh_tunnel)
      ssh_tunnel_signal_exit(session->connections[i].ssh_tunnel);
  }
}
