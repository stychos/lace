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
#include <stdbool.h>
#include <stddef.h>

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
                           const char *password, int *conn_id, char **err);

/*
 * Close a database connection.
 *
 * @param session  Session handle
 * @param conn_id  Connection ID to close
 * @param err      Output: error message on failure (caller must free)
 * @return         true on success, false on failure
 */
bool laced_session_disconnect(LacedSession *session, int conn_id, char **err);

/*
 * Get a database connection by ID.
 *
 * @param session  Session handle
 * @param conn_id  Connection ID
 * @return         Connection handle, or NULL if not found
 */
DbConnection *laced_session_get_connection(LacedSession *session, int conn_id);

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

/* ==========================================================================
 * Query Cancellation
 * ========================================================================== */

/*
 * Prepare cancellation handle before executing a query.
 * Call this before starting a query to enable cancellation.
 *
 * @param session  Session handle
 * @param conn_id  Connection ID
 * @return         true if cancel handle was prepared
 */
bool laced_session_prepare_cancel(LacedSession *session, int conn_id);

/*
 * Cancel the current query on a connection.
 * This should be called from a different thread than the one executing the query.
 *
 * @param session  Session handle
 * @param conn_id  Connection ID
 * @param err      Output: error message on failure (caller must free)
 * @return         true if cancellation was sent
 */
bool laced_session_cancel_query(LacedSession *session, int conn_id, char **err);

/*
 * Clean up cancellation state after a query completes.
 * Call this after each query (success or failure).
 *
 * @param session  Session handle
 * @param conn_id  Connection ID
 */
void laced_session_finish_query(LacedSession *session, int conn_id);

#endif /* LACED_SESSION_H */
