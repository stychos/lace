/*
 * Lace TUI
 * Database compatibility layer implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "db_compat.h"
#include "../../liblace/include/config/history.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ==========================================================================
 * Daemon recovery
 * If the daemon died, liblace spawns a new one and bumps the client
 * generation; our conn_id then refers to nothing. Re-open the database from
 * the stored connection string so the session continues transparently.
 * ========================================================================== */

/* Serializes conn_refresh: the main thread and prefetch workers can both
 * notice a new generation at once; only one may re-open the database. */
static pthread_mutex_t g_refresh_lock = PTHREAD_MUTEX_INITIALIZER;

/* Reconnect if the daemon was replaced, then declare this thread's
 * generation so liblace refuses to send a stale conn_id to a different
 * daemon. Returns true if conn_id changed. */
static bool conn_refresh(DbConnection *conn) {
  bool changed = false;
  pthread_mutex_lock(&g_refresh_lock);
  uint64_t gen = lace_client_generation(conn->client);
  if (gen != conn->generation && conn->connstr) {
    int new_id = -1;
    if (lace_connect(conn->client, conn->connstr, NULL, &new_id) == LACE_OK) {
      conn->conn_id = new_id;
      /* lace_connect may itself have hit another restart */
      conn->generation = lace_client_generation(conn->client);
      changed = true;
    }
  }
  uint64_t mine = conn->generation;
  pthread_mutex_unlock(&g_refresh_lock);
  lace_expect_generation(conn->client, mine);
  return changed;
}

/* After a failed read-only call: retry once if the link dropped or the
 * daemon was replaced. Writes are never retried (they may have applied). */
static bool conn_should_retry(DbConnection *conn, int rc) {
  if (rc == LACE_ERR_CONNECTION_LOST || rc == LACE_ERR_PIPE_ERROR) {
    conn_refresh(conn);
    return true;
  }
  return conn_refresh(conn);
}

static bool sql_is_read_only(const char *sql) {
  while (*sql == ' ' || *sql == '\t' || *sql == '\n' || *sql == '\r') sql++;
  return strncasecmp(sql, "SELECT", 6) == 0 || strncasecmp(sql, "PRAGMA", 6) == 0 ||
         strncasecmp(sql, "SHOW", 4) == 0 || strncasecmp(sql, "EXPLAIN", 7) == 0 ||
         strncasecmp(sql, "DESCRIBE", 8) == 0;
}

/* Quote a table reference for this connection's dialect, e.g.
 * "schema"."table" on PostgreSQL (caller frees). */
static char *quote_table(const DbConnection *conn, const char *table) {
  return lace_quote_table(conn->driver_type, table);
}

/* Build "SELECT * FROM t [WHERE w] [ORDER BY o] LIMIT n OFFSET m" without
 * a fixed-size buffer (long raw filters used to be silently truncated). */
static char *build_page_sql(const DbConnection *conn, const char *table,
                            const char *where_clause, const char *order_clause,
                            size_t limit, size_t offset) {
  char *qt = quote_table(conn, table);
  if (!qt) {
    return NULL;
  }
  bool has_where = where_clause && where_clause[0];
  bool has_order = order_clause && order_clause[0];
  char *sql = str_printf("SELECT * FROM %s%s%s%s%s LIMIT %zu OFFSET %zu", qt,
                         has_where ? " WHERE " : "", has_where ? where_clause : "",
                         has_order ? " ORDER BY " : "", has_order ? order_clause : "",
                         limit, offset);
  free(qt);
  return sql;
}

/* Static driver stubs for each supported driver */
static const DbDriver g_driver_sqlite = {"sqlite", "SQLite"};
static const DbDriver g_driver_postgres = {"postgres", "PostgreSQL"};
static const DbDriver g_driver_mysql = {"mysql", "MySQL"};
static const DbDriver g_driver_mariadb = {"mariadb", "MariaDB"};
static const DbDriver g_driver_unknown = {"unknown", "Unknown"};

/* Get driver stub from driver type */
static const DbDriver *driver_from_type(LaceDriver type) {
  switch (type) {
    case LACE_DRIVER_SQLITE:   return &g_driver_sqlite;
    case LACE_DRIVER_POSTGRES: return &g_driver_postgres;
    case LACE_DRIVER_MYSQL:    return &g_driver_mysql;
    case LACE_DRIVER_MARIADB:  return &g_driver_mariadb;
    default:                   return &g_driver_unknown;
  }
}

