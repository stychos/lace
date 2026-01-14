/*
 * Lace
 * SQLite driver - uses libsqlite3 C API
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../util/mem.h"
#include "../../util/str.h"
#include "../connstr.h"
#include "../db.h"
#include "../db_common.h"
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SQLite connection data */
typedef struct {
  sqlite3 *db;
  char *path;
} SqliteData;

/* Forward declarations */
static DbConnection *sqlite_connect(const char *connstr, char **err);
static void sqlite_disconnect(DbConnection *conn);
static bool sqlite_ping(DbConnection *conn);
static ConnStatus sqlite_status(DbConnection *conn);
static const char *sqlite_get_error(DbConnection *conn);
static char **sqlite_list_tables(DbConnection *conn, size_t *count, char **err);
static TableSchema *sqlite_get_table_schema(DbConnection *conn,
                                            const char *table, char **err);
static ResultSet *sqlite_query(DbConnection *conn, const char *sql, char **err);
static int64_t sqlite_exec(DbConnection *conn, const char *sql, char **err);
static ResultSet *sqlite_query_page(DbConnection *conn, const char *table,
                                    size_t offset, size_t limit,
                                    const char *order_by, bool desc,
                                    char **err);
static bool sqlite_update_cell(DbConnection *conn, const char *table,
                               const char **pk_cols, const DbValue *pk_vals,
                               size_t num_pk_cols, const char *col,
                               const DbValue *new_val, char **err);
static bool sqlite_delete_row(DbConnection *conn, const char *table,
                              const char **pk_cols, const DbValue *pk_vals,
                              size_t num_pk_cols, char **err);
static bool sqlite_insert_row(DbConnection *conn, const char *table,
                              const ColumnDef *cols, const DbValue *vals,
                              size_t num_cols, char **err);
static void sqlite_free_result(ResultSet *rs);
static void sqlite_free_schema(TableSchema *schema);
static void sqlite_free_string_list(char **list, size_t count);
static void *sqlite_prepare_cancel(DbConnection *conn);
static bool sqlite_cancel_query(DbConnection *conn, void *cancel_handle,
                                char **err);
static void sqlite_free_cancel_handle(void *cancel_handle);
static int64_t sqlite_estimate_row_count(DbConnection *conn, const char *table,
                                         char **err);

/* Driver definition */
DbDriver sqlite_driver = {
    .name = "sqlite",
    .display_name = "SQLite",
    .connect = sqlite_connect,
    .disconnect = sqlite_disconnect,
    .ping = sqlite_ping,
    .status = sqlite_status,
    .get_error = sqlite_get_error,
    .list_databases = NULL,
    .list_tables = sqlite_list_tables,
    .get_table_schema = sqlite_get_table_schema,
    .query = sqlite_query,
    .exec = sqlite_exec,
    .query_page = sqlite_query_page,
    .update_cell = sqlite_update_cell,
    .insert_row = sqlite_insert_row,
    .delete_row = sqlite_delete_row,
    .begin_transaction = NULL,
    .commit = NULL,
    .rollback = NULL,
    .free_result = sqlite_free_result,
    .free_schema = sqlite_free_schema,
    .free_string_list = sqlite_free_string_list,
    .prepare_cancel = sqlite_prepare_cancel,
    .cancel_query = sqlite_cancel_query,
    .free_cancel_handle = sqlite_free_cancel_handle,
    .estimate_row_count = sqlite_estimate_row_count,
    .library_cleanup = NULL,
};

