/*
 * Lace
 * Database manager - driver registry and high-level API
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../core/constants.h"
#include "../util/str.h"
#include "connstr.h"
#include "db.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helper to invoke history callback if set */
static inline void db_record_history(DbConnection *conn, const char *sql,
                                     int type) {
  if (conn && conn->history_callback && sql) {
    conn->history_callback(conn->history_context, sql, type);
  }
}

/* Global driver registry */
static DbDriver *g_drivers[MAX_DRIVERS];
static size_t g_num_drivers = 0;
static bool g_initialized = false;

/* Escape a simple identifier (column name, etc.) for SQL.
 * MySQL/MariaDB use backticks, PostgreSQL/SQLite use double quotes. */
static char *escape_identifier(DbConnection *conn, const char *name) {
  if (!conn || !conn->driver || !name)
    return NULL;

  if (str_eq(conn->driver->name, "mysql") ||
      str_eq(conn->driver->name, "mariadb")) {
    return str_escape_identifier_backtick(name);
  }
  return str_escape_identifier_dquote(name);
}

/* Public wrapper for escape_identifier */
char *db_escape_identifier(DbConnection *conn, const char *name) {
  return escape_identifier(conn, name);
}

/* Escape table name for SQL, handling schema-qualified names for PostgreSQL.
 * MySQL/MariaDB use backticks, PostgreSQL/SQLite use double quotes.
 * For PostgreSQL, schema.table becomes "schema"."table". */
static char *escape_table_name(DbConnection *conn, const char *table) {
  if (!conn || !conn->driver || !table)
    return NULL;

  /* For PostgreSQL, handle schema.table format */
  const char *dot = strchr(table, '.');
  if (dot && str_eq(conn->driver->name, "postgres")) {
    /* Schema-qualified: escape each part separately */
    size_t schema_len = (size_t)(dot - table);
    char *schema = str_ndup(table, schema_len);
    char *tbl = str_dup(dot + 1);

    char *escaped_schema = str_escape_identifier_dquote(schema);
    char *escaped_table = str_escape_identifier_dquote(tbl);
    free(schema);
    free(tbl);

    if (!escaped_schema || !escaped_table) {
      free(escaped_schema);
      free(escaped_table);
      return NULL;
    }

    char *result = str_printf("%s.%s", escaped_schema, escaped_table);
    free(escaped_schema);
    free(escaped_table);
    return result;
  }

  /* Simple case: use generic identifier escaping */
  return escape_identifier(conn, table);
}

/* Escape a value for SQL (escape single quotes by doubling) */
static char *escape_sql_value(const char *value) {
  if (!value)
    return str_dup("NULL");

  size_t len = strlen(value);
  /* Worst case: every char is a quote, plus surrounding quotes + null */
  if (len >= (SIZE_MAX - 3) / 2)
    return NULL;

  StringBuilder *sb = sb_new(len * 2 + 3);
  if (!sb)
    return NULL;

  sb_append_char(sb, '\'');
  for (const char *p = value; *p; p++) {
    if (*p == '\'') {
      sb_append(sb, "''"); /* Escape by doubling */
    } else {
      sb_append_char(sb, *p);
    }
  }
  sb_append_char(sb, '\'');

  return sb_to_string(sb);
}

/* Forward declarations for built-in drivers */
extern DbDriver sqlite_driver;
extern DbDriver postgres_driver;
extern DbDriver mysql_driver;
extern DbDriver mariadb_driver;

void db_init(void) {
  if (g_initialized)
    return;

  /* Register built-in drivers */
  db_register_driver(&sqlite_driver);
  db_register_driver(&postgres_driver);
  db_register_driver(&mysql_driver);
  db_register_driver(&mariadb_driver);

  g_initialized = true;
}

void db_cleanup(void) {
  /* Call library cleanup for all drivers */
  for (size_t i = 0; i < g_num_drivers; i++) {
    if (g_drivers[i] && g_drivers[i]->library_cleanup) {
      g_drivers[i]->library_cleanup();
    }
  }

  g_num_drivers = 0;
  g_initialized = false;
}

