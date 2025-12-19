/*
 * lace - Database Viewer and Manager
 * Database manager - driver registry and high-level API
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
    if (str_eq(name, "postgresql") && str_eq(g_drivers[i]->name, "postgres")) {
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

  if (conn->driver->query_page) {
    return conn->driver->query_page(conn, table, offset, limit, order_by, desc,
                                    err);
  }

  /* Fallback: build SQL query with proper identifier escaping */
  char *escaped_table = str_escape_identifier_dquote(table);
  if (!escaped_table) {
    SET_ERROR(err, "Out of memory");
    return NULL;
  }

  char *sql;
  if (order_by && *order_by) {
    char *escaped_order = str_escape_identifier_dquote(order_by);
    if (!escaped_order) {
      free(escaped_table);
      SET_ERROR(err, "Out of memory");
      return NULL;
    }
    sql = str_printf("SELECT * FROM %s ORDER BY %s %s LIMIT %zu OFFSET %zu",
                     escaped_table, escaped_order, desc ? "DESC" : "ASC", limit,
                     offset);
    free(escaped_order);
  } else {
    sql = str_printf("SELECT * FROM %s LIMIT %zu OFFSET %zu", escaped_table,
                     limit, offset);
  }
  free(escaped_table);

  if (!sql) {
    SET_ERROR(err, "Out of memory");
    return NULL;
  }

  ResultSet *rs = db_query(conn, sql, err);
  free(sql);
  return rs;
}

int64_t db_count_rows(DbConnection *conn, const char *table, char **err) {
  if (!conn || !conn->driver || !table) {
    SET_ERROR(err, "Invalid parameters");
    return -1;
  }

  /* Build COUNT query with proper identifier escaping */
  char *escaped_table;
  if (str_eq(conn->driver->name, "mysql") ||
      str_eq(conn->driver->name, "mariadb")) {
    escaped_table = str_escape_identifier_backtick(table);
  } else {
    escaped_table = str_escape_identifier_dquote(table);
  }
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

  if (!rs) {
    return -1;
  }

  int64_t count = -1;
  if (rs->num_rows > 0 && rs->num_columns > 0 && rs->rows &&
      rs->rows[0].cells && rs->rows[0].num_cells > 0) {
    DbValue *val = &rs->rows[0].cells[0];
    if (val->type == DB_TYPE_INT) {
      count = val->int_val;
    } else if (val->type == DB_TYPE_TEXT && val->text.data) {
      char *endptr;
      errno = 0;
      long long parsed = strtoll(val->text.data, &endptr, 10);
      if (errno == 0 && endptr != val->text.data && *endptr == '\0') {
        count = parsed;
      }
      /* On parse error, count remains -1 */
    }
  }

  db_result_free(rs);
  return count;
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
