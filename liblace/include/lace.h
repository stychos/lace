/*
 * liblace - Lace Client Library
 * Main public API header
 *
 * This library provides a C interface for communicating with the laced daemon.
 * It handles process spawning, IPC, and JSON-RPC marshaling.
 *
 * Usage:
 *   #include <lace.h>
 *
 *   lace_client_t *client = lace_client_create(NULL);  // Use default daemon path
 *   if (!client) { handle error }
 *
 *   int conn_id;
 *   int err = lace_connect(client, "sqlite:///data.db", NULL, &conn_id);
 *   if (err != LACE_OK) { handle error }
 *
 *   LaceResult *result;
 *   err = lace_query(client, conn_id, "users", NULL, 0, NULL, 0, 0, 100, &result);
 *   if (err == LACE_OK) {
 *     // Use result...
 *     lace_result_free(result);
 *   }
 *
 *   lace_client_destroy(client);
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_H
#define LIBLACE_H

#include "types.h"
#include "error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Client Handle
 * ========================================================================== */

/* Opaque client handle */
typedef struct lace_client lace_client_t;

/* ==========================================================================
 * Connection Configuration
 * ========================================================================== */

/* Connection mode for client-daemon communication */
typedef enum {
  LACE_CONN_SPAWN,    /* Spawn dedicated daemon (default, current behavior) */
  LACE_CONN_UNIX,     /* Connect to existing Unix socket */
  LACE_CONN_TCP       /* Connect to existing TCP socket */
} LaceConnMode;

/* Preferred spawn mode when starting a new daemon */
typedef enum {
  LACE_SPAWN_UNIX,   /* Unix socket mode (default, multi-client) */
  LACE_SPAWN_STDIO,  /* Stdio pipes (single client, auto-exit on EOF) */
  LACE_SPAWN_TCP,    /* TCP socket mode (network, multi-client) */
  LACE_SPAWN_NONE    /* Don't spawn, connect to existing daemon only */
} LaceSpawnMode;

/* Client configuration for extended creation */
typedef struct {
  LaceConnMode mode;          /* Connection mode */
  const char *socket_path;    /* Unix socket path (NULL = auto-detect) */
  const char *host;           /* TCP host (NULL = localhost) */
  int port;                   /* TCP port (0 = default 7433) */
  const char *daemon_path;    /* Path to laced for spawn mode */
  bool spawn_if_missing;      /* Spawn daemon if socket not found */
  int connect_timeout_ms;     /* Connection timeout (0 = default 5000) */
  LaceSpawnMode spawn_mode;   /* Preferred mode when spawning daemon */
} LaceClientConfig;

/* Default values for config */
#define LACE_DEFAULT_PORT 7433
#define LACE_DEFAULT_CONNECT_TIMEOUT_MS 5000

/* ==========================================================================
 * Client Lifecycle
 * ========================================================================== */

/*
 * Create a new client and spawn the laced daemon.
 * Uses Unix socket mode by default.
 *
 * @param daemon_path  Path to laced executable, or NULL to search PATH
 * @return             Client handle, or NULL on failure (check errno)
 */
lace_client_t *lace_client_create(const char *daemon_path);

/*
 * Create a new client with specified spawn mode.
 *
 * @param daemon_path  Path to laced executable, or NULL to search PATH
 * @param spawn_mode   LACE_SPAWN_UNIX (multi-client) or LACE_SPAWN_STDIO (single-client)
 * @return             Client handle, or NULL on failure (check errno)
 */
lace_client_t *lace_client_create_ex(const char *daemon_path, LaceSpawnMode spawn_mode);

/*
 * Create a new client with extended configuration.
 * Allows connecting to existing daemon via socket instead of spawning.
 *
 * @param config  Client configuration (NULL for defaults, same as lace_client_create(NULL))
 * @return        Client handle, or NULL on failure (check lace_client_error)
 */
lace_client_t *lace_client_create_with_config(const LaceClientConfig *config);

/*
 * Connect to an existing daemon via Unix socket.
 *
 * @param socket_path  Path to Unix socket (NULL = auto-detect)
 * @return             Client handle, or NULL on failure
 */
lace_client_t *lace_client_connect(const char *socket_path);

/*
 * Connect to an existing daemon via TCP.
 *
 * @param host  Hostname or IP (NULL = localhost)
 * @param port  Port number (0 = default 7433)
 * @return      Client handle, or NULL on failure
 */
lace_client_t *lace_client_connect_tcp(const char *host, int port);

