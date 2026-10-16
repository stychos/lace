/*
 * Lace
 * SQLite driver - uses libsqlite3 C API
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "util/mem.h"
#include "util/str.h"
#include "util/connstr.h"
#include "../db.h"
#include "../db_common.h"
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* SQLite connection data */
typedef struct {
  sqlite3 *db;
  char *path;
} SqliteData;

/* Forward declarations */
static DbConnection *sqlite_connect(const char *connstr, char **err);
static void sqlite_disconnect(DbConnection *conn);
static ResultSet *sqlite_query(DbConnection *conn, const char *sql, char **err);
static int sqlite_transaction_state(DbConnection *conn);
static void *sqlite_prepare_cancel(DbConnection *conn);
static bool sqlite_cancel_query(DbConnection *conn, void *cancel_handle,
                                char **err);
static void sqlite_free_cancel_handle(void *cancel_handle);

/* Driver definition */
DbDriver sqlite_driver = {
    .name = "sqlite",
    .display_name = "SQLite",
    .connect = sqlite_connect,
    .disconnect = sqlite_disconnect,
    .transaction_state = sqlite_transaction_state,
    .query = sqlite_query,
    .prepare_cancel = sqlite_prepare_cancel,
    .cancel_query = sqlite_cancel_query,
    .free_cancel_handle = sqlite_free_cancel_handle,
    .library_cleanup = NULL,
};

/* Get DbValue from SQLite column */
static DbValue sqlite_get_value(sqlite3_stmt *stmt, int col) {
  DbValue val;
  memset(&val, 0, sizeof(val));

  int type = sqlite3_column_type(stmt, col);

  if (type == SQLITE_NULL) {
    val.type = DB_TYPE_NULL;
    val.is_null = true;
    return val;
  }

  val.is_null = false;

  switch (type) {
  case SQLITE_INTEGER:
    val.type = DB_TYPE_INT;
    val.int_val = sqlite3_column_int64(stmt, col);
    break;

  case SQLITE_FLOAT:
    val.type = DB_TYPE_FLOAT;
    val.float_val = sqlite3_column_double(stmt, col);
    break;

  case SQLITE_TEXT: {
    const char *text = (const char *)sqlite3_column_text(stmt, col);
    int raw_len = sqlite3_column_bytes(stmt, col);
    if (!text || raw_len < 0) {
      val.type = DB_TYPE_NULL;
      val.is_null = true;
    } else {
      size_t len = (size_t)raw_len;
      if (len > LACE_MAX_FIELD_SIZE) {
        val = db_value_oversized_placeholder("TEXT", len);
      } else {
        val.text.data = safe_malloc(len + 1);
        memcpy(val.text.data, text, len);
        val.text.data[len] = '\0';
        val.text.len = len;
        val.type = DB_TYPE_TEXT;
      }
    }
    break;
  }

  case SQLITE_BLOB: {
    const void *blob = sqlite3_column_blob(stmt, col);
    int raw_len = sqlite3_column_bytes(stmt, col);
    if (!blob || raw_len <= 0) {
      /* A zero-length blob (SQLite returns no pointer for it): empty, not
       * NULL */
      val.type = DB_TYPE_BLOB;
      val.blob.data = NULL;
      val.blob.len = 0;
    } else {
      size_t len = (size_t)raw_len;
      if (len > LACE_MAX_FIELD_SIZE) {
        val = db_value_oversized_placeholder("BLOB", len);
      } else {
        val.blob.data = safe_malloc(len);
        memcpy(val.blob.data, blob, len);
        val.blob.len = len;
        val.type = DB_TYPE_BLOB;
      }
    }
    break;
  }

  default:
    val.type = DB_TYPE_NULL;
    val.is_null = true;
    break;
  }

  return val;
}