/* Create a DbConnection wrapper from liblace connection info */
DbConnection *db_conn_wrap(lace_client_t *client, int conn_id, const char *connstr) {
  if (!client || conn_id < 0) return NULL;

  DbConnection *conn = safe_calloc(1, sizeof(DbConnection));

  conn->conn_id = conn_id;
  conn->client = client;
  conn->connstr = connstr ? str_dup(connstr) : NULL;
  conn->generation = lace_client_generation(client);
  conn->driver = &g_driver_unknown;

  /* Get connection info from liblace */
  LaceConnInfo *info = NULL;
  size_t count = 0;
  if (lace_list_connections(client, &info, &count) == LACE_OK && info) {
    for (size_t i = 0; i < count; i++) {
      if (info[i].id == conn_id) {
        conn->driver_type = info[i].driver;
        conn->driver = driver_from_type(info[i].driver);
        conn->database = info[i].database ? str_dup(info[i].database) : NULL;
        conn->host = info[i].host ? str_dup(info[i].host) : NULL;
        conn->port = info[i].port;
        conn->user = info[i].user ? str_dup(info[i].user) : NULL;
        break;
      }
    }
    lace_conn_info_array_free(info, count);
  }

  return conn;
}

/* Free a DbConnection wrapper (does NOT close the connection) */
void db_conn_free(DbConnection *conn) {
  if (!conn) return;
  str_secure_free(conn->connstr); /* May embed a password */
  free(conn->database);
  free(conn->host);
  free(conn->user);
  free(conn->last_error);
  free(conn);
}

/* Get driver name from connection */
const char *db_conn_driver_name(DbConnection *conn) {
  if (!conn || !conn->driver) return "unknown";
  return conn->driver->name;
}

/* Get driver stub from connection */
const DbDriver *db_get_driver(DbConnection *conn) {
  if (!conn) return &g_driver_unknown;
  return conn->driver;
}

/* ==========================================================================
 * Database operation wrappers
 * ========================================================================== */

/* Connect to database - returns DbConnection wrapper or NULL on error */
DbConnection *db_connect(lace_client_t *client, const char *connstr, char **err) {
  if (!client || !connstr) {
    if (err) *err = str_dup("Invalid arguments");
    return NULL;
  }

  int conn_id = -1;
  int rc = lace_connect(client, connstr, NULL, &conn_id);
  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(client));
    return NULL;
  }

  return db_conn_wrap(client, conn_id, connstr);
}

/* Disconnect and free connection wrapper */
void db_disconnect(DbConnection *conn) {
  if (!conn) return;
  if (conn->client && conn->conn_id >= 0) {
    lace_disconnect(conn->client, conn->conn_id);
  }
  db_conn_free(conn);
}

/* List tables in connection */
char **db_list_tables(DbConnection *conn, size_t *count, char **err) {
  if (!conn || !count) {
    if (err) *err = str_dup("Invalid arguments");
    return NULL;
  }

  char **tables = NULL;
  conn_refresh(conn);
  int rc = lace_list_tables(conn->client, conn->conn_id, &tables, count);
  if (rc != LACE_OK && conn_should_retry(conn, rc)) {
    rc = lace_list_tables(conn->client, conn->conn_id, &tables, count);
  }
  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return NULL;
  }

  return tables;
}

/* Free table list */
void db_free_tables(char **tables, size_t count) {
  lace_tables_free(tables, count);
}

/* Get table schema */
TableSchema *db_get_table_schema(DbConnection *conn, const char *table, char **err) {
  if (!conn || !table) {
    if (err) *err = str_dup("Invalid arguments");
    return NULL;
  }

  LaceSchema *schema = NULL;
  conn_refresh(conn);
  int rc = lace_get_schema(conn->client, conn->conn_id, table, &schema);
  if (rc != LACE_OK && conn_should_retry(conn, rc)) {
    rc = lace_get_schema(conn->client, conn->conn_id, table, &schema);
  }
  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return NULL;
  }

  return schema;
}

/* Execute query */
ResultSet *db_query(DbConnection *conn, const char *sql, char **err) {
  if (!conn || !sql) {
    if (err) *err = str_dup("Invalid arguments");
    return NULL;
  }

  LaceResult *result = NULL;
  conn_refresh(conn);
  int rc = lace_exec(conn->client, conn->conn_id, sql, &result);
  if (rc != LACE_OK && sql_is_read_only(sql) && conn_should_retry(conn, rc)) {
    rc = lace_exec(conn->client, conn->conn_id, sql, &result);
  }
  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return NULL;
  }

  return result;
}

/* Execute non-SELECT statement */
int64_t db_exec(DbConnection *conn, const char *sql, char **err) {
  if (!conn || !sql) {
    if (err) *err = str_dup("Invalid arguments");
    return -1;
  }

  LaceResult *result = NULL;
  conn_refresh(conn);
  int rc = lace_exec(conn->client, conn->conn_id, sql, &result);
  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return -1;
  }

  int64_t affected = result ? result->rows_affected : 0;
  lace_result_free(result);
  return affected;
}

