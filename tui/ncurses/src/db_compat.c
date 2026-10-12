/*
 * Lace TUI
 * Database compatibility layer implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "db_compat.h"
#include "async/async.h"
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
void db_conn_preempt_background(DbConnection *conn, const void *except) {
  if (!conn || !conn->background_op || conn->background_op == except ||
      async_in_worker()) {
    return;
  }
  AsyncOperation *bg = (AsyncOperation *)conn->background_op;
  conn->background_op = NULL; /* Its owner frees it when it polls */
  async_cancel_and_wait(bg);
}

static bool conn_refresh(DbConnection *conn) {
  db_conn_preempt_background(conn, NULL);
  bool changed = false;
  pthread_mutex_lock(&g_refresh_lock);
  uint64_t gen = lace_client_generation(conn->client);
  if (gen != conn->generation && conn->in_transaction) {
    /* The daemon (and with it this connection) was replaced while a
     * transaction was open: its work is gone. Carrying on would run the
     * rest of it in autocommit on a new connection, and COMMIT would
     * "succeed" with nothing in it. */
    conn->transaction_lost = true;
    conn->in_transaction = false;
    conn->transaction_failed = false;
  }
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

/* True for exactly ROLLBACK / ABORT (see lace_sql_is_rollback) */
static bool sql_is_rollback(const char *sql) { return lace_sql_is_rollback(sql); }

#define TX_LOST_MSG                                                            \
  "The connection was reset while a transaction was open: everything since "  \
  "BEGIN was rolled back. Run ROLLBACK to acknowledge, then redo the work."

/* Refuse work on a connection whose transaction was lost until the user
 * acknowledges it with ROLLBACK (which is then answered here: there is
 * nothing left to roll back). Returns false to stop the call; *acked is
 * set when sql was that ROLLBACK. */
static bool conn_guard(DbConnection *conn, const char *sql, char **err,
                       bool *acked) {
  if (acked)
    *acked = false;
  if (!conn->transaction_lost)
    return true;
  if (sql && sql_is_rollback(sql)) {
    conn->transaction_lost = false;
    if (acked)
      *acked = true;
    return false;
  }
  if (err)
    *err = str_dup(TX_LOST_MSG);
  return false;
}

/* Record the transaction state the last reply reported (result: its
 * fallback when the reply did not say) */
static void conn_note_tx(DbConnection *conn, const LaceResult *result) {
  int tx = lace_last_transaction_state();
  if (tx < 0 && result)
    tx = result->in_transaction ? 1 : 0;
  if (tx < 0)
    return;
  conn->in_transaction = tx != 0;
  conn->transaction_failed = tx == 2;
}

/* Error text for a failed call. A call that failed because the daemon was
 * replaced in the middle of a transaction reports the lost transaction
 * (and marks it, see conn_refresh) rather than a generic reconnect. */
static char *conn_error(DbConnection *conn) {
  if (conn->in_transaction &&
      lace_client_generation(conn->client) != conn->generation) {
    conn_refresh(conn);
    if (conn->transaction_lost)
      return str_dup(TX_LOST_MSG);
  }
  /* A failed statement can still open or end a transaction (a failed
   * COMMIT rolls back; a failure inside BEGIN leaves it open) */
  conn_note_tx(conn, NULL);
  return str_dup(lace_client_error(conn->client));
}

/* After a failed read-only call: retry once if the link dropped or the
 * daemon was replaced. Writes are never retried (they may have applied). */
static bool conn_should_retry(DbConnection *conn, int rc) {
  if (conn->in_transaction) {
    return false; /* A retry would run outside the open transaction */
  }
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
/* Reads the TUI makes on its own (pages, counts, schema, row checks) run
 * under a savepoint inside a PostgreSQL transaction: a failure, or Esc
 * cancelling a slow page, would otherwise abort the user's transaction.
 * End it after the error text was taken (it is per-thread). */
static bool read_savepoint_begin(DbConnection *conn) {
  if (conn->driver_type != LACE_DRIVER_POSTGRES || !conn->in_transaction ||
      conn->transaction_failed) {
    return false;
  }
  return lace_exec(conn->client, conn->conn_id, "SAVEPOINT lace_read",
                   NULL) == LACE_OK;
}

static void read_savepoint_end(DbConnection *conn, bool active, bool ok) {
  if (active &&
      lace_exec(conn->client, conn->conn_id,
                ok ? "RELEASE SAVEPOINT lace_read"
                   : "ROLLBACK TO SAVEPOINT lace_read",
                NULL) == LACE_OK) {
    /* The failed read marked the transaction failed; it is live again */
    conn_note_tx(conn, NULL);
  }
}

char **db_list_tables(DbConnection *conn, size_t *count, char **err) {
  if (!conn || !count) {
    if (err) *err = str_dup("Invalid arguments");
    return NULL;
  }

  char **tables = NULL;
  conn_refresh(conn);
  if (!conn_guard(conn, NULL, err, NULL)) {
    return NULL;
  }
  bool sp = read_savepoint_begin(conn);
  int rc = lace_list_tables(conn->client, conn->conn_id, &tables, count);
  if (rc != LACE_OK && conn_should_retry(conn, rc)) {
    rc = lace_list_tables(conn->client, conn->conn_id, &tables, count);
  }
  if (rc != LACE_OK) {
    if (err) *err = conn_error(conn);
    read_savepoint_end(conn, sp, false);
    return NULL;
  }
  read_savepoint_end(conn, sp, true);
  return tables;
}

/* Get table schema */
TableSchema *db_get_table_schema(DbConnection *conn, const char *table, char **err) {
  if (!conn || !table) {
    if (err) *err = str_dup("Invalid arguments");
    return NULL;
  }

  LaceSchema *schema = NULL;
  conn_refresh(conn);
  if (!conn_guard(conn, NULL, err, NULL)) {
    return NULL;
  }
  bool sp = read_savepoint_begin(conn);
  int rc = lace_get_schema(conn->client, conn->conn_id, table, &schema);
  if (rc != LACE_OK && conn_should_retry(conn, rc)) {
    rc = lace_get_schema(conn->client, conn->conn_id, table, &schema);
  }
  if (rc != LACE_OK) {
    if (err) *err = conn_error(conn);
    read_savepoint_end(conn, sp, false);
    return NULL;
  }
  read_savepoint_end(conn, sp, true);
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
  bool acked = false;
  if (!conn_guard(conn, sql, err, &acked)) {
    return acked ? (ResultSet *)safe_calloc(1, sizeof(ResultSet)) : NULL;
  }
  int rc = lace_exec(conn->client, conn->conn_id, sql, &result);
  if (rc != LACE_OK && sql_is_read_only(sql) && conn_should_retry(conn, rc)) {
    rc = lace_exec(conn->client, conn->conn_id, sql, &result);
  }
  if (rc != LACE_OK) {
    if (err) *err = conn_error(conn);
    return NULL;
  }
  if (result) {
    conn_note_tx(conn, result);
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
  bool acked = false;
  if (!conn_guard(conn, sql, err, &acked)) {
    return acked ? 0 : -1;
  }
  int rc = lace_exec(conn->client, conn->conn_id, sql, &result);
  if (rc != LACE_OK) {
    if (err) *err = conn_error(conn);
    return -1;
  }
  if (result) {
    conn_note_tx(conn, result);
  }

  int64_t affected = result ? result->rows_affected : 0;
  lace_result_free(result);
  return affected;
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
  conn_refresh(conn);
  bool sp = !conn->transaction_lost && read_savepoint_begin(conn);
  ResultSet *rs = db_query(conn, sql, err);
  read_savepoint_end(conn, sp, rs != NULL);
  free(sql);
  return rs;
}

/* Count rows in table (with optional filters) */
bool db_count_rows(DbConnection *conn, const char *table, size_t *count,
                   bool *approximate, char **err) {
  if (!conn || !table || !count) {
    if (err) *err = str_dup("Invalid arguments");
    return false;
  }

  conn_refresh(conn);
  if (!conn_guard(conn, NULL, err, NULL)) {
    return false;
  }
  bool sp = read_savepoint_begin(conn);
  int rc = lace_count(conn->client, conn->conn_id, table, count, approximate);
  if (rc != LACE_OK && conn_should_retry(conn, rc)) {
    rc = lace_count(conn->client, conn->conn_id, table, count, approximate);
  }
  if (rc != LACE_OK) {
    if (err) *err = conn_error(conn);
    read_savepoint_end(conn, sp, false);
    return false;
  }
  read_savepoint_end(conn, sp, true);
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
  if (!conn_guard(conn, NULL, err, NULL)) {
    free(sql);
    return false;
  }
  bool sp = read_savepoint_begin(conn);
  int rc = lace_exec(conn->client, conn->conn_id, sql, &result);
  if (rc != LACE_OK && sql_is_read_only(sql) && conn_should_retry(conn, rc)) {
    rc = lace_exec(conn->client, conn->conn_id, sql, &result);
  }
  free(sql);
  if (rc != LACE_OK) {
    if (err) *err = conn_error(conn);
    read_savepoint_end(conn, sp, false);
    return false;
  }
  read_savepoint_end(conn, sp, true);

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

  conn_refresh(conn);
  if (!conn_guard(conn, NULL, err, NULL)) {
    return false;
  }

  LacePkValue *pk = build_pk_array(pk_cols, pk_vals, num_pk);
  if (!pk) {
    if (err) *err = str_dup("Failed to build PK array");
    return false;
  }

  int rc = lace_update(conn->client, conn->conn_id, table,
                       pk, num_pk, column, value);
  free_pk_array(pk, num_pk);

  if (rc != LACE_OK) {
    if (err) *err = conn_error(conn);
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

  conn_refresh(conn);
  if (!conn_guard(conn, NULL, err, NULL)) {
    return false;
  }

  LacePkValue *pk = build_pk_array(pk_cols, pk_vals, num_pk);
  if (!pk) {
    if (err) *err = str_dup("Failed to build PK array");
    return false;
  }

  int rc = lace_delete(conn->client, conn->conn_id, table, pk, num_pk);
  free_pk_array(pk, num_pk);

  if (rc != LACE_OK) {
    if (err) *err = conn_error(conn);
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

  conn_refresh(conn);
  if (!conn_guard(conn, NULL, err, NULL)) {
    return false;
  }

  /* Extract column names from LaceColumn array (0 columns: all defaults) */
  const char **col_names = safe_malloc((num_columns ? num_columns : 1) * sizeof(char *));
  for (size_t i = 0; i < num_columns; i++) {
    col_names[i] = columns[i].name;
  }

  int rc = lace_insert(conn->client, conn->conn_id, table,
                       col_names, values, num_columns);
  free(col_names);

  if (rc != LACE_OK) {
    if (err) *err = conn_error(conn);
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
  if (!conn_guard(conn, NULL, err, NULL)) {
    free_pk_array(pk, num_pk);
    return NULL;
  }
  bool sp = read_savepoint_begin(conn);
  int rc = lace_select_by_pk(conn->client, conn->conn_id, table, pk, num_pk,
                             columns, num_columns, &result);
  free_pk_array(pk, num_pk);
  if (rc != LACE_OK) {
    if (err) *err = conn_error(conn);
    read_savepoint_end(conn, sp, false);
    return NULL;
  }
  read_savepoint_end(conn, sp, true);
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
    if (err) *err = conn_error(conn);
    return false;
  }

  return true;
}

bool db_verify_row(DbConnection *conn, const char *table, const char **pk_cols,
                   const DbValue *pk_vals, size_t num_pk, const char **cols,
                   const DbValue *expected, size_t n, char **why) {
  *why = NULL;
  char *err = NULL;
  ResultSet *db = db_select_by_pk(conn, table, pk_cols, pk_vals, num_pk, cols,
                                  n, &err);
  if (!db) {
    *why = err ? err : str_dup("cannot re-read the row");
    return false;
  }
  free(err);
  bool ok = false;
  if (db->num_rows != 1 || !db->rows) {
    *why = str_dup(db->num_rows == 0
                       ? "the row no longer exists (or its key changed)"
                       : "its key matches more than one row");
  } else {
    ok = true;
    for (size_t i = 0; i < n && ok; i++) {
      if (i >= db->rows[0].num_cells) {
        ok = false;
        *why = str_dup("cannot re-read the row");
        break;
      }
      const DbValue *a = &expected[i];
      const DbValue *b = &db->rows[0].cells[i];
      char *sa = a->is_null ? NULL : lace_value_to_edit_text(a);
      char *sb = b->is_null ? NULL : lace_value_to_edit_text(b);
      bool same = a->is_null == b->is_null &&
                  (a->is_null || (sa && sb && strcmp(sa, sb) == 0));
      free(sa);
      free(sb);
      if (!same) {
        ok = false;
        *why = str_printf("column '%s' does not match the database (changed "
                          "since it was loaded, or a computed/aliased column; "
                          "refresh first)",
                          cols[i]);
      }
    }
  }
  db_result_free(db);
  return ok;
}

size_t db_schema_column_index(const TableSchema *schema, const char *name) {
  if (!schema || !name)
    return SIZE_MAX;
  for (size_t k = 0; k < schema->num_columns; k++) {
    if (schema->columns[k].name && strcmp(schema->columns[k].name, name) == 0)
      return k;
  }
  return SIZE_MAX;
}