void db_register_driver(DbDriver *driver) {
  if (!driver || g_num_drivers >= MAX_DRIVERS)
    return;

  /* Check for duplicate */
  for (size_t i = 0; i < g_num_drivers; i++) {
    if (str_eq(g_drivers[i]->name, driver->name)) {
      g_drivers[i] = driver; /* Replace */
      return;
    }
  }

  g_drivers[g_num_drivers++] = driver;
}

DbDriver *db_get_driver(const char *name) {
  if (!name)
    return NULL;

  for (size_t i = 0; i < g_num_drivers; i++) {
    if (str_eq(g_drivers[i]->name, name)) {
      return g_drivers[i];
    }
    /* Handle aliases */
    if ((str_eq(name, "postgresql") || str_eq(name, "pg")) &&
        str_eq(g_drivers[i]->name, "postgres")) {
      return g_drivers[i];
    }
    if (str_eq(name, "mariadb") && str_eq(g_drivers[i]->name, "mysql")) {
      return g_drivers[i];
    }
  }

  return NULL;
}

DbDriver **db_get_all_drivers(size_t *count) {
  if (count)
    *count = g_num_drivers;
  return g_drivers;
}

/* Connection helpers */

void db_set_error(DbConnection *conn, const char *fmt, ...) {
  if (!conn)
    return;

  free(conn->last_error);

  va_list args;
  va_start(args, fmt);
  conn->last_error = str_vprintf(fmt, args);
  va_end(args);

  conn->status = CONN_STATUS_ERROR;
}

void db_clear_error(DbConnection *conn) {
  if (!conn)
    return;

  free(conn->last_error);
  conn->last_error = NULL;

  if (conn->status == CONN_STATUS_ERROR) {
    conn->status = CONN_STATUS_CONNECTED;
  }
}

const char *db_get_error(DbConnection *conn) {
  return conn ? conn->last_error : NULL;
}

ConnStatus db_status(DbConnection *conn) {
  return conn ? conn->status : CONN_STATUS_DISCONNECTED;
}

/* High-level API */

DbConnection *db_connect(const char *connstr, char **err) {
  if (!g_initialized) {
    db_init();
  }

  ConnString *cs = connstr_parse(connstr, err);
  if (!cs) {
    return NULL;
  }

  if (!connstr_validate(cs, err)) {
    connstr_free(cs);
    return NULL;
  }

  DbDriver *driver = db_get_driver(cs->driver);
  if (!driver) {
    if (err) {
      *err = str_printf("Unknown driver: %s", cs->driver);
      /* str_printf can return NULL on allocation failure - callers handle this */
    }
    connstr_free(cs);
    return NULL;
  }

  DbConnection *conn = driver->connect(connstr, err);
  connstr_free(cs);

  return conn;
}

void db_disconnect(DbConnection *conn) {
  if (!conn || !conn->driver)
    return;

  /* Clear transaction state - connection close implicitly rolls back */
  conn->in_transaction = false;
  conn->transaction_depth = 0;

  conn->driver->disconnect(conn);
}

bool db_ping(DbConnection *conn) {
  if (!conn || !conn->driver || !conn->driver->ping)
    return false;
  return conn->driver->ping(conn);
}

char **db_list_databases(DbConnection *conn, size_t *count, char **err) {
  if (!conn || !conn->driver || !conn->driver->list_databases) {
    err_set(err, "Not supported");
    return NULL;
  }
  return conn->driver->list_databases(conn, count, err);
}

char **db_list_tables(DbConnection *conn, size_t *count, char **err) {
  if (!conn || !conn->driver || !conn->driver->list_tables) {
    err_set(err, "Not supported");
    return NULL;
  }
  return conn->driver->list_tables(conn, count, err);
}

TableSchema *db_get_table_schema(DbConnection *conn, const char *table,
                                 char **err) {
  if (!conn || !conn->driver || !conn->driver->get_table_schema) {
    err_set(err, "Not supported");
    return NULL;
  }
  return conn->driver->get_table_schema(conn, table, err);
}

ResultSet *db_query(DbConnection *conn, const char *sql, char **err) {
  if (!conn || !conn->driver || !conn->driver->query) {
    err_set(err, "Not supported");
    return NULL;
  }
  ResultSet *rs = conn->driver->query(conn, sql, err);
  if (rs) {
    db_record_history(conn, sql, DB_HISTORY_AUTO);
  }
  return rs;
}