/* Bind DbValue to SQLite statement parameter */
static void sqlite_bind_value(sqlite3_stmt *stmt, int param_idx,
                              const DbValue *val) {
  if (val->is_null) {
    sqlite3_bind_null(stmt, param_idx);
    return;
  }

  switch (val->type) {
  case DB_TYPE_INT:
    sqlite3_bind_int64(stmt, param_idx, val->int_val);
    break;
  case DB_TYPE_FLOAT:
    sqlite3_bind_double(stmt, param_idx, val->float_val);
    break;
  case DB_TYPE_TEXT:
    if (val->text.len <= (size_t)INT_MAX) {
      sqlite3_bind_text(stmt, param_idx, val->text.data, (int)val->text.len,
                        SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(stmt, param_idx);
    }
    break;
  case DB_TYPE_BLOB:
    if (val->blob.len <= (size_t)INT_MAX) {
      sqlite3_bind_blob(stmt, param_idx, val->blob.data, (int)val->blob.len,
                        SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(stmt, param_idx);
    }
    break;
  case DB_TYPE_BOOL:
    sqlite3_bind_int(stmt, param_idx, val->bool_val ? 1 : 0);
    break;
  default:
    sqlite3_bind_null(stmt, param_idx);
    break;
  }
}

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
      if (len > MAX_FIELD_SIZE) {
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
      val.type = DB_TYPE_NULL;
      val.is_null = true;
    } else {
      size_t len = (size_t)raw_len;
      if (len > MAX_FIELD_SIZE) {
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

  /* Open the database */
  sqlite3 *db = NULL;
  int rc = sqlite3_open(cs->database, &db);
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
  conn->status = CONN_STATUS_CONNECTED;
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

static bool sqlite_ping(DbConnection *conn) {
  if (!conn || !conn->driver_data)
    return false;
  SqliteData *data = conn->driver_data;
  return data->db != NULL;
}

static ConnStatus sqlite_status(DbConnection *conn) {
  if (!conn)
    return CONN_STATUS_DISCONNECTED;
  return conn->status;
}

static const char *sqlite_get_error(DbConnection *conn) {
  if (!conn)
    return NULL;
  SqliteData *data = conn->driver_data;
  if (data && data->db) {
    return sqlite3_errmsg(data->db);
  }
  return conn->last_error;
}

static char **sqlite_list_tables(DbConnection *conn, size_t *count,
                                 char **err) {
  DB_REQUIRE_PARAMS_CONN(count, conn, SqliteData, data, db, err, NULL);
  *count = 0;

  const char *sql = "SELECT name FROM sqlite_master WHERE type='table' "
                    "AND name NOT LIKE 'sqlite_%' ORDER BY name";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(data->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    if (err)
      *err = str_printf("Query failed: %s", sqlite3_errmsg(data->db));
    return NULL;
  }

  /* First pass: count tables */
  size_t num_tables = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    num_tables++;
  }

  if (num_tables == 0) {
    sqlite3_finalize(stmt);
    /* Return empty array (not NULL) to distinguish from error */
    char **tables = safe_calloc(1, sizeof(char *));
    tables[0] = NULL; /* NULL-terminated empty array */
    *count = 0;
    return tables;
  }

  /* Allocate array */
  char **tables = safe_calloc(num_tables, sizeof(char *));

  /* Reset and second pass: collect names */
  sqlite3_reset(stmt);
  size_t i = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && i < num_tables) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    tables[i] = str_dup(name ? name : "");
    i++;
  }

  sqlite3_finalize(stmt);
  *count = i;
  return tables;
}

static TableSchema *sqlite_get_table_schema(DbConnection *conn,
                                            const char *table, char **err) {
  DB_REQUIRE_PARAMS_CONN(table, conn, SqliteData, data, db, err, NULL);

  TableSchema *schema = safe_calloc(1, sizeof(TableSchema));
  schema->name = str_dup(table);

  /* Get column info using PRAGMA table_info */
  /* Use proper identifier escaping for table name */
  char *escaped_table = str_escape_identifier_dquote(table);
  if (!escaped_table) {
    err_set(err, "Memory allocation failed");
    db_schema_free(schema);
    return NULL;
  }
  char *sql = str_printf("PRAGMA table_info(%s)", escaped_table);
  free(escaped_table);
  escaped_table = NULL;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(data->db, sql, -1, &stmt, NULL);
  free(sql);

  if (rc != SQLITE_OK) {
    if (err)
      *err =
          str_printf("Failed to get table info: %s", sqlite3_errmsg(data->db));
    db_schema_free(schema);
    return NULL;
  }

  /* Count columns first */
  size_t num_cols = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    num_cols++;
  }

  if (num_cols == 0) {
    sqlite3_finalize(stmt);
    err_set(err, "Table not found or has no columns");
    db_schema_free(schema);
    return NULL;
  }

  schema->columns = safe_calloc(num_cols, sizeof(ColumnDef));

  /* Reset and collect column info */
  sqlite3_reset(stmt);
  size_t i = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && i < num_cols) {
    ColumnDef *col = &schema->columns[i];

    /* Column: cid, name, type, notnull, dflt_value, pk */
    const char *name_text = (const char *)sqlite3_column_text(stmt, 1);
    col->name = str_dup(name_text ? name_text : "");
    const char *type_text = (const char *)sqlite3_column_text(stmt, 2);
    col->type_name = str_dup(type_text ? type_text : "");
    col->nullable = sqlite3_column_int(stmt, 3) == 0;
    col->primary_key = sqlite3_column_int(stmt, 5) > 0;

    const char *dflt = (const char *)sqlite3_column_text(stmt, 4);
    if (dflt)
      col->default_val = str_dup(dflt);

    /* Determine type from type name */
    const char *type_name = col->type_name;
    if (type_name) {
      if (strstr(type_name, "INT"))
        col->type = DB_TYPE_INT;
      else if (strstr(type_name, "REAL") || strstr(type_name, "FLOAT") ||
               strstr(type_name, "DOUBLE"))
        col->type = DB_TYPE_FLOAT;
      else if (strstr(type_name, "BLOB"))
        col->type = DB_TYPE_BLOB;
      else if (strstr(type_name, "BOOL"))
        col->type = DB_TYPE_BOOL;
      else
        col->type = DB_TYPE_TEXT;
    }

    i++;
  }
  schema->num_columns = i;

  sqlite3_finalize(stmt);

  /* Check for autoincrement (INTEGER PRIMARY KEY is implicitly autoincrement)
   */
  for (size_t j = 0; j < schema->num_columns; j++) {
    if (schema->columns[j].primary_key && schema->columns[j].type_name &&
        strcasestr(schema->columns[j].type_name, "INTEGER")) {
      schema->columns[j].auto_increment = true;
    }
  }

  /* Get index info */
  escaped_table = str_escape_identifier_dquote(table);
  if (escaped_table) {
    sql = str_printf("PRAGMA index_list(%s)", escaped_table);
    free(escaped_table);
  } else {
    sql = NULL;
  }
  if (sql) {
    rc = sqlite3_prepare_v2(data->db, sql, -1, &stmt, NULL);
    free(sql);
  } else {
    rc = SQLITE_ERROR;
  }

  if (rc == SQLITE_OK) {
    /* Count indexes */
    size_t num_indexes = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      num_indexes++;
    }

    if (num_indexes > 0) {
      schema->indexes = safe_calloc(num_indexes, sizeof(IndexDef));
      {
        sqlite3_reset(stmt);
        size_t idx = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW && idx < num_indexes) {
          IndexDef *index = &schema->indexes[idx];
          /* Initialize to NULL/0 for safe cleanup on failure */
          memset(index, 0, sizeof(IndexDef));

          const char *idx_name = (const char *)sqlite3_column_text(stmt, 1);
          index->name = str_dup(idx_name ? idx_name : "");
          index->unique = sqlite3_column_int(stmt, 2) != 0;

          /* Get index columns */
          char *escaped_idx = str_escape_identifier_dquote(index->name);
          char *idx_sql = escaped_idx
                              ? str_printf("PRAGMA index_info(%s)", escaped_idx)
                              : NULL;
          free(escaped_idx);
          sqlite3_stmt *idx_stmt = NULL;
          if (idx_sql && sqlite3_prepare_v2(data->db, idx_sql, -1, &idx_stmt,
                                            NULL) == SQLITE_OK) {
            /* Count columns in index */
            size_t ncols = 0;
            while (sqlite3_step(idx_stmt) == SQLITE_ROW)
              ncols++;

            if (ncols > 0) {
              index->columns = safe_calloc(ncols, sizeof(char *));
              sqlite3_reset(idx_stmt);
              size_t c = 0;
              while (sqlite3_step(idx_stmt) == SQLITE_ROW && c < ncols) {
                const char *col_name =
                    (const char *)sqlite3_column_text(idx_stmt, 2);
                index->columns[c] = str_dup(col_name ? col_name : "");
                c++;
              }
              index->num_columns = c;
            }
            sqlite3_finalize(idx_stmt);
          }
          free(idx_sql);
          idx++;
        }
        schema->num_indexes = idx;
      }
    }
    sqlite3_finalize(stmt);
  }

  /* Get foreign key info */
  escaped_table = str_escape_identifier_dquote(table);
  if (escaped_table) {
    sql = str_printf("PRAGMA foreign_key_list(%s)", escaped_table);
    free(escaped_table);
  } else {
    sql = NULL;
  }
  if (sql) {
    rc = sqlite3_prepare_v2(data->db, sql, -1, &stmt, NULL);
    free(sql);
  } else {
    rc = SQLITE_ERROR;
  }

  if (rc == SQLITE_OK) {
    /* Count foreign keys */
    size_t num_fks = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      num_fks++;
    }

    if (num_fks > 0) {
      schema->foreign_keys = safe_calloc(num_fks, sizeof(ForeignKeyDef));
      {
        sqlite3_reset(stmt);
        size_t fk_idx = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW && fk_idx < num_fks) {
          ForeignKeyDef *fk = &schema->foreign_keys[fk_idx];
          /* Initialize to zero for safe cleanup on allocation failure */
          memset(fk, 0, sizeof(ForeignKeyDef));

          /* id, seq, table, from, to, on_update, on_delete, match */
          const char *ref_table = (const char *)sqlite3_column_text(stmt, 2);
          fk->ref_table = str_dup(ref_table ? ref_table : "");

          const char *from_col = (const char *)sqlite3_column_text(stmt, 3);
          const char *to_col = (const char *)sqlite3_column_text(stmt, 4);

          fk->columns = safe_calloc(1, sizeof(char *));
          fk->columns[0] = str_dup(from_col ? from_col : "");
          fk->num_columns = 1;

          fk->ref_columns = safe_calloc(1, sizeof(char *));
          fk->ref_columns[0] = str_dup(to_col ? to_col : "");
          fk->num_ref_columns = 1;

          const char *on_update = (const char *)sqlite3_column_text(stmt, 5);
          const char *on_delete = (const char *)sqlite3_column_text(stmt, 6);
          fk->on_update = str_dup(on_update ? on_update : "");
          fk->on_delete = str_dup(on_delete ? on_delete : "");
          /* on_update/on_delete can be NULL on failure - acceptable for display
           */

          fk_idx++;
        }
        schema->num_foreign_keys = fk_idx;
      }
    }
    sqlite3_finalize(stmt);
  }

  return schema;
}

