/*
 * Lace
 * Database driver interface
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_DB_H
#define LACE_DB_H

#include "util/str.h"
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

  /* Run SQL (every statement in it); the reply is the last result with
   * rows, else the last affected count. The daemon's only data path: table
   * listing, schema, paging and edits are SQL that liblace builds. */
  ResultSet *(*query)(DbConnection *conn, const char *sql, char **err);

  /* Optional. Before each statement, and before its cancel handle is taken:
   * make sure there is a live connection to run sql on (reopen a lost one,
   * or refuse while a lost transaction is unacknowledged). *answered: sql
   * was fully handled here (the ROLLBACK acknowledging a lost transaction)
   * and must not be run. Returns false with *err set to fail the statement. */
  bool (*ensure_ready)(DbConnection *conn, const char *sql, bool *answered,
                       char **err);

  /* Query cancellation support */
  void *(*prepare_cancel)(DbConnection *conn);
  bool (*cancel_query)(DbConnection *conn, void *cancel_handle, char **err);
  void (*free_cancel_handle)(void *cancel_handle);
  /* Optional. Called (under the session lock) when the query a handle was
   * prepared for has finished while a cancel still uses the handle, so the
   * driver can skip a late cancel that would hit the connection's next query. */
  void (*cancel_handle_finished)(void *cancel_handle);

  /* Optional: is a transaction open on this connection right now (after
   * the last statement)? 0 = no, 1 = yes, 2 = yes but failed (PostgreSQL
   * "in error": only ROLLBACK can end it) */
  int (*transaction_state)(DbConnection *conn);

  /* Optional: the network path under the connection is gone (its SSH
   * tunnel was restarted). Reopen before the next statement, or, if a
   * transaction was open, refuse statements until ROLLBACK. */
  void (*connection_lost)(DbConnection *conn);

  /* Library init (optional; called once from db_init on the main thread,
   * before any worker exists: connects run on worker threads) */
  void (*library_init)(void);

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
  void *driver_data; /* Driver-specific data */
};

/* Driver registration */
void db_register_driver(DbDriver *driver);
DbDriver *db_get_driver(const char *name);
DbDriver **db_get_all_drivers(size_t *count);

/* High-level connection API */
DbConnection *db_connect(const char *connstr, char **err);
void db_disconnect(DbConnection *conn);
ResultSet *db_query(DbConnection *conn, const char *sql, char **err);

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

#endif /* LACE_DB_H */