/*
 * Destroy client and terminate the daemon process.
 *
 * @param client  Client handle (NULL is safe)
 */
void lace_client_destroy(lace_client_t *client);

/*
 * Check if client is connected to daemon.
 *
 * @param client  Client handle
 * @return        true if connected, false otherwise
 */
bool lace_client_connected(const lace_client_t *client);

/*
 * Get last error message from client.
 *
 * @param client  Client handle
 * @return        Error message string (valid until next call)
 */
const char *lace_client_error(const lace_client_t *client);

/* ==========================================================================
 * Database Connection
 * ========================================================================== */

/*
 * Open a database connection.
 *
 * @param client    Client handle
 * @param connstr   Connection string (e.g., "sqlite:///path.db", "postgres://user@host/db")
 * @param password  Password (NULL if not needed or embedded in connstr)
 * @param conn_id   Output: connection ID on success
 * @return          LACE_OK on success, error code on failure
 */
int lace_connect(lace_client_t *client, const char *connstr, const char *password, int *conn_id);

/*
 * Close a database connection.
 *
 * @param client   Client handle
 * @param conn_id  Connection ID to close
 * @return         LACE_OK on success, error code on failure
 */
int lace_disconnect(lace_client_t *client, int conn_id);

/*
 * Reconnect a database connection.
 *
 * @param client   Client handle
 * @param conn_id  Connection ID to reconnect
 * @param password New password (NULL to reuse previous)
 * @return         LACE_OK on success, error code on failure
 */
int lace_reconnect(lace_client_t *client, int conn_id, const char *password);

/*
 * List all active connections.
 *
 * @param client  Client handle
 * @param info    Output: array of connection info (caller must free with lace_conn_info_array_free)
 * @param count   Output: number of connections
 * @return        LACE_OK on success, error code on failure
 */
int lace_list_connections(lace_client_t *client, LaceConnInfo **info, size_t *count);

/* Free connection info array */
void lace_conn_info_array_free(LaceConnInfo *info, size_t count);

/* ==========================================================================
 * Schema Discovery
 * ========================================================================== */

/*
 * List tables in a database.
 *
 * @param client   Client handle
 * @param conn_id  Connection ID
 * @param tables   Output: array of table names (caller must free with lace_tables_free)
 * @param count    Output: number of tables
 * @return         LACE_OK on success, error code on failure
 */
int lace_list_tables(lace_client_t *client, int conn_id, char ***tables, size_t *count);

/* Free table name array */
void lace_tables_free(char **tables, size_t count);

/*
 * Get table schema.
 *
 * @param client   Client handle
 * @param conn_id  Connection ID
 * @param table    Table name
 * @param schema   Output: table schema (caller must free with lace_schema_free)
 * @return         LACE_OK on success, error code on failure
 */
int lace_get_schema(lace_client_t *client, int conn_id, const char *table, LaceSchema **schema);

/* ==========================================================================
 * Data Queries
 * ========================================================================== */

/*
 * Query table data with optional filters, sorting, and pagination.
 *
 * @param client       Client handle
 * @param conn_id      Connection ID
 * @param table        Table name
 * @param filters      Array of filters (NULL for none)
 * @param num_filters  Number of filters
 * @param sorts        Array of sort specifications (NULL for none)
 * @param num_sorts    Number of sort specifications
 * @param offset       Row offset for pagination
 * @param limit        Maximum rows to return (0 for default)
 * @param result       Output: result set (caller must free with lace_result_free)
 * @return             LACE_OK on success, error code on failure
 */
int lace_query(lace_client_t *client, int conn_id, const char *table,
               const LaceFilter *filters, size_t num_filters,
               const LaceSort *sorts, size_t num_sorts,
               size_t offset, size_t limit,
               LaceResult **result);

/*
 * Count rows in a table with optional filters.
 *
 * @param client       Client handle
 * @param conn_id      Connection ID
 * @param table        Table name
 * @param filters      Array of filters (NULL for none)
 * @param num_filters  Number of filters
 * @param count        Output: row count
 * @param approximate  Output: true if count is approximate
 * @return             LACE_OK on success, error code on failure
 */
int lace_count(lace_client_t *client, int conn_id, const char *table,
               const LaceFilter *filters, size_t num_filters,
               size_t *count, bool *approximate);

/*
 * Execute raw SQL query.
 *
 * @param client   Client handle
 * @param conn_id  Connection ID
 * @param sql      SQL statement to execute
 * @param result   Output: result set for SELECT, or affected count for others
 *                 (caller must free with lace_result_free)
 * @return         LACE_OK on success, error code on failure
 */