static ResultSet *sqlite_query(DbConnection *conn, const char *sql,
                               char **err) {
  DB_REQUIRE_PARAMS_CONN(sql, conn, SqliteData, data, db, err, NULL);

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(data->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    if (err)
      *err = str_printf("Query failed: %s", sqlite3_errmsg(data->db));
    return NULL;
  }

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
  size_t row_cap = 64;
  rs->rows = safe_calloc(row_cap, sizeof(Row));

  size_t max_rows = conn->max_result_rows > 0 ? conn->max_result_rows
                                              : (size_t)MAX_RESULT_ROWS;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    /* Limit result set size to prevent unbounded memory growth */
    if (rs->num_rows >= max_rows) {
      break;
    }
    if (rs->num_rows >= row_cap) {
      size_t new_cap = row_cap * 2;
      rs->rows = safe_reallocarray(rs->rows, new_cap, sizeof(Row));
      row_cap = new_cap;
    }

    Row *row = &rs->rows[rs->num_rows];
    row->num_cells = num_cols;
    row->cells = safe_calloc(num_cols, sizeof(DbValue));

    for (int i = 0; i < num_cols; i++) {
      row->cells[i] = sqlite_get_value(stmt, i);
    }

    rs->num_rows++;
  }

  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    if (err)
      *err = str_printf("Query execution failed: %s", sqlite3_errmsg(data->db));
    db_result_free(rs);
    return NULL;
  }

  return rs;
}