int64_t db_exec(DbConnection *conn, const char *sql, char **err) {
  if (!conn || !conn->driver || !conn->driver->exec) {
    err_set(err, "Not supported");
    return -1;
  }
  int64_t affected = conn->driver->exec(conn, sql, err);
  if (affected >= 0) {
    db_record_history(conn, sql, DB_HISTORY_AUTO);
  }
  return affected;
}

ResultSet *db_query_page(DbConnection *conn, const char *table, size_t offset,
                         size_t limit, const char *order_by, bool desc,
                         char **err) {
  if (!conn || !conn->driver) {
    err_set(err, "Not connected");
    return NULL;
  }

  /* Use driver-specific implementation if available */
  if (conn->driver->query_page) {
    ResultSet *rs = conn->driver->query_page(conn, table, offset, limit,
                                             order_by, desc, err);
    /* Record history for driver-specific query_page */
    if (rs && conn->history_callback) {
      char *sql = str_printf("SELECT * FROM %s LIMIT %zu OFFSET %zu", table,
                             limit, offset);
      if (sql) {
        db_record_history(conn, sql, DB_HISTORY_SELECT);
        free(sql);
      }
    }
    return rs;
  }

  /* Fall back to generic implementation (db_query handles history) */
  return db_query_page_where(conn, table, offset, limit, NULL, order_by, desc,
                             err);
}

/* Extract count from a single-cell result set */
static int64_t extract_count_from_result(ResultSet *rs) {
  if (!rs || rs->num_rows == 0 || rs->num_columns == 0 || !rs->rows ||
      !rs->rows[0].cells || rs->rows[0].num_cells == 0)
    return -1;

  DbValue *val = &rs->rows[0].cells[0];
  if (val->type == DB_TYPE_INT)
    return val->int_val;

  if (val->type == DB_TYPE_TEXT && val->text.data) {
    char *endptr;
    errno = 0;
    long long parsed = strtoll(val->text.data, &endptr, 10);

    /* Check for parsing errors and overflow */
    if (errno == ERANGE || endptr == val->text.data || *endptr != '\0')
      return -1;

    /* Validate result fits in int64_t (should always be true for long long,
     * but be defensive for portability) */
    if (parsed < INT64_MIN || parsed > INT64_MAX)
      return -1;

    return (int64_t)parsed;
  }
  return -1;
}

int64_t db_count_rows(DbConnection *conn, const char *table, char **err) {
  if (!conn || !conn->driver || !table) {
    err_set(err, "Invalid parameters");
    return -1;
  }

  char *escaped_table = escape_table_name(conn, table);
  if (!escaped_table) {
    err_set(err, "Out of memory");
    return -1;
  }

  char *sql = str_printf("SELECT COUNT(*) FROM %s", escaped_table);
  free(escaped_table);

  if (!sql) {
    err_set(err, "Out of memory");
    return -1;
  }

  ResultSet *rs = db_query(conn, sql, err);
  free(sql);

  if (!rs)
    return -1;

  int64_t count = extract_count_from_result(rs);
  db_result_free(rs);
  return count;
}

int64_t db_count_rows_where(DbConnection *conn, const char *table,
                            const char *where_clause, char **err) {
  if (!conn || !conn->driver || !table) {
    err_set(err, "Invalid parameters");
    return -1;
  }

  char *escaped_table = escape_table_name(conn, table);
  if (!escaped_table) {
    err_set(err, "Out of memory");
    return -1;
  }

  char *sql;
  if (where_clause && *where_clause) {
    sql = str_printf("SELECT COUNT(*) FROM %s WHERE %s", escaped_table,
                     where_clause);
  } else {
    sql = str_printf("SELECT COUNT(*) FROM %s", escaped_table);
  }
  free(escaped_table);

  if (!sql) {
    err_set(err, "Out of memory");
    return -1;
  }

  ResultSet *rs = db_query(conn, sql, err);
  free(sql);

  if (!rs)
    return -1;

  int64_t count = extract_count_from_result(rs);
  db_result_free(rs);
  return count;
}

