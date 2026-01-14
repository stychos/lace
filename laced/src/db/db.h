/*
 * Lace
 * Database driver interface
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_DB_H
#define LACE_DB_H

#include "../util/str.h"
#include "db_types.h"
#include <string.h>

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

  /* Query limits */
  size_t max_result_rows; /* Maximum rows to return from queries (0 = use
                             default) */

  /* History recording callback (called after each successful query/exec)
   * type values: 0=auto-detect, or HistoryEntryType from history.h */
  void (*history_callback)(void *context, const char *sql, int type);
  void *history_context;
};

/* History type hint for callback (matches HistoryEntryType) */
#define DB_HISTORY_AUTO 0   /* Auto-detect from SQL */
#define DB_HISTORY_QUERY 0  /* Manual query */
#define DB_HISTORY_SELECT 1 /* Table open/refresh */
#define DB_HISTORY_UPDATE 2 /* Cell edit */
#define DB_HISTORY_DELETE 3 /* Row delete */
#define DB_HISTORY_INSERT 4 /* Row insert */
#define DB_HISTORY_DDL 5    /* CREATE/ALTER/DROP */

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

/* Identifier escaping - uses driver-appropriate quoting (backticks for MySQL,
 * double quotes for PostgreSQL/SQLite). Returns newly allocated string. */
char *db_escape_identifier(DbConnection *conn, const char *name);

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

/* ============================================================================
 * Driver validation macros
 * ============================================================================
 * These reduce boilerplate for common parameter/connection checks in drivers.
 */

/* Validate parameters, set error and return on failure */
#define DB_REQUIRE(cond, err_ptr, ret_val)                                     \
  do {                                                                         \
    if (!(cond)) {                                                             \
      err_set(err_ptr, "Invalid parameters");                                  \
      return ret_val;                                                          \
    }                                                                          \
  } while (0)

/* Validate connection and extract driver data into a variable.
 * data_type: the driver-specific struct type (e.g., SqliteData)
 * data_var: variable name to declare and assign
 * native_field: field in driver data that must be non-NULL (e.g., db, mysql)
 */
#define DB_REQUIRE_CONN(conn, data_type, data_var, native_field, err_ptr,      \
                        ret_val)                                               \
  data_type *data_var = (conn) ? (data_type *)(conn)->driver_data : NULL;      \
  do {                                                                         \
    if (!data_var || !data_var->native_field) {                                \
      err_set(err_ptr, "Not connected");                                       \
      return ret_val;                                                          \
    }                                                                          \
  } while (0)

/* Combined: validate params then extract connection data */
#define DB_REQUIRE_PARAMS_CONN(cond, conn, data_type, data_var, native_field,  \
                               err_ptr, ret_val)                               \
  DB_REQUIRE(cond, err_ptr, ret_val);                                          \
  DB_REQUIRE_CONN(conn, data_type, data_var, native_field, err_ptr, ret_val)

/* ============================================================================
 * Query building helpers
 * ============================================================================
 */

/* Check if an ORDER BY string is a pre-built clause (contains ASC/DESC/comma)
 * or a simple column name that needs escaping. Used by drivers. */
static inline bool db_order_is_prebuilt(const char *order_by) {
  if (!order_by)
    return false;
  return strstr(order_by, " ASC") || strstr(order_by, " DESC") ||
         strstr(order_by, " asc") || strstr(order_by, " desc") ||
         strchr(order_by, ',');
}

#endif /* LACE_DB_H */