static int64_t sqlite_exec(DbConnection *conn, const char *sql, char **err) {
  DB_REQUIRE_PARAMS_CONN(sql, conn, SqliteData, data, db, err, -1);

  char *errmsg = NULL;
  int rc = sqlite3_exec(data->db, sql, NULL, NULL, &errmsg);

  if (rc != SQLITE_OK) {
    err_set(err, errmsg ? errmsg : sqlite3_errmsg(data->db));
    sqlite3_free(errmsg);
    return -1;
  }

  return sqlite3_changes(data->db);
}

static ResultSet *sqlite_query_page(DbConnection *conn, const char *table,
                                    size_t offset, size_t limit,
                                    const char *order_by, bool desc,
                                    char **err) {
  DB_REQUIRE(table, err, NULL);

  /* Escape table name */
  char *escaped_table = db_common_escape_table(table, DB_QUOTE_DOUBLE, false);
  if (!escaped_table) {
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  /* Build paginated query using common helper */
  char *sql = db_common_build_query_page_sql(escaped_table, offset, limit,
                                             order_by, desc, DB_QUOTE_DOUBLE, err);
  free(escaped_table);

  if (!sql) {
    return NULL; /* Error already set by helper */
  }

  ResultSet *rs = sqlite_query(conn, sql, err);
  free(sql);
  return rs;
}

static bool sqlite_update_cell(DbConnection *conn, const char *table,
                               const char **pk_cols, const DbValue *pk_vals,
                               size_t num_pk_cols, const char *col,
                               const DbValue *new_val, char **err) {
  DB_REQUIRE_PARAMS_CONN(table && pk_cols && pk_vals && num_pk_cols > 0 && col &&
                             new_val,
                         conn, SqliteData, data, db, err, false);

  /* Escape table name */
  char *escaped_table = db_common_escape_table(table, DB_QUOTE_DOUBLE, false);
  if (!escaped_table) {
    err_set(err, "Memory allocation failed");
    return false;
  }

  /* Build UPDATE statement using common helper */
  char *sql = db_common_build_update_sql(escaped_table, col, pk_cols,
                                         num_pk_cols, DB_QUOTE_DOUBLE, false, err);
  free(escaped_table);

  if (!sql) {
    return false; /* Error already set by helper */
  }

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(data->db, sql, -1, &stmt, NULL);
  free(sql);

  if (rc != SQLITE_OK) {
    if (err)
      *err = str_printf("Failed to prepare statement: %s",
                        sqlite3_errmsg(data->db));
    return false;
  }

  /* Bind new value (parameter 1) */
  sqlite_bind_value(stmt, 1, new_val);

  /* Bind PK values (parameters 2..N+1) */
  for (size_t i = 0; i < num_pk_cols; i++) {
    /* Validate index fits in int before cast */
    if (i + 2 > (size_t)INT_MAX) {
      sqlite3_finalize(stmt);
      err_set(err, "Too many primary key columns");
      return false;
    }
    sqlite_bind_value(stmt, (int)(i + 2), &pk_vals[i]);
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    if (err)
      *err = str_printf("Update failed: %s", sqlite3_errmsg(data->db));
    return false;
  }

  return true;
}

static bool sqlite_delete_row(DbConnection *conn, const char *table,
                              const char **pk_cols, const DbValue *pk_vals,
                              size_t num_pk_cols, char **err) {
  DB_REQUIRE_PARAMS_CONN(table && pk_cols && pk_vals && num_pk_cols > 0, conn,
                         SqliteData, data, db, err, false);

  /* Escape table name */
  char *escaped_table = db_common_escape_table(table, DB_QUOTE_DOUBLE, false);
  if (!escaped_table) {
    err_set(err, "Memory allocation failed");
    return false;
  }

  /* Build DELETE statement using common helper */
  char *sql = db_common_build_delete_sql(escaped_table, pk_cols, num_pk_cols,
                                         DB_QUOTE_DOUBLE, false, err);
  free(escaped_table);

  if (!sql) {
    return false; /* Error already set by helper */
  }

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(data->db, sql, -1, &stmt, NULL);
  free(sql);

  if (rc != SQLITE_OK) {
    if (err)
      *err = str_printf("Failed to prepare statement: %s",
                        sqlite3_errmsg(data->db));
    return false;
  }

  /* Bind PK values (parameters 1..N) */
  for (size_t i = 0; i < num_pk_cols; i++) {
    /* Validate index fits in int before cast */
    if (i + 1 > (size_t)INT_MAX) {
      sqlite3_finalize(stmt);
      err_set(err, "Too many primary key columns");
      return false;
    }
    sqlite_bind_value(stmt, (int)(i + 1), &pk_vals[i]);
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    if (err)
      *err = str_printf("Delete failed: %s", sqlite3_errmsg(data->db));
    return false;
  }

  return true;
}

static bool sqlite_insert_row(DbConnection *conn, const char *table,
                              const ColumnDef *cols, const DbValue *vals,
                              size_t num_cols, char **err) {
  DB_REQUIRE_PARAMS_CONN(table && cols && vals && num_cols > 0, conn,
                         SqliteData, data, db, err, false);

  /* Escape table name */
  char *escaped_table = db_common_escape_table(table, DB_QUOTE_DOUBLE, false);
  if (!escaped_table) {
    err_set(err, "Memory allocation failed");
    return false;
  }

  /* Build INSERT statement using common helper */
  DbInsertLists lists;
  char *sql = db_common_build_insert_sql(escaped_table, cols, vals, num_cols,
                                         DB_QUOTE_DOUBLE, false, &lists, err);
  free(escaped_table);

  if (!sql) {
    return false; /* Error already set by helper */
  }

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(data->db, sql, -1, &stmt, NULL);
  free(sql);

  if (rc != SQLITE_OK) {
    db_common_free_insert_lists(&lists);
    if (err)
      *err = str_printf("Failed to prepare statement: %s",
                        sqlite3_errmsg(data->db));
    return false;
  }

  /* Bind values using column map from common helper */
  for (size_t i = 0; i < lists.num_params; i++) {
    size_t col_idx = lists.col_map[i];

    /* Validate index fits in int before cast (param is 1-indexed) */
    if (i + 1 > (size_t)INT_MAX)
      continue;

    sqlite_bind_value(stmt, (int)(i + 1), &vals[col_idx]);
  }

  db_common_free_insert_lists(&lists);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    if (err)
      *err = str_printf("Insert failed: %s", sqlite3_errmsg(data->db));
    return false;
  }

  return true;
}

