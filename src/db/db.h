/*
 * Lace
 * Database driver interface
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_DB_H
#define LACE_DB_H

#include "db_types.h"

/* Forward declaration */
typedef struct DbConnection DbConnection;

/* Database driver interface (vtable) */
typedef struct DbDriver {
  const char *name;         /* "sqlite", "postgres", "mysql" */
  const char *display_name; /* "SQLite", "PostgreSQL", "MySQL" */

  /* Connection lifecycle */
  DbConnection *(*connect)(const char *connstr, char **err);
  void (*disconnect)(DbConnection *conn);
  bool (*ping)(DbConnection *conn);
  ConnStatus (*status)(DbConnection *conn);
  const char *(*get_error)(DbConnection *conn);

  /* Schema discovery */
  char **(*list_databases)(DbConnection *conn, size_t *count, char **err);
  char **(*list_tables)(DbConnection *conn, size_t *count, char **err);
  TableSchema *(*get_table_schema)(DbConnection *conn, const char *table,
                                   char **err);

  /* Query execution */
  ResultSet *(*query)(DbConnection *conn, const char *sql, char **err);
  int64_t (*exec)(DbConnection *conn, const char *sql, char **err);

  /* Paginated queries */
  ResultSet *(*query_page)(DbConnection *conn, const char *table, size_t offset,
                           size_t limit, const char *order_by, bool desc,
                           char **err);

  /* Data manipulation */
  bool (*update_cell)(DbConnection *conn, const char *table,
                      const char **pk_cols, const DbValue *pk_vals,
                      size_t num_pk_cols, const char *col,
                      const DbValue *new_val, char **err);
  bool (*insert_row)(DbConnection *conn, const char *table,
                     const ColumnDef *cols, const DbValue *vals,
                     size_t num_cols, char **err);
  bool (*delete_row)(DbConnection *conn, const char *table,
                     const char **pk_cols, const DbValue *pk_vals,
                     size_t num_pk_cols, char **err);

  /* Transaction support */
  bool (*begin_transaction)(DbConnection *conn, char **err);
  bool (*commit)(DbConnection *conn, char **err);
  bool (*rollback)(DbConnection *conn, char **err);

  /* Memory management */
  void (*free_result)(ResultSet *rs);
  void (*free_schema)(TableSchema *schema);
  void (*free_string_list)(char **list, size_t count);

  /* Query cancellation support */
  void *(*prepare_cancel)(DbConnection *conn);
  bool (*cancel_query)(DbConnection *conn, void *cancel_handle, char **err);
  void (*free_cancel_handle)(void *cancel_handle);

  /* Approximate row count (fast estimate from system tables) */
  int64_t (*estimate_row_count)(DbConnection *conn, const char *table,
                                char **err);

  /* Library cleanup (called once at program exit) */
  void (*library_cleanup)(void);

} DbDriver;

/* Connection structure (base) */
struct DbConnection {
  const DbDriver *driver;
  char *connstr;
  char *database;
  char *host;
  int port;
  char *user;
  ConnStatus status;
  char *last_error;
  void *driver_data; /* Driver-specific data */

  /* Transaction state tracking */
  bool in_transaction;   /* True if transaction is active */
  int transaction_depth; /* Nesting depth (for savepoints, future use) */
};

/* Driver registration */
void db_register_driver(DbDriver *driver);
DbDriver *db_get_driver(const char *name);
DbDriver **db_get_all_drivers(size_t *count);

/* High-level connection API */
DbConnection *db_connect(const char *connstr, char **err);
void db_disconnect(DbConnection *conn);
bool db_ping(DbConnection *conn);
ConnStatus db_status(DbConnection *conn);
const char *db_get_error(DbConnection *conn);
void db_set_error(DbConnection *conn, const char *fmt, ...);
void db_clear_error(DbConnection *conn);

/* Schema operations */
char **db_list_databases(DbConnection *conn, size_t *count, char **err);
char **db_list_tables(DbConnection *conn, size_t *count, char **err);
TableSchema *db_get_table_schema(DbConnection *conn, const char *table,
                                 char **err);

/* Query operations */
ResultSet *db_query(DbConnection *conn, const char *sql, char **err);
int64_t db_exec(DbConnection *conn, const char *sql, char **err);
ResultSet *db_query_page(DbConnection *conn, const char *table, size_t offset,
                         size_t limit, const char *order_by, bool desc,
                         char **err);
int64_t db_count_rows(DbConnection *conn, const char *table, char **err);

/* Fast row count (uses approximate estimate if available) */
int64_t db_count_rows_fast(DbConnection *conn, const char *table,
                           bool allow_approximate, bool *is_approximate,
                           char **err);

/* Filtered queries (with WHERE clause) */
ResultSet *db_query_page_where(DbConnection *conn, const char *table,
                               size_t offset, size_t limit,
                               const char *where_clause, const char *order_by,
                               bool desc, char **err);
int64_t db_count_rows_where(DbConnection *conn, const char *table,
                            const char *where_clause, char **err);

/* Data manipulation */
bool db_update_cell(DbConnection *conn, const char *table, const char **pk_cols,
                    const DbValue *pk_vals, size_t num_pk_cols, const char *col,
                    const DbValue *new_val, char **err);
bool db_insert_row(DbConnection *conn, const char *table, const ColumnDef *cols,
                   const DbValue *vals, size_t num_cols, char **err);
bool db_delete_row(DbConnection *conn, const char *table, const char **pk_cols,
                   const DbValue *pk_vals, size_t num_pk_cols, char **err);

/* Transaction support */
bool db_begin_transaction(DbConnection *conn, char **err);
bool db_commit(DbConnection *conn, char **err);
bool db_rollback(DbConnection *conn, char **err);
bool db_in_transaction(DbConnection *conn);

/* Transaction context - auto-rollback on scope exit or error */
typedef struct {
  DbConnection *conn;
  bool committed;
  bool owns_transaction; /* True if this context started the transaction */
} DbTransaction;

/* Start a transaction context (auto-rollback if not committed) */
DbTransaction db_transaction_begin(DbConnection *conn, char **err);

/* Commit the transaction context */
bool db_transaction_commit(DbTransaction *txn, char **err);

/* Rollback the transaction context (also called automatically on scope exit) */
bool db_transaction_rollback(DbTransaction *txn, char **err);

/* End transaction context - rolls back if not committed */
void db_transaction_end(DbTransaction *txn);

/* Initialize database subsystem */
void db_init(void);
void db_cleanup(void);

#endif /* LACE_DB_H */