int64_t db_count_rows_fast(DbConnection *conn, const char *table,
                           bool allow_approximate, bool *is_approximate,
                           char **err) {
  if (!conn || !conn->driver || !table) {
    err_set(err, "Invalid parameters");
    if (is_approximate)
      *is_approximate = false;
    return -1;
  }

  /* Try approximate count first if allowed and driver supports it */
  if (allow_approximate && conn->driver->estimate_row_count) {
    int64_t estimate = conn->driver->estimate_row_count(conn, table, NULL);
    if (estimate >= 0) {
      if (is_approximate)
        *is_approximate = true;
      return estimate;
    }
    /* Fall through to exact count if estimate failed */
  }

  /* Fall back to exact COUNT(*) */
  if (is_approximate)
    *is_approximate = false;
  return db_count_rows(conn, table, err);
}

ResultSet *db_query_page_where(DbConnection *conn, const char *table,
                               size_t offset, size_t limit,
                               const char *where_clause, const char *order_by,
                               bool desc, char **err) {
  if (!conn || !conn->driver) {
    err_set(err, "Not connected");
    return NULL;
  }

  /* Build SQL query with proper identifier escaping (handles schema.table) */
  char *escaped_table = escape_table_name(conn, table);
  if (!escaped_table) {
    err_set(err, "Out of memory");
    return NULL;
  }

  StringBuilder *sb = sb_new(256);
  if (!sb) {
    free(escaped_table);
    err_set(err, "Out of memory");
    return NULL;
  }

  bool ok = sb_printf(sb, "SELECT * FROM %s", escaped_table);

  if (ok && where_clause && *where_clause) {
    ok = sb_printf(sb, " WHERE %s", where_clause);
  }

  if (ok && order_by && *order_by) {
    if (db_order_is_prebuilt(order_by)) {
      /* Pre-built clause - use directly */
      ok = sb_printf(sb, " ORDER BY %s", order_by);
    } else {
      /* Single column - escape and add direction */
      char *escaped_order = escape_identifier(conn, order_by);
      if (escaped_order) {
        ok = sb_printf(sb, " ORDER BY %s %s", escaped_order,
                       desc ? "DESC" : "ASC");
        free(escaped_order);
      }
    }
  }

  if (ok) {
    ok = sb_printf(sb, " LIMIT %zu OFFSET %zu", limit, offset);
  }

  if (!ok) {
    sb_free(sb);
    free(escaped_table);
    err_set(err, "Out of memory building query");
    return NULL;
  }

  char *sql = sb_to_string(sb);
  free(escaped_table);

  if (!sql) {
    err_set(err, "Out of memory");
    return NULL;
  }

  ResultSet *rs = db_query(conn, sql, err);
  free(sql);
  return rs;
}

bool db_update_cell(DbConnection *conn, const char *table, const char **pk_cols,
                    const DbValue *pk_vals, size_t num_pk_cols, const char *col,
                    const DbValue *new_val, char **err) {
  if (!conn || !conn->driver || !conn->driver->update_cell) {
    err_set(err, "Not supported");
    return false;
  }
  bool success = conn->driver->update_cell(conn, table, pk_cols, pk_vals,
                                           num_pk_cols, col, new_val, err);
  if (success && conn->history_callback) {
    /* Build SQL for history with proper escaping */
    char *escaped_table = escape_table_name(conn, table);
    char *escaped_col = escape_identifier(conn, col);
    char *val_str = new_val ? db_value_to_string(new_val) : NULL;
    /* Check if db_value_to_string failed (returned NULL for non-NULL input) */
    if (new_val && !val_str) {
      free(escaped_table);
      free(escaped_col);
      return success; /* Skip history on allocation failure */
    }
    char *escaped_val = escape_sql_value(val_str);
    free(val_str);

    char *where_str = NULL;
    bool alloc_ok = escaped_table && escaped_col && escaped_val;
    for (size_t i = 0; i < num_pk_cols && alloc_ok; i++) {
      char *pk_val_str = db_value_to_string(&pk_vals[i]);
      char *escaped_pk_col = escape_identifier(conn, pk_cols[i]);
      char *escaped_pk_val = escape_sql_value(pk_val_str);
      free(pk_val_str);

      if (!escaped_pk_col || !escaped_pk_val) {
        free(escaped_pk_col);
        free(escaped_pk_val);
        alloc_ok = false;
        break;
      }

      char *new_where;
      if (i == 0) {
        new_where = str_printf("%s = %s", escaped_pk_col, escaped_pk_val);
      } else {
        new_where = str_printf("%s AND %s = %s", where_str, escaped_pk_col,
                               escaped_pk_val);
      }
      free(escaped_pk_col);
      free(escaped_pk_val);
      free(where_str);
      where_str = new_where;
      if (!new_where)
        alloc_ok = false;
    }

    if (alloc_ok && where_str) {
      char *sql = str_printf("UPDATE %s SET %s = %s WHERE %s", escaped_table,
                             escaped_col, escaped_val, where_str);
      if (sql) {
        db_record_history(conn, sql, DB_HISTORY_UPDATE);
        free(sql);
      }
    }
    free(escaped_table);
    free(escaped_col);
    free(escaped_val);
    free(where_str);
  }
  return success;
}