/* Query table with pagination and optional filters/sorts */
ResultSet *db_query_table(DbConnection *conn, const char *table,
                          const LaceFilter *filters, size_t num_filters,
                          const LaceSort *sorts, size_t num_sorts,
                          size_t offset, size_t limit, char **err) {
  if (!conn || !table) {
    if (err) *err = str_dup("Invalid arguments");
    return NULL;
  }

  LaceResult *result = NULL;
  conn_refresh(conn);
  int rc = lace_query(conn->client, conn->conn_id, table,
                      filters, num_filters, sorts, num_sorts,
                      offset, limit, &result);
  if (rc != LACE_OK && conn_should_retry(conn, rc)) {
    rc = lace_query(conn->client, conn->conn_id, table,
                        filters, num_filters, sorts, num_sorts,
                        offset, limit, &result);
  }
  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return NULL;
  }

  return result;
}

/* Query page (simple pagination without filters) */
ResultSet *db_query_page(DbConnection *conn, const char *table,
                         size_t offset, size_t limit,
                         const char *order_clause, bool order_desc, char **err) {
  return db_query_page_where(conn, table, offset, limit, NULL, order_clause,
                             order_desc, err);
}

/* Query page with WHERE clause */
ResultSet *db_query_page_where(DbConnection *conn, const char *table,
                               size_t offset, size_t limit,
                               const char *where_clause,
                               const char *order_clause, bool order_desc,
                               char **err) {
  if (!conn || !table) {
    if (err) *err = str_dup("Invalid arguments");
    return NULL;
  }

  (void)order_desc; /* order_clause already includes direction */

  char *sql = build_page_sql(conn, table, where_clause, order_clause, limit, offset);
  if (!sql) {
    if (err) *err = str_dup("Out of memory");
    return NULL;
  }
  ResultSet *rs = db_query(conn, sql, err);
  free(sql);
  return rs;
}

/* Count rows in table (with optional filters) */
bool db_count_rows(DbConnection *conn, const char *table,
                   const LaceFilter *filters, size_t num_filters,
                   size_t *count, bool *approximate, char **err) {
  if (!conn || !table || !count) {
    if (err) *err = str_dup("Invalid arguments");
    return false;
  }

  conn_refresh(conn);
  int rc = lace_count(conn->client, conn->conn_id, table,
                      filters, num_filters, count, approximate);
  if (rc != LACE_OK && conn_should_retry(conn, rc)) {
    rc = lace_count(conn->client, conn->conn_id, table,
                        filters, num_filters, count, approximate);
  }
  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return false;
  }

  return true;
}

/* Count rows with WHERE clause (raw SQL) */
bool db_count_rows_where(DbConnection *conn, const char *table,
                         const char *where_clause, size_t *count, char **err) {
  if (!conn || !table || !count) {
    if (err) *err = str_dup("Invalid arguments");
    return false;
  }

  char *qt = quote_table(conn, table);
  bool has_where = where_clause && where_clause[0];
  char *sql = qt ? str_printf("SELECT COUNT(*) FROM %s%s%s", qt,
                              has_where ? " WHERE " : "",
                              has_where ? where_clause : "")
                 : NULL;
  free(qt);
  if (!sql) {
    if (err) *err = str_dup("Out of memory");
    return false;
  }

  LaceResult *result = NULL;
  conn_refresh(conn);
  int rc = lace_exec(conn->client, conn->conn_id, sql, &result);
  if (rc != LACE_OK && sql_is_read_only(sql) && conn_should_retry(conn, rc)) {
    rc = lace_exec(conn->client, conn->conn_id, sql, &result);
  }
  free(sql);
  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return false;
  }

  *count = 0;
  if (result && result->num_rows > 0 && result->rows &&
      result->rows[0].num_cells > 0 && result->rows[0].cells) {
    LaceValue *val = &result->rows[0].cells[0];
    if (val->type == LACE_TYPE_INT) {
      *count = (size_t)val->int_val;
    }
  }

  lace_result_free(result);
  return true;
}

/* Helper: Build LacePkValue array from separate arrays */
static LacePkValue *build_pk_array(const char **pk_cols, const LaceValue *pk_vals,
                                   size_t num_pk) {
  if (!pk_cols || !pk_vals || num_pk == 0) return NULL;

  LacePkValue *pk = safe_malloc(num_pk * sizeof(LacePkValue));
  if (!pk) return NULL;

  for (size_t i = 0; i < num_pk; i++) {
    pk[i].column = (char *)pk_cols[i];  /* Don't copy, just reference */
    pk[i].value = lace_value_copy(&pk_vals[i]);
  }

  return pk;
}