static DbConnection *sqlite_connect(const char *connstr, char **err) {
  ConnString *cs = connstr_parse(connstr, err);
  if (!cs)
    return NULL;

  if (!str_eq(cs->driver, "sqlite")) {
    connstr_free(cs);
    err_set(err, "Not a SQLite connection string");
    return NULL;
  }

  if (!cs->database || !cs->database[0]) {
    connstr_free(cs);
    err_set(err, "No database path specified");
    return NULL;
  }

  /* Open the database. A missing file is an error, not a new empty database
   * (a typo or a path relative to the wrong directory used to create one
   * silently); ?create=1 asks for creation explicitly. */
  const char *create = connstr_get_option(cs, "create");
  bool may_create = create && (str_eq(create, "1") || str_eq(create, "true"));
  int flags = SQLITE_OPEN_READWRITE | (may_create ? SQLITE_OPEN_CREATE : 0);
  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(cs->database, &db, flags, NULL);
  if (rc == SQLITE_CANTOPEN && !may_create && access(cs->database, F_OK) != 0) {
    err_setf(err, "Database file not found: %s (add ?create=1 to create it)",
             cs->database);
    if (db)
      sqlite3_close(db);
    connstr_free(cs);
    return NULL;
  }
  if (rc != SQLITE_OK) {
    err_setf(err, "Failed to open database: %s",
             db ? sqlite3_errmsg(db) : "unknown error");
    if (db)
      sqlite3_close(db);
    connstr_free(cs);
    return NULL;
  }

  /* Enable foreign keys */
  (void)sqlite3_exec(db, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

  SqliteData *data = safe_calloc(1, sizeof(SqliteData));
  data->db = db;
  data->path = str_dup(cs->database);

  DbConnection *conn = safe_calloc(1, sizeof(DbConnection));

  conn->driver = &sqlite_driver;
  conn->connstr = str_dup(connstr);

  /* Use just the filename for display, not full path */
  const char *basename = strrchr(cs->database, '/');
  if (basename) {
    basename++; /* Skip the '/' */
  } else {
    basename = cs->database; /* No '/' found, use as-is */
  }
  conn->database = str_dup(basename);
  conn->driver_data = data;

  connstr_free(cs);
  return conn;
}

static void sqlite_disconnect(DbConnection *conn) {
  if (!conn)
    return;

  SqliteData *data = conn->driver_data;
  if (data) {
    if (data->db)
      sqlite3_close(data->db);
    free(data->path);
    free(data);
  }

  db_common_free_connection(conn);
}

/* Run one prepared statement (finalized here): its rows if it has columns,
 * else an empty result carrying its row-change count */
static ResultSet *sqlite_run_one(SqliteData *data, sqlite3_stmt *stmt, char **err) {
  int rc;
  int changes_before = sqlite3_total_changes(data->db);
  ResultSet *rs = db_result_alloc_empty();
  if (!rs) {
    sqlite3_finalize(stmt);
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  /* Get column info */
  int num_cols = sqlite3_column_count(stmt);
  if (num_cols > 0) {
    rs->columns = safe_calloc(num_cols, sizeof(ColumnDef));
    rs->num_columns = num_cols;
    for (int i = 0; i < num_cols; i++) {
      rs->columns[i].name = str_dup(sqlite3_column_name(stmt, i));
      if (!rs->columns[i].name) {
        db_result_free(rs);
        sqlite3_finalize(stmt);
        err_set(err, "Memory allocation failed for column name");
        return NULL;
      }
      const char *type = sqlite3_column_decltype(stmt, i);
      if (type)
        rs->columns[i].type_name = str_dup(type);
    }
  }

  /* Collect rows */
  DbRowCollector rows;
  db_rows_init(&rows, rs);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    DbValue *cells = db_rows_add(&rows, (size_t)num_cols);
    if (!cells)
      break; /* Limit reached */
    for (int i = 0; i < num_cols; i++) {
      cells[i] = sqlite_get_value(stmt, i);
      db_rows_count(&rows, (size_t)sqlite3_column_bytes(stmt, i));
    }
  }

  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    if (err)
      *err = str_printf("Query execution failed: %s", sqlite3_errmsg(data->db));
    sqlite3_finalize(stmt);
    db_result_free(rs);
    return NULL;
  }
  sqlite3_finalize(stmt);

  if (num_cols == 0) {
    /* The statement's own rows, as the other drivers count them:
     * sqlite3_changes() leaves out what foreign-key actions and triggers
     * did (a DELETE with ON DELETE CASCADE children is one row), which
     * total_changes counts; but sqlite3_changes() keeps reporting the
     * previous DML statement's count after one that changes nothing (DDL),
     * so total_changes decides whether this statement changed anything */
    int total = sqlite3_total_changes(data->db) - changes_before;
    rs->rows_affected = total > 0 ? sqlite3_changes(data->db) : 0;
  }
  return rs;
}

/* Run every statement in sql (the old code ran only the first and silently
 * ignored the rest). Like the other drivers: the result of the last
 * statement that produced rows (SELECT, ... RETURNING), else the row count
 * of the last statement. */
static ResultSet *sqlite_query(DbConnection *conn, const char *sql,
                               char **err) {
  DB_REQUIRE_PARAMS_CONN(sql, conn, SqliteData, data, db, err, NULL);

  ResultSet *rows = NULL;
  int64_t last_affected = 0;
  const char *tail = sql;
  while (tail && *tail) {
    sqlite3_stmt *stmt = NULL;
    const char *next = NULL;
    int rc = sqlite3_prepare_v2(data->db, tail, -1, &stmt, &next);
    if (rc != SQLITE_OK) {
      /* The bare message, like the other drivers: callers add context */
      err_set(err, sqlite3_errmsg(data->db));
      db_result_free(rows);
      return NULL;
    }
    if (!stmt) {
      break; /* Only whitespace or comments left */
    }
    ResultSet *rs = sqlite_run_one(data, stmt, err);
    if (!rs) {
      db_result_free(rows);
      return NULL;
    }
    if (rs->num_columns > 0) {
      db_result_free(rows);
      rows = rs;
    } else {
      last_affected = rs->rows_affected;
      db_result_free(rs);
    }
    if (next == tail) {
      break;
    }
    tail = next;
  }

  if (rows) {
    return rows;
  }
  ResultSet *rs = db_result_alloc_empty();
  rs->rows_affected = last_affected;
  return rs;
}

/* Query cancellation support - SQLite uses sqlite3_interrupt */
static void *sqlite_prepare_cancel(DbConnection *conn) {
  if (!conn)
    return NULL;
  SqliteData *data = conn->driver_data;
  if (!data || !data->db)
    return NULL;
  /* Return the db handle itself - we don't need a separate handle */
  return data->db;
}

static bool sqlite_cancel_query(DbConnection *conn, void *cancel_handle,
                                char **err) {
  (void)conn; /* Not needed */
  if (!cancel_handle) {
    err_set(err, "Invalid cancel handle");
    return false;
  }

  sqlite3 *db = (sqlite3 *)cancel_handle;
  sqlite3_interrupt(db);
  return true;
}

static void sqlite_free_cancel_handle(void *cancel_handle) {
  /* No-op: we don't own the db handle, just borrowed the pointer */
  (void)cancel_handle;
}

static int sqlite_transaction_state(DbConnection *conn) {
  SqliteData *data = conn ? conn->driver_data : NULL;
  return data && data->db && !sqlite3_get_autocommit(data->db) ? 1 : 0;
}