bool db_insert_row(DbConnection *conn, const char *table, const ColumnDef *cols,
                   const DbValue *vals, size_t num_cols, char **err) {
  if (!conn || !conn->driver || !conn->driver->insert_row) {
    err_set(err, "Not supported");
    return false;
  }
  bool success = conn->driver->insert_row(conn, table, cols, vals, num_cols, err);
  if (success && conn->history_callback) {
    /* Build SQL for history with proper escaping */
    char *escaped_table = escape_table_name(conn, table);
    if (!escaped_table)
      return success; /* Skip history if allocation fails */

    char *col_list = NULL;
    char *val_list = NULL;
    bool alloc_ok = true;
    for (size_t i = 0; i < num_cols && alloc_ok; i++) {
      char *escaped_col = escape_identifier(conn, cols[i].name);
      char *val_str = db_value_to_string(&vals[i]);
      char *escaped_val = escape_sql_value(val_str);
      free(val_str);

      if (!escaped_col || !escaped_val) {
        free(escaped_col);
        free(escaped_val);
        alloc_ok = false;
        break;
      }

      char *new_cols, *new_vals;
      if (i == 0) {
        new_cols = str_printf("%s", escaped_col);
        new_vals = str_printf("%s", escaped_val);
      } else {
        new_cols = str_printf("%s, %s", col_list, escaped_col);
        new_vals = str_printf("%s, %s", val_list, escaped_val);
      }
      free(escaped_col);
      free(escaped_val);
      free(col_list);
      free(val_list);
      col_list = new_cols;
      val_list = new_vals;
      if (!new_cols || !new_vals)
        alloc_ok = false;
    }

    if (alloc_ok && col_list && val_list) {
      char *sql = str_printf("INSERT INTO %s (%s) VALUES (%s)", escaped_table,
                             col_list, val_list);
      if (sql) {
        db_record_history(conn, sql, DB_HISTORY_INSERT);
        free(sql);
      }
    }
    free(escaped_table);
    free(col_list);
    free(val_list);
  }
  return success;
}

bool db_delete_row(DbConnection *conn, const char *table, const char **pk_cols,
                   const DbValue *pk_vals, size_t num_pk_cols, char **err) {
  if (!conn || !conn->driver || !conn->driver->delete_row) {
    err_set(err, "Not supported");
    return false;
  }
  bool success = conn->driver->delete_row(conn, table, pk_cols, pk_vals,
                                          num_pk_cols, err);
  if (success && conn->history_callback) {
    /* Build SQL for history with proper escaping */
    char *escaped_table = escape_table_name(conn, table);
    if (!escaped_table)
      return success; /* Skip history if allocation fails */

    char *where_str = NULL;
    bool alloc_ok = true;
    for (size_t i = 0; i < num_pk_cols && alloc_ok; i++) {
      char *escaped_pk_col = escape_identifier(conn, pk_cols[i]);
      char *pk_val_str = db_value_to_string(&pk_vals[i]);
      char *escaped_pk_val = escape_sql_value(pk_val_str);
      free(pk_val_str);

      if (!escaped_pk_col || !escaped_pk_val) {
        free(escaped_pk_col);
        free(escaped_pk_val);
        alloc_ok = false;
        break;
      }

      char *new_where;
      if (i == 0) {
        new_where = str_printf("%s = %s", escaped_pk_col, escaped_pk_val);
      } else {
        new_where = str_printf("%s AND %s = %s", where_str, escaped_pk_col,
                               escaped_pk_val);
      }
      free(escaped_pk_col);
      free(escaped_pk_val);
      free(where_str);
      where_str = new_where;
      if (!new_where)
        alloc_ok = false;
    }

    if (alloc_ok && where_str) {
      char *sql = str_printf("DELETE FROM %s WHERE %s", escaped_table, where_str);
      if (sql) {
        db_record_history(conn, sql, DB_HISTORY_DELETE);
        free(sql);
      }
    }
    free(escaped_table);
    free(where_str);
  }
  return success;
}