/* Helper: Free LacePkValue array */
static void free_pk_array(LacePkValue *pk, size_t num_pk) {
  if (!pk) return;
  for (size_t i = 0; i < num_pk; i++) {
    lace_value_free(&pk[i].value);
  }
  free(pk);
}

/* Update a cell value (old-style PK with separate arrays) */
bool db_update_cell(DbConnection *conn, const char *table,
                    const char **pk_cols, const LaceValue *pk_vals, size_t num_pk,
                    const char *column, const LaceValue *value, char **err) {
  if (!conn || !table || !pk_cols || !pk_vals || !column || !value) {
    if (err) *err = str_dup("Invalid arguments");
    return false;
  }

  LacePkValue *pk = build_pk_array(pk_cols, pk_vals, num_pk);
  if (!pk) {
    if (err) *err = str_dup("Failed to build PK array");
    return false;
  }

  conn_refresh(conn);
  int rc = lace_update(conn->client, conn->conn_id, table,
                       pk, num_pk, column, value);
  free_pk_array(pk, num_pk);

  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return false;
  }

  if (conn->on_sql && lace_last_sql()) {
    conn->on_sql(conn->on_sql_ctx, conn, lace_last_sql(), LACE_HISTORY_UPDATE);
  }
  return true;
}

/* Delete a row (old-style PK with separate arrays) */
bool db_delete_row(DbConnection *conn, const char *table,
                   const char **pk_cols, const LaceValue *pk_vals, size_t num_pk,
                   char **err) {
  if (!conn || !table || !pk_cols || !pk_vals) {
    if (err) *err = str_dup("Invalid arguments");
    return false;
  }

  LacePkValue *pk = build_pk_array(pk_cols, pk_vals, num_pk);
  if (!pk) {
    if (err) *err = str_dup("Failed to build PK array");
    return false;
  }

  conn_refresh(conn);
  int rc = lace_delete(conn->client, conn->conn_id, table, pk, num_pk);
  free_pk_array(pk, num_pk);

  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return false;
  }

  if (conn->on_sql && lace_last_sql()) {
    conn->on_sql(conn->on_sql_ctx, conn, lace_last_sql(), LACE_HISTORY_DELETE);
  }
  return true;
}

/* Insert a row - extract column names from LaceColumn array */
bool db_insert_row(DbConnection *conn, const char *table,
                   const LaceColumn *columns, const LaceValue *values,
                   size_t num_columns, char **err) {
  if (!conn || !table || !columns || !values) {
    if (err) *err = str_dup("Invalid arguments");
    return false;
  }

  /* Extract column names from LaceColumn array (0 columns: all defaults) */
  const char **col_names = safe_malloc((num_columns ? num_columns : 1) * sizeof(char *));
  for (size_t i = 0; i < num_columns; i++) {
    col_names[i] = columns[i].name;
  }

  conn_refresh(conn);
  int rc = lace_insert(conn->client, conn->conn_id, table,
                       col_names, values, num_columns, NULL, NULL);
  free(col_names);

  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return false;
  }

  if (conn->on_sql && lace_last_sql()) {
    conn->on_sql(conn->on_sql_ctx, conn, lace_last_sql(), LACE_HISTORY_INSERT);
  }
  return true;
}

/* Fetch `columns` of the rows a PK WHERE clause matches (at most 2) */
ResultSet *db_select_by_pk(DbConnection *conn, const char *table,
                           const char **pk_cols, const LaceValue *pk_vals,
                           size_t num_pk, const char **columns,
                           size_t num_columns, char **err) {
  if (!conn || !table || !pk_cols || !pk_vals || num_pk == 0) {
    if (err) *err = str_dup("Invalid arguments");
    return NULL;
  }
  LacePkValue *pk = build_pk_array(pk_cols, pk_vals, num_pk);
  if (!pk) {
    if (err) *err = str_dup("Out of memory");
    return NULL;
  }
  LaceResult *result = NULL;
  conn_refresh(conn);
  int rc = lace_select_by_pk(conn->client, conn->conn_id, table, pk, num_pk,
                             columns, num_columns, &result);
  free_pk_array(pk, num_pk);
  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return NULL;
  }
  return result;
}

/* Cancel a running query on a connection */
bool db_cancel_query(DbConnection *conn, char **err) {
  if (!conn || !conn->client) {
    if (err) *err = str_dup("Invalid connection");
    return false;
  }

  int rc = lace_cancel_query(conn->client, conn->conn_id);
  if (rc != LACE_OK) {
    if (err) *err = str_dup(lace_client_error(conn->client));
    return false;
  }

  return true;
}