int lace_exec(lace_client_t *client, int conn_id, const char *sql, LaceResult **result);

/*
 * Cancel a running query on a connection.
 *
 * This sends a cancellation request to the daemon. For network databases,
 * this uses database-specific cancellation mechanisms (e.g., PQcancel for
 * PostgreSQL, KILL QUERY for MySQL). For SQLite, it uses sqlite3_interrupt.
 *
 * Note: The query must be currently executing for cancellation to work.
 * This function returns immediately; the query may take some time to actually
 * stop. Query cancellation is best-effort and may not work in all situations.
 *
 * @param client   Client handle
 * @param conn_id  Connection ID with the running query
 * @return         LACE_OK on success, error code on failure
 */
int lace_cancel_query(lace_client_t *client, int conn_id);

/* ==========================================================================
 * Data Mutations
 * ========================================================================== */

/*
 * Update a cell value in a table.
 *
 * @param client   Client handle
 * @param conn_id  Connection ID
 * @param table    Table name
 * @param pk       Primary key values identifying the row
 * @param num_pk   Number of primary key columns
 * @param column   Column name to update
 * @param value    New value
 * @return         LACE_OK on success, error code on failure
 */
int lace_update(lace_client_t *client, int conn_id, const char *table,
                const LacePkValue *pk, size_t num_pk,
                const char *column, const LaceValue *value);

/*
 * Delete a row from a table.
 *
 * @param client   Client handle
 * @param conn_id  Connection ID
 * @param table    Table name
 * @param pk       Primary key values identifying the row
 * @param num_pk   Number of primary key columns
 * @return         LACE_OK on success, error code on failure
 */
int lace_delete(lace_client_t *client, int conn_id, const char *table,
                const LacePkValue *pk, size_t num_pk);

/*
 * Insert a row into a table.
 *
 * @param client       Client handle
 * @param conn_id      Connection ID
 * @param table        Table name
 * @param columns      Array of column names
 * @param values       Array of values (parallel to columns)
 * @param num_columns  Number of columns
 * @param out_pk       Output: primary key of inserted row (caller must free, may be NULL)
 * @param out_num_pk   Output: number of primary key columns
 * @return             LACE_OK on success, error code on failure
 */
int lace_insert(lace_client_t *client, int conn_id, const char *table,
                const char **columns, const LaceValue *values, size_t num_columns,
                LacePkValue **out_pk, size_t *out_num_pk);

/* Free primary key array */
void lace_pk_free(LacePkValue *pk, size_t num_pk);

/* ==========================================================================
 * Utility Functions
 * ========================================================================== */

/*
 * Ping the daemon to check if it's alive.
 *
 * @param client  Client handle
 * @return        LACE_OK if daemon responds, error code otherwise
 */
int lace_ping(lace_client_t *client);

/*
 * Get daemon version information.
 *
 * @param client   Client handle
 * @param version  Output: version string (caller must free)
 * @return         LACE_OK on success, error code on failure
 */
int lace_version(lace_client_t *client, char **version);

/*
 * Request graceful daemon shutdown.
 *
 * @param client  Client handle
 * @return        LACE_OK on success, error code on failure
 */
int lace_shutdown(lace_client_t *client);

/* ==========================================================================
 * Configuration
 * ========================================================================== */

/*
 * Set request timeout in milliseconds.
 *
 * @param client      Client handle
 * @param timeout_ms  Timeout in milliseconds (0 for infinite)
 */
void lace_set_timeout(lace_client_t *client, int timeout_ms);

/*
 * Get current request timeout.
 *
 * @param client  Client handle
 * @return        Timeout in milliseconds
 */
int lace_get_timeout(const lace_client_t *client);

/* ==========================================================================
 * Daemon Discovery
 * ========================================================================== */

/*
 * Get the default Unix socket path for the daemon.
 * Checks: $XDG_RUNTIME_DIR/laced.sock, /tmp/laced-{uid}.sock
 *
 * @return  Allocated string with socket path (caller must free), or NULL
 */
char *lace_get_default_socket_path(void);

/*
 * Check if a daemon is running at the given socket path.
 *
 * @param socket_path  Path to check (NULL = auto-detect)
 * @return             true if daemon is responding, false otherwise
 */
bool lace_daemon_is_running(const char *socket_path);

#ifdef __cplusplus
}
#endif

#endif /* LIBLACE_H */
