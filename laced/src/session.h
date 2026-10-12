/*
 * laced - Lace Database Daemon
 * Session/connection pool manager
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACED_SESSION_H
#define LACED_SESSION_H

#include "db/db.h"
#include "constants.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration */
typedef struct LacedSession LacedSession;

/* ==========================================================================
 * Session Lifecycle
 * ========================================================================== */

/*
 * Create a new session manager.
 * Initializes the database driver subsystem.
 *
 * @return  Session handle, or NULL on failure
 */
LacedSession *laced_session_create(void);

/*
 * Destroy session manager and close all connections.
 *
 * @param session  Session handle (NULL is safe)
 */
void laced_session_destroy(LacedSession *session);

/* ==========================================================================
 * Connection Management
 * ========================================================================== */

/*
 * Open a new database connection.
 *
 * @param session   Session handle
 * @param connstr   Connection string
 * @param password  Password (NULL if embedded in connstr or not needed)
 * @param conn_id   Output: connection ID on success
 * @param err       Output: error message on failure (caller must free)
 * @return          true on success, false on failure
 */
bool laced_session_connect(LacedSession *session, const char *connstr,
                           const char *password, const char *owner,
                           int *conn_id, char **err);

/*
 * Orphan reaping. Connections belong to the daemon, not to a socket, so they
 * survive a client reconnecting; but a client that is gone for good would
 * leave them (and any open transaction holding locks) behind forever.
 * Marks every connection whose owner is not in live_owners as orphaned, and
 * returns (up to max_ids) those orphaned for at least grace_seconds that are
 * not already being reaped, flagging them as being reaped.
 */
size_t laced_session_collect_orphans(LacedSession *session,
                                     const char *const *live_owners,
                                     size_t num_live, int grace_seconds,
                                     int *out_ids, size_t max_ids);

/* A reaper could not close conn_id (e.g. its query would not stop): allow a
 * later pass to try again. */
void laced_session_reap_failed(LacedSession *session, int conn_id);

/* Whether a reaper should keep at conn_id: false (and the reaping mark
 * cleared) once its owner is back or the connection is gone. Checked before
 * every attempt, so a client that returns keeps its connection. */
bool laced_session_reap_continue(LacedSession *session, int conn_id);

/*
 * Close a database connection.
 *
 * @param session  Session handle
 * @param conn_id  Connection ID to close
 * @param err      Output: error message on failure (caller must free)
 * @return         true on success, false on failure
 */
bool laced_session_disconnect(LacedSession *session, int conn_id, char **err);

/* Shutdown without teardown: ask every tunnel's ssh to exit (nothing is
 * freed; workers may still use the connections) */
void laced_session_stop_tunnels(LacedSession *session);

/* True if conn_id is an open connection */
bool laced_session_has_connection(LacedSession *session, int conn_id);

/*
 * Before a statement on conn_id (the caller holds it busy): if its SSH
 * tunnel died, tell the driver the connection is gone and start the tunnel
 * again on the same local port. False (with err) if the restart failed.
 */
bool laced_session_check_tunnel(LacedSession *session, int conn_id,
                                char **err);

/*
 * Get the number of active connections.
 *
 * @param session  Session handle
 * @return         Number of connections
 */
size_t laced_session_connection_count(LacedSession *session);

/* ==========================================================================
 * Connection Info
 * ========================================================================== */

/* Connection information for listing */
typedef struct {
  int id;
  char *driver;
  char *database;
  char *host;
  int port;
  char *user;
} LacedConnInfo;

/* Connection statistics */
typedef struct {
  uint64_t uptime_ms;              /* Time since connection was established */
  uint64_t query_count;            /* Total queries executed */
  uint64_t total_query_time_ms;    /* Cumulative query execution time */
  uint64_t failed_query_count;     /* Queries that failed */
  bool query_running;              /* True if a query is currently running */
  uint64_t current_query_time_ms;       /* Duration of current query (0 if not running) */
  bool tunnel;                     /* Connected through an SSH tunnel */
  char tunnel_socket[256]; /* The local socket the tunnel listens on */
  char tunnel_host[256];           /* SSH server */
} LacedConnStats;

/*
 * List all active connections.
 *
 * @param session  Session handle
 * @param info     Output: array of connection info (caller must free with
 *                 laced_conn_info_array_free)
 * @param count    Output: number of connections
 * @return         true on success, false on failure
 */
bool laced_session_list_connections(LacedSession *session, LacedConnInfo **info,
                                    size_t *count);

/*
 * Free connection info array.
 *
 * @param info   Array to free
 * @param count  Number of elements
 */
void laced_conn_info_array_free(LacedConnInfo *info, size_t count);

/*
 * Get connection statistics.
 *
 * @param session  Session handle
 * @param conn_id  Connection ID
 * @param stats    Output: connection statistics
 * @return         true if connection found, false otherwise
 */
bool laced_session_get_conn_stats(LacedSession *session, int conn_id,
                                  LacedConnStats *stats);

/* ==========================================================================
 * Query State Management
 * ========================================================================== */

/* Start a statement on conn_id (the daemon runs one statement per
 * connection): marks it busy and returns its DbConnection, which stays valid
 * until laced_session_finish_query, because disconnect refuses a busy
 * connection. This is the only way to reach a DbConnection outside the
 * session. NULL if conn_id is unknown (*busy false) or already running a
 * statement (*busy true). */
DbConnection *laced_session_begin_query(LacedSession *session, int conn_id,
                                        bool *busy);

/* Take the driver's cancel handle for the statement begun on conn_id. Call
 * after the driver's ensure_ready hook, so a reconnected connection is the
 * one a cancel reaches. */
void laced_session_arm_cancel(LacedSession *session, int conn_id,
                              int64_t query_id);

/*
 * Cancel the current query on a connection.
 * This should be called from a different thread than the one executing the query.
 *
 * @param session  Session handle
 * @param conn_id  Connection ID
 * @param query_id Only if this query's handle is armed (0: whatever runs)
 * @param err      Output: error message on failure (caller must free)
 * @return         true if cancellation was sent
 */
bool laced_session_cancel_query(LacedSession *session, int conn_id,
                                int64_t query_id, char **err);

/*
 * Clean up cancellation state after a query completes.
 * Call this after each query (success or failure).
 * Updates connection statistics.
 *
 * @param session  Session handle
 * @param conn_id  Connection ID
 * @param success  true if query succeeded, false if it failed
 */
void laced_session_finish_query(LacedSession *session, int conn_id, bool success);

#endif /* LACED_SESSION_H */