bool db_begin_transaction(DbConnection *conn, char **err) {
  if (!conn || !conn->driver) {
    err_set(err, "Not connected");
    return false;
  }

  bool success;
  if (conn->driver->begin_transaction) {
    success = conn->driver->begin_transaction(conn, err);
  } else {
    /* Fallback */
    success = db_exec(conn, "BEGIN", err) >= 0;
  }

  if (success) {
    conn->in_transaction = true;
    conn->transaction_depth = 1;
  }
  return success;
}

bool db_commit(DbConnection *conn, char **err) {
  if (!conn || !conn->driver) {
    err_set(err, "Not connected");
    return false;
  }

  bool success;
  if (conn->driver->commit) {
    success = conn->driver->commit(conn, err);
  } else {
    /* Fallback */
    success = db_exec(conn, "COMMIT", err) >= 0;
  }

  if (success) {
    conn->in_transaction = false;
    conn->transaction_depth = 0;
  }
  return success;
}

bool db_rollback(DbConnection *conn, char **err) {
  if (!conn || !conn->driver) {
    err_set(err, "Not connected");
    return false;
  }

  bool success;
  if (conn->driver->rollback) {
    success = conn->driver->rollback(conn, err);
  } else {
    /* Fallback */
    success = db_exec(conn, "ROLLBACK", err) >= 0;
  }

  /* Always clear transaction state on rollback attempt */
  conn->in_transaction = false;
  conn->transaction_depth = 0;
  return success;
}

bool db_in_transaction(DbConnection *conn) {
  return conn && conn->in_transaction;
}

/* Transaction context API - auto-rollback on scope exit or error */

DbTransaction db_transaction_begin(DbConnection *conn, char **err) {
  DbTransaction txn = {
      .conn = conn, .committed = false, .owns_transaction = false};

  if (!conn) {
    err_set(err, "Not connected");
    return txn;
  }

  /* If already in transaction, participate but don't own */
  if (conn->in_transaction) {
    if (conn->transaction_depth >= MAX_TRANSACTION_DEPTH) {
      err_set(err, "Maximum transaction nesting depth exceeded");
      return txn;
    }
    conn->transaction_depth++;
    return txn;
  }

  /* Start new transaction */
  if (db_begin_transaction(conn, err)) {
    txn.owns_transaction = true;
  }
  return txn;
}

bool db_transaction_commit(DbTransaction *txn, char **err) {
  if (!txn || !txn->conn || txn->committed) {
    return false;
  }

  /* Only actually commit if we own the transaction */
  if (txn->owns_transaction) {
    if (!db_commit(txn->conn, err)) {
      return false; /* Don't set committed - allow auto-rollback */
    }
    txn->committed = true;
    return true;
  }

  /* Nested: just decrement depth */
  if (txn->conn->transaction_depth > 0) {
    txn->conn->transaction_depth--;
  }
  txn->committed = true;
  return true;
}

bool db_transaction_rollback(DbTransaction *txn, char **err) {
  if (!txn || !txn->conn) {
    return false;
  }

  /* Mark as "committed" to prevent double rollback in end */
  txn->committed = true;

  /* Always rollback the whole transaction */
  return db_rollback(txn->conn, err);
}

void db_transaction_end(DbTransaction *txn) {
  if (!txn || !txn->conn || txn->committed) {
    return;
  }

  /* Auto-rollback if not committed */
  db_rollback(txn->conn, NULL);
  txn->committed = true;
}