static void sqlite_free_result(ResultSet *rs) { db_result_free(rs); }

static void sqlite_free_schema(TableSchema *schema) { db_schema_free(schema); }

static void sqlite_free_string_list(char **list, size_t count) {
  db_common_free_string_list(list, count);
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

/* Approximate row count using sqlite_stat1 (populated by ANALYZE) */
static int64_t sqlite_estimate_row_count(DbConnection *conn, const char *table,
                                         char **err) {
  DB_REQUIRE_PARAMS_CONN(table, conn, SqliteData, data, db, err, -1);

  /* Try sqlite_stat1 first (populated by ANALYZE command) */
  /* First try table-level stats (idx IS NULL), then any index stats */
  const char *sql = "SELECT stat FROM sqlite_stat1 WHERE tbl = ? ORDER BY idx "
                    "IS NULL DESC LIMIT 1";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(data->db, sql, -1, &stmt, NULL);

  if (rc != SQLITE_OK) {
    /* sqlite_stat1 might not exist - return -1 to fall back to COUNT(*) */
    return -1;
  }

  sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *stat = (const char *)sqlite3_column_text(stmt, 0);
    if (stat && *stat) {
      /* First number in stat string is the row count estimate */
      errno = 0;
      char *endptr;
      int64_t count = strtoll(stat, &endptr, 10);
      sqlite3_finalize(stmt);
      if (errno == 0 && endptr != stat && count >= 0) {
        return count;
      }
      return -1;
    }
  }

  sqlite3_finalize(stmt);

  /* sqlite_stat1 not available or no data - return -1 to indicate fallback
   * needed */
  return -1;
}
