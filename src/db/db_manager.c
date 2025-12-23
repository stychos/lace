/*
 * Lace
 * Database manager - driver registry and high-level API
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../util/str.h"
#include "connstr.h"
#include "db.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DRIVERS 16

/* Helper to safely set error message (frees existing message first) */
#define SET_ERROR(err, msg)                                                    \
  do {                                                                         \
    if (err) {                                                                 \
      free(*err);                                                              \
      *err = str_dup(msg);                                                     \
    }                                                                          \
  } while (0)

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
    char *schema = strndup(table, schema_len);
    char *tbl = strdup(dot + 1);
    if (!schema || !tbl) {
      free(schema);
      free(tbl);
      return NULL;
    }

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
    if (err)
      *err = str_printf("Unknown driver: %s", cs->driver);
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
  conn->driver->disconnect(conn);
}

bool db_ping(DbConnection *conn) {
  if (!conn || !conn->driver || !conn->driver->ping)
    return false;
  return conn->driver->ping(conn);
}

char **db_list_databases(DbConnection *conn, size_t *count, char **err) {
  if (!conn || !conn->driver || !conn->driver->list_databases) {
    SET_ERROR(err, "Not supported");
    return NULL;
  }
  return conn->driver->list_databases(conn, count, err);
}

char **db_list_tables(DbConnection *conn, size_t *count, char **err) {
  if (!conn || !conn->driver || !conn->driver->list_tables) {
    SET_ERROR(err, "Not supported");
    return NULL;
  }
  return conn->driver->list_tables(conn, count, err);
}

TableSchema *db_get_table_schema(DbConnection *conn, const char *table,
                                 char **err) {
  if (!conn || !conn->driver || !conn->driver->get_table_schema) {
    SET_ERROR(err, "Not supported");
    return NULL;
  }
  return conn->driver->get_table_schema(conn, table, err);
}

ResultSet *db_query(DbConnection *conn, const char *sql, char **err) {
  if (!conn || !conn->driver || !conn->driver->query) {
    SET_ERROR(err, "Not supported");
    return NULL;
  }
  return conn->driver->query(conn, sql, err);
}

int64_t db_exec(DbConnection *conn, const char *sql, char **err) {
  if (!conn || !conn->driver || !conn->driver->exec) {
    SET_ERROR(err, "Not supported");
    return -1;
  }
  return conn->driver->exec(conn, sql, err);
}

ResultSet *db_query_page(DbConnection *conn, const char *table, size_t offset,
                         size_t limit, const char *order_by, bool desc,
                         char **err) {
  if (!conn || !conn->driver) {
    SET_ERROR(err, "Not connected");
    return NULL;
  }

  /* Use driver-specific implementation if available */
  if (conn->driver->query_page) {
    return conn->driver->query_page(conn, table, offset, limit, order_by, desc,
                                    err);
  }

  /* Fall back to generic implementation */
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
    if (errno == 0 && endptr != val->text.data && *endptr == '\0')
      return parsed;
  }
  return -1;
}

int64_t db_count_rows(DbConnection *conn, const char *table, char **err) {
  if (!conn || !conn->driver || !table) {
    SET_ERROR(err, "Invalid parameters");
    return -1;
  }

  char *escaped_table = escape_table_name(conn, table);
  if (!escaped_table) {
    SET_ERROR(err, "Out of memory");
    return -1;
  }

  char *sql = str_printf("SELECT COUNT(*) FROM %s", escaped_table);
  free(escaped_table);

  if (!sql) {
    SET_ERROR(err, "Out of memory");
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
    SET_ERROR(err, "Invalid parameters");
    return -1;
  }

  char *escaped_table = escape_table_name(conn, table);
  if (!escaped_table) {
    SET_ERROR(err, "Out of memory");
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
    SET_ERROR(err, "Out of memory");
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
    SET_ERROR(err, "Invalid parameters");
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
    SET_ERROR(err, "Not connected");
    return NULL;
  }

  /* Build SQL query with proper identifier escaping (handles schema.table) */
  char *escaped_table = escape_table_name(conn, table);
  if (!escaped_table) {
    SET_ERROR(err, "Out of memory");
    return NULL;
  }

  StringBuilder *sb = sb_new(256);
  if (!sb) {
    free(escaped_table);
    SET_ERROR(err, "Out of memory");
    return NULL;
  }

  sb_printf(sb, "SELECT * FROM %s", escaped_table);

  if (where_clause && *where_clause) {
    sb_printf(sb, " WHERE %s", where_clause);
  }

  if (order_by && *order_by) {
    char *escaped_order = escape_identifier(conn, order_by);
    if (escaped_order) {
      sb_printf(sb, " ORDER BY %s %s", escaped_order, desc ? "DESC" : "ASC");
      free(escaped_order);
    }
  }

  sb_printf(sb, " LIMIT %zu OFFSET %zu", limit, offset);

  char *sql = sb_to_string(sb);
  free(escaped_table);

  if (!sql) {
    SET_ERROR(err, "Out of memory");
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
    SET_ERROR(err, "Not supported");
    return false;
  }
  return conn->driver->update_cell(conn, table, pk_cols, pk_vals, num_pk_cols,
                                   col, new_val, err);
}

bool db_insert_row(DbConnection *conn, const char *table, const ColumnDef *cols,
                   const DbValue *vals, size_t num_cols, char **err) {
  if (!conn || !conn->driver || !conn->driver->insert_row) {
    SET_ERROR(err, "Not supported");
    return false;
  }
  return conn->driver->insert_row(conn, table, cols, vals, num_cols, err);
}

bool db_delete_row(DbConnection *conn, const char *table, const char **pk_cols,
                   const DbValue *pk_vals, size_t num_pk_cols, char **err) {
  if (!conn || !conn->driver || !conn->driver->delete_row) {
    SET_ERROR(err, "Not supported");
    return false;
  }
  return conn->driver->delete_row(conn, table, pk_cols, pk_vals, num_pk_cols,
                                  err);
}

bool db_begin_transaction(DbConnection *conn, char **err) {
  if (!conn || !conn->driver) {
    SET_ERROR(err, "Not connected");
    return false;
  }

  if (conn->driver->begin_transaction) {
    return conn->driver->begin_transaction(conn, err);
  }

  /* Fallback */
  return db_exec(conn, "BEGIN", err) >= 0;
}

bool db_commit(DbConnection *conn, char **err) {
  if (!conn || !conn->driver) {
    SET_ERROR(err, "Not connected");
    return false;
  }

  if (conn->driver->commit) {
    return conn->driver->commit(conn, err);
  }

  /* Fallback */
  return db_exec(conn, "COMMIT", err) >= 0;
}

bool db_rollback(DbConnection *conn, char **err) {
  if (!conn || !conn->driver) {
    SET_ERROR(err, "Not connected");
    return false;
  }

  if (conn->driver->rollback) {
    return conn->driver->rollback(conn, err);
  }

  /* Fallback */
  return db_exec(conn, "ROLLBACK", err) >= 0;
}
