/*
 * Lace TUI
 * Database compatibility layer - maps old db types to liblace
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_DB_COMPAT_H
#define LACE_DB_COMPAT_H

#include <stdatomic.h>

#include "../../liblace/include/lace.h"

/* ==========================================================================
 * Type aliases - Map old db types to liblace types
 * These are just typedefs, not redefinitions.
 * ========================================================================== */

/* Value types */
typedef LaceValueType DbValueType;
#define DB_TYPE_NULL      LACE_TYPE_NULL
#define DB_TYPE_INT       LACE_TYPE_INT
#define DB_TYPE_FLOAT     LACE_TYPE_FLOAT

/* Value struct */
typedef LaceValue DbValue;

/* Column definition */
typedef LaceColumn ColumnDef;

/* Row struct */
typedef LaceRow Row;

/* Result set */
typedef LaceResult ResultSet;

/* Index definition */
typedef LaceIndex IndexDef;

/* Foreign key definition */
typedef LaceForeignKey ForeignKeyDef;

/* Table schema */
typedef LaceSchema TableSchema;

/* ==========================================================================
 * Connection compatibility
 * The old code used DbConnection pointers. We provide a minimal struct
 * that wraps the liblace connection ID for compatibility.
 * ========================================================================== */

/* Driver vtable stub for compatibility with conn->driver->name access */
typedef struct DbDriver {
  const char *name;
  const char *display_name;
} DbDriver;

/* Minimal DbConnection wrapper for compatibility */
/* conn_id, generation and the transaction flags are written by worker
 * threads (a reconnect, the reply of a statement) and read by the main
 * thread (status bar, guards, cancel): they are atomics. */
typedef struct DbConnection {
  _Atomic int conn_id;      /* liblace connection ID */
  lace_client_t *client;    /* Back-reference to client */
  char *connstr;            /* Connection string (for display) */
  char *database;           /* Database name */
  char *host;               /* Host (NULL for SQLite) */
  int port;                 /* Port */
  char *user;               /* Username */
  LaceDriver driver_type;   /* Driver type enum */
  const DbDriver *driver;   /* Driver stub (for conn->driver->name) */
  char *last_error;         /* Last error message */
  _Atomic uint64_t generation; /* lace_client_generation() when issued */

  /* A best-effort background statement (the query tab's row count) using
   * this connection. The daemon runs one statement per connection, so any
   * foreground call from the main thread cancels it first. */
  void *background_op;

  /* Transaction state as the daemon reported it after the last statement.
   * transaction_lost: the connection was replaced while one was open;
   * further work is refused until the user runs ROLLBACK. */
  _Atomic bool in_transaction;
  _Atomic bool transaction_failed; /* Open but failed: only ROLLBACK works */
  _Atomic bool transaction_lost;

  /* Called on the calling thread after a successful data change made
   * through db_update_cell/db_delete_row/db_insert_row (type is a
   * LaceHistoryType). Set by the app layer to record query history. */
  void (*on_sql)(void *ctx, struct DbConnection *conn, const char *sql, int type);
  void *on_sql_ctx;
} DbConnection;

/* Cancel (and wait for) the connection's background statement unless it is
 * `except`. Call before any foreground statement on the connection, from the
 * main thread (no-op on worker threads). */
void db_conn_preempt_background(DbConnection *conn, const void *except);

/* Before changing a row by its key: the key must match exactly one row, and
 * cols must still hold the expected (loaded) values. On failure *why says
 * what is wrong (caller frees). */
bool db_verify_row(DbConnection *conn, const char *table, const char **pk_cols,
                   const DbValue *pk_vals, size_t num_pk, const char **cols,
                   const DbValue *expected, size_t n, char **why);

/* Create a DbConnection wrapper from liblace connection info */
DbConnection *db_conn_wrap(lace_client_t *client, int conn_id, const char *connstr);

/* Free a DbConnection wrapper (does NOT close the connection) */
void db_conn_free(DbConnection *conn);

