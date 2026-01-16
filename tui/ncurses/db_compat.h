/*
 * Lace TUI
 * Database compatibility layer - maps old db types to liblace
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_DB_COMPAT_H
#define LACE_DB_COMPAT_H

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
#define DB_TYPE_TEXT      LACE_TYPE_TEXT
#define DB_TYPE_BLOB      LACE_TYPE_BLOB
#define DB_TYPE_BOOL      LACE_TYPE_BOOL
#define DB_TYPE_DATE      LACE_TYPE_DATE
#define DB_TYPE_TIMESTAMP LACE_TYPE_TIMESTAMP

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
typedef struct DbConnection {
  int conn_id;              /* liblace connection ID */
  lace_client_t *client;    /* Back-reference to client */
  char *connstr;            /* Connection string (for display) */
  char *database;           /* Database name */
  char *host;               /* Host (NULL for SQLite) */
  int port;                 /* Port */
  char *user;               /* Username */
  LaceDriver driver_type;   /* Driver type enum */
  const DbDriver *driver;   /* Driver stub (for conn->driver->name) */
  char *last_error;         /* Last error message */
} DbConnection;

/* Connection status (compatibility) */
typedef enum {
  CONN_STATUS_DISCONNECTED,
  CONN_STATUS_CONNECTING,
  CONN_STATUS_CONNECTED,
  CONN_STATUS_ERROR
} ConnStatus;

/* Create a DbConnection wrapper from liblace connection info */
DbConnection *db_conn_wrap(lace_client_t *client, int conn_id, const char *connstr);

/* Free a DbConnection wrapper (does NOT close the connection) */
void db_conn_free(DbConnection *conn);

/* Get driver name from connection */
const char *db_conn_driver_name(DbConnection *conn);

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

/* Free table list */
void db_free_tables(char **tables, size_t count);

/* Get table schema */
TableSchema *db_get_table_schema(DbConnection *conn, const char *table, char **err);

/* Execute query */
ResultSet *db_query(DbConnection *conn, const char *sql, char **err);

/* Execute non-SELECT statement */
int64_t db_exec(DbConnection *conn, const char *sql, char **err);

/* Query table with pagination and optional filters/sorts */
ResultSet *db_query_table(DbConnection *conn, const char *table,
                          const LaceFilter *filters, size_t num_filters,
                          const LaceSort *sorts, size_t num_sorts,
                          size_t offset, size_t limit, char **err);

/* Query page (simple pagination without filters - uses db_query_table internally) */
ResultSet *db_query_page(DbConnection *conn, const char *table,
                         size_t offset, size_t limit,
                         const char *order_clause, bool order_desc, char **err);

/* Query page with WHERE clause (uses raw SQL) */
ResultSet *db_query_page_where(DbConnection *conn, const char *table,
                               size_t offset, size_t limit,
                               const char *where_clause,
                               const char *order_clause, bool order_desc,
                               char **err);

/* Count rows in table (with optional filters) */
bool db_count_rows(DbConnection *conn, const char *table,
                   const LaceFilter *filters, size_t num_filters,
                   size_t *count, bool *approximate, char **err);

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

/* Cancel a running query on a connection */
bool db_cancel_query(DbConnection *conn, char **err);

/* Get driver stub from connection (for conn->driver->name) */
const DbDriver *db_get_driver(DbConnection *conn);

/* ==========================================================================
 * Function aliases
 * ========================================================================== */

/* Value creation */
#define db_value_null()           lace_value_null()
#define db_value_int(v)           lace_value_int(v)
#define db_value_float(v)         lace_value_float(v)
#define db_value_text(s)          lace_value_text(s)
#define db_value_text_len(s, l)   lace_value_text_len(s, l)
#define db_value_blob(d, l)       lace_value_blob(d, l)
#define db_value_bool(v)          lace_value_bool(v)
#define db_value_copy(v)          lace_value_copy(v)

/* Value conversion */
#define db_value_to_string(v)     lace_value_to_string(v)

/* Memory management */
#define db_value_free(v)          lace_value_free(v)
#define db_row_free(r)            lace_row_free(r)
#define db_result_free(r)         lace_result_free(r)
#define db_schema_free(s)         lace_schema_free(s)

/* Type info */
#define db_value_type_name(t)     lace_type_name(t)

#endif /* LACE_DB_COMPAT_H */