/* Get driver name from connection */
const char *db_conn_driver_name(DbConnection *conn);

/* Index of the schema column called name, SIZE_MAX if none. Result columns
 * and schema columns are matched by name, never by position: they differ
 * when the table has hidden or generated columns, or a query picks some. */
size_t db_schema_column_index(const TableSchema *schema, const char *name);

/* ==========================================================================
 * Database operation wrappers
 * These wrap liblace calls for backwards compatibility with old db_* API
 * ========================================================================== */

/* Connect to database - returns DbConnection wrapper or NULL on error */
DbConnection *db_connect(lace_client_t *client, const char *connstr, char **err);

/* Disconnect and free connection wrapper */
void db_disconnect(DbConnection *conn);

/* List tables in connection */
char **db_list_tables(DbConnection *conn, size_t *count, char **err);

/* Get table schema */
TableSchema *db_get_table_schema(DbConnection *conn, const char *table, char **err);

/* The tables referencing table and their ON DELETE rules (lace_table_dependents) */
bool db_table_dependents(DbConnection *conn, const char *table, LaceDependent **deps, size_t *n, char **err);

/* Execute query */
ResultSet *db_query(DbConnection *conn, const char *sql, char **err);

/* Execute non-SELECT statement */
int64_t db_exec(DbConnection *conn, const char *sql, char **err);

/* Query page (simple pagination without filters) */
ResultSet *db_query_page(DbConnection *conn, const char *table,
                         size_t offset, size_t limit,
                         const char *order_clause, bool order_desc, char **err);

/* Query page with WHERE clause (uses raw SQL) */
ResultSet *db_query_page_where(DbConnection *conn, const char *table,
                               size_t offset, size_t limit,
                               const char *where_clause,
                               const char *order_clause, bool order_desc,
                               char **err);

/* Count rows in table (*approximate in: estimate allowed; out: is estimate) */
bool db_count_rows(DbConnection *conn, const char *table, size_t *count,
                   bool *approximate, char **err);

/* Count rows with WHERE clause (raw SQL) */
bool db_count_rows_where(DbConnection *conn, const char *table,
                         const char *where_clause, size_t *count, char **err);

/* Update a cell value (old-style PK with separate arrays) */
bool db_update_cell(DbConnection *conn, const char *table,
                    const char **pk_cols, const LaceValue *pk_vals, size_t num_pk,
                    const char *column, const LaceValue *value, char **err);

/* Delete a row (old-style PK with separate arrays) */
bool db_delete_row(DbConnection *conn, const char *table,
                   const char **pk_cols, const LaceValue *pk_vals, size_t num_pk,
                   char **err);

/* Insert a row */
bool db_insert_row(DbConnection *conn, const char *table,
                   const LaceColumn *columns, const LaceValue *values,
                   size_t num_columns, char **err);

/* Fetch `columns` of the rows a primary-key WHERE clause matches, exactly
 * as db_update_cell/db_delete_row would target them (at most 2 rows). */
ResultSet *db_select_by_pk(DbConnection *conn, const char *table,
                           const char **pk_cols, const LaceValue *pk_vals,
                           size_t num_pk, const char **columns,
                           size_t num_columns, char **err);

/* Cancel a running query on a connection */
bool db_cancel_query(DbConnection *conn, char **err);

/* ==========================================================================
 * Function aliases
 * ========================================================================== */

/* Value creation */
#define db_value_null()           lace_value_null()
#define db_value_text(s)          lace_value_text(s)
#define db_value_copy(v)          lace_value_copy(v)

/* Value conversion */
#define db_value_to_string(v)     lace_value_to_string(v)

/* Memory management */
#define db_value_free(v)          lace_value_free(v)
#define db_result_free(r)         lace_result_free(r)
#define db_schema_free(s)         lace_schema_free(s)

/* Type info */
#define db_value_type_name(t)     lace_type_name(t)

#endif /* LACE_DB_COMPAT_H */
