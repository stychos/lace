/*
 * lace - Database Viewer and Manager
 * SQLite driver - uses libsqlite3 C API
 */

#include "../../util/str.h"
#include "../connstr.h"
#include "../db.h"
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
static void sqlite_free_result(ResultSet *rs);
static void sqlite_free_schema(TableSchema *schema);
static void sqlite_free_string_list(char **list, size_t count);

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
    .insert_row = NULL,
    .delete_row = sqlite_delete_row,
    .begin_transaction = NULL,
    .commit = NULL,
    .rollback = NULL,
    .free_result = sqlite_free_result,
    .free_schema = sqlite_free_schema,
    .free_string_list = sqlite_free_string_list,
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
    int len = sqlite3_column_bytes(stmt, col);
    if (!text || len < 0) {
      val.type = DB_TYPE_NULL;
      val.is_null = true;
    } else if ((size_t)len > MAX_FIELD_SIZE) {
      /* Oversized field - show placeholder */
      char placeholder[64];
      snprintf(placeholder, sizeof(placeholder), "[TEXT: %d bytes]", len);
      val.text.data = str_dup(placeholder);
      if (val.text.data) {
        val.type = DB_TYPE_TEXT;
        val.text.len = strlen(val.text.data);
      } else {
        val.type = DB_TYPE_NULL;
        val.is_null = true;
      }
    } else {
      val.text.data = malloc(len + 1);
      if (val.text.data) {
        memcpy(val.text.data, text, len);
        val.text.data[len] = '\0';
        val.text.len = len;
        val.type = DB_TYPE_TEXT;
      } else {
        val.type = DB_TYPE_NULL;
        val.is_null = true;
      }
    }
    break;
  }

  case SQLITE_BLOB: {
    const void *blob = sqlite3_column_blob(stmt, col);
    int len = sqlite3_column_bytes(stmt, col);
    if (!blob || len <= 0) {
      val.type = DB_TYPE_NULL;
      val.is_null = true;
    } else if ((size_t)len > MAX_FIELD_SIZE) {
      /* Oversized field - show placeholder */
      char placeholder[64];
      snprintf(placeholder, sizeof(placeholder), "[BLOB: %d bytes]", len);
      val.text.data = str_dup(placeholder);
      if (val.text.data) {
        val.type = DB_TYPE_TEXT;
        val.text.len = strlen(val.text.data);
      } else {
        val.type = DB_TYPE_NULL;
        val.is_null = true;
      }
    } else {
      val.blob.data = malloc(len);
      if (val.blob.data) {
        memcpy(val.blob.data, blob, len);
        val.blob.len = len;
        val.type = DB_TYPE_BLOB;
      } else {
        val.type = DB_TYPE_NULL;
        val.is_null = true;
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
    if (err)
      *err = str_dup("Not a SQLite connection string");
    return NULL;
  }

  if (!cs->database || !cs->database[0]) {
    connstr_free(cs);
    if (err)
      *err = str_dup("No database path specified");
    return NULL;
  }

  /* Open the database */
  sqlite3 *db = NULL;
  int rc = sqlite3_open(cs->database, &db);
  if (rc != SQLITE_OK) {
    if (err)
      *err = str_printf("Failed to open database: %s",
                        db ? sqlite3_errmsg(db) : "unknown error");
    if (db)
      sqlite3_close(db);
    connstr_free(cs);
    return NULL;
  }

  /* Enable foreign keys */
  sqlite3_exec(db, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

  SqliteData *data = calloc(1, sizeof(SqliteData));
  if (!data) {
    sqlite3_close(db);
    connstr_free(cs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  data->db = db;
  data->path = str_dup(cs->database);
  if (!data->path) {
    sqlite3_close(db);
    free(data);
    connstr_free(cs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  DbConnection *conn = calloc(1, sizeof(DbConnection));
  if (!conn) {
    sqlite3_close(db);
    free(data->path);
    free(data);
    connstr_free(cs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  conn->driver = &sqlite_driver;
  conn->connstr = str_dup(connstr);
  conn->database = str_dup(cs->database);
  if (!conn->connstr || !conn->database) {
    sqlite3_close(db);
    free(data->path);
    free(data);
    free(conn->connstr);
    free(conn->database);
    free(conn);
    connstr_free(cs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }
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

  free(conn->connstr);
  free(conn->database);
  free(conn->host);
  free(conn->user);
  free(conn->last_error);
  free(conn);
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
  if (!conn || !count) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  *count = 0;
  SqliteData *data = conn->driver_data;

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
    char **tables = calloc(1, sizeof(char *));
    if (!tables) {
      if (err)
        *err = str_dup("Memory allocation failed");
      return NULL;
    }
    tables[0] = NULL; /* NULL-terminated empty array */
    *count = 0;
    return tables;
  }

  /* Allocate array */
  char **tables = calloc(num_tables, sizeof(char *));
  if (!tables) {
    sqlite3_finalize(stmt);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  /* Reset and second pass: collect names */
  sqlite3_reset(stmt);
  size_t i = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && i < num_tables) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    tables[i] = str_dup(name ? name : "");
    if (!tables[i]) {
      /* Cleanup on allocation failure */
      for (size_t j = 0; j < i; j++) {
        free(tables[j]);
      }
      free(tables);
      sqlite3_finalize(stmt);
      if (err)
        *err = str_dup("Memory allocation failed");
      return NULL;
    }
    i++;
  }

  sqlite3_finalize(stmt);
  *count = i;
  return tables;
}

static TableSchema *sqlite_get_table_schema(DbConnection *conn,
                                            const char *table, char **err) {
  if (!conn || !table) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  SqliteData *data = conn->driver_data;

  TableSchema *schema = calloc(1, sizeof(TableSchema));
  if (!schema) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  schema->name = str_dup(table);
  if (!schema->name) {
    if (err)
      *err = str_dup("Memory allocation failed");
    free(schema);
    return NULL;
  }

  /* Get column info using PRAGMA table_info */
  /* Use proper identifier escaping for table name */
  char *escaped_table = str_escape_identifier_dquote(table);
  if (!escaped_table) {
    if (err)
      *err = str_dup("Memory allocation failed");
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
    if (err)
      *err = str_dup("Table not found or has no columns");
    db_schema_free(schema);
    return NULL;
  }

  schema->columns = calloc(num_cols, sizeof(ColumnDef));
  if (!schema->columns) {
    sqlite3_finalize(stmt);
    if (err)
      *err = str_dup("Memory allocation failed");
    db_schema_free(schema);
    return NULL;
  }

  /* Reset and collect column info */
  sqlite3_reset(stmt);
  size_t i = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && i < num_cols) {
    ColumnDef *col = &schema->columns[i];

    /* Column: cid, name, type, notnull, dflt_value, pk */
    col->name = str_dup((const char *)sqlite3_column_text(stmt, 1));
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
        strstr(schema->columns[j].type_name, "INTEGER")) {
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
      schema->indexes = calloc(num_indexes, sizeof(IndexDef));
      if (schema->indexes) {
        sqlite3_reset(stmt);
        size_t idx = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW && idx < num_indexes) {
          IndexDef *index = &schema->indexes[idx];
          /* Initialize to NULL/0 for safe cleanup on failure */
          memset(index, 0, sizeof(IndexDef));

          const char *idx_name = (const char *)sqlite3_column_text(stmt, 1);
          index->name = str_dup(idx_name ? idx_name : "");
          if (!index->name) {
            /* Allocation failed - skip this index */
            continue;
          }
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
              index->columns = calloc(ncols, sizeof(char *));
              if (index->columns) {
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
      schema->foreign_keys = calloc(num_fks, sizeof(ForeignKeyDef));
      if (schema->foreign_keys) {
        sqlite3_reset(stmt);
        size_t fk_idx = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW && fk_idx < num_fks) {
          ForeignKeyDef *fk = &schema->foreign_keys[fk_idx];
          /* Initialize to zero for safe cleanup on allocation failure */
          memset(fk, 0, sizeof(ForeignKeyDef));

          /* id, seq, table, from, to, on_update, on_delete, match */
          const char *ref_table = (const char *)sqlite3_column_text(stmt, 2);
          fk->ref_table = str_dup(ref_table ? ref_table : "");
          if (!fk->ref_table) {
            /* Allocation failed - skip this FK */
            continue;
          }

          const char *from_col = (const char *)sqlite3_column_text(stmt, 3);
          const char *to_col = (const char *)sqlite3_column_text(stmt, 4);

          fk->columns = calloc(1, sizeof(char *));
          if (fk->columns) {
            fk->columns[0] = str_dup(from_col ? from_col : "");
            if (fk->columns[0]) {
              fk->num_columns = 1;
            } else {
              free(fk->columns);
              fk->columns = NULL;
            }
          }

          fk->ref_columns = calloc(1, sizeof(char *));
          if (fk->ref_columns) {
            fk->ref_columns[0] = str_dup(to_col ? to_col : "");
            if (fk->ref_columns[0]) {
              fk->num_ref_columns = 1;
            } else {
              free(fk->ref_columns);
              fk->ref_columns = NULL;
            }
          }

          const char *on_update = (const char *)sqlite3_column_text(stmt, 5);
          const char *on_delete = (const char *)sqlite3_column_text(stmt, 6);
          fk->on_update = str_dup(on_update ? on_update : "");
          fk->on_delete = str_dup(on_delete ? on_delete : "");
          /* on_update/on_delete can be NULL on failure - acceptable for display */

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
  if (!conn || !sql) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  SqliteData *data = conn->driver_data;

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(data->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    if (err)
      *err = str_printf("Query failed: %s", sqlite3_errmsg(data->db));
    return NULL;
  }

  ResultSet *rs = calloc(1, sizeof(ResultSet));
  if (!rs) {
    sqlite3_finalize(stmt);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  /* Get column info */
  int num_cols = sqlite3_column_count(stmt);
  if (num_cols > 0) {
    rs->columns = calloc(num_cols, sizeof(ColumnDef));
    if (rs->columns) {
      rs->num_columns = num_cols;
      for (int i = 0; i < num_cols; i++) {
        rs->columns[i].name = str_dup(sqlite3_column_name(stmt, i));
        const char *type = sqlite3_column_decltype(stmt, i);
        if (type)
          rs->columns[i].type_name = str_dup(type);
      }
    }
  }

  /* Collect rows */
  size_t row_cap = 64;
  rs->rows = calloc(row_cap, sizeof(Row));
  if (!rs->rows) {
    db_result_free(rs);
    sqlite3_finalize(stmt);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  bool oom = false;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (rs->num_rows >= row_cap) {
      /* Check for overflow BEFORE doubling */
      if (row_cap > SIZE_MAX / 2 || row_cap > SIZE_MAX / sizeof(Row) / 2) {
        oom = true;
        break;
      }
      size_t new_cap = row_cap * 2;
      Row *new_rows = realloc(rs->rows, new_cap * sizeof(Row));
      if (!new_rows) {
        oom = true;
        break;
      }
      rs->rows = new_rows;
      row_cap = new_cap;
    }

    Row *row = &rs->rows[rs->num_rows];
    row->num_cells = num_cols;
    row->cells = calloc(num_cols, sizeof(DbValue));
    if (!row->cells) {
      oom = true;
      break;
    }

    for (int i = 0; i < num_cols; i++) {
      row->cells[i] = sqlite_get_value(stmt, i);
    }

    rs->num_rows++;
  }

  sqlite3_finalize(stmt);

  if (oom) {
    if (err)
      *err = str_dup("Out of memory while fetching results");
    db_result_free(rs);
    return NULL;
  }

  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    if (err)
      *err = str_printf("Query execution failed: %s", sqlite3_errmsg(data->db));
    db_result_free(rs);
    return NULL;
  }

  return rs;
}

static int64_t sqlite_exec(DbConnection *conn, const char *sql, char **err) {
  if (!conn || !sql) {
    if (err)
      *err = str_dup("Invalid parameters");
    return -1;
  }

  SqliteData *data = conn->driver_data;

  char *errmsg = NULL;
  int rc = sqlite3_exec(data->db, sql, NULL, NULL, &errmsg);

  if (rc != SQLITE_OK) {
    if (err)
      *err = str_dup(errmsg ? errmsg : sqlite3_errmsg(data->db));
    sqlite3_free(errmsg);
    return -1;
  }

  return sqlite3_changes(data->db);
}

static ResultSet *sqlite_query_page(DbConnection *conn, const char *table,
                                    size_t offset, size_t limit,
                                    const char *order_by, bool desc,
                                    char **err) {
  if (!conn || !table) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  /* Escape identifiers to prevent SQL injection */
  char *escaped_table = str_escape_identifier_dquote(table);
  if (!escaped_table) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  char *sql;
  if (order_by) {
    char *escaped_order = str_escape_identifier_dquote(order_by);
    if (!escaped_order) {
      free(escaped_table);
      if (err)
        *err = str_dup("Memory allocation failed");
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
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  ResultSet *rs = sqlite_query(conn, sql, err);
  free(sql);
  return rs;
}

static bool sqlite_update_cell(DbConnection *conn, const char *table,
                               const char **pk_cols, const DbValue *pk_vals,
                               size_t num_pk_cols, const char *col,
                               const DbValue *new_val, char **err) {
  if (!conn || !table || !pk_cols || !pk_vals || num_pk_cols == 0 || !col ||
      !new_val) {
    if (err)
      *err = str_dup("Invalid parameters");
    return false;
  }

  SqliteData *data = conn->driver_data;

  /* Escape identifiers to prevent SQL injection */
  char *escaped_table = str_escape_identifier_dquote(table);
  char *escaped_col = str_escape_identifier_dquote(col);
  if (!escaped_table || !escaped_col) {
    free(escaped_table);
    free(escaped_col);
    if (err)
      *err = str_dup("Memory allocation failed");
    return false;
  }

  /* Build WHERE clause for composite primary key */
  char *where_clause = str_dup("");
  for (size_t i = 0; i < num_pk_cols; i++) {
    char *escaped_pk = str_escape_identifier_dquote(pk_cols[i]);
    if (!escaped_pk || !where_clause) {
      free(escaped_pk);
      free(where_clause);
      free(escaped_table);
      free(escaped_col);
      if (err)
        *err = str_dup("Memory allocation failed");
      return false;
    }
    char *new_where;
    if (i == 0) {
      new_where = str_printf("%s = ?", escaped_pk);
    } else {
      new_where = str_printf("%s AND %s = ?", where_clause, escaped_pk);
    }
    free(escaped_pk);
    free(where_clause);
    where_clause = new_where;
    if (!where_clause) {
      free(escaped_table);
      free(escaped_col);
      if (err)
        *err = str_dup("Memory allocation failed");
      return false;
    }
  }

  /* Build parameterized UPDATE statement */
  char *sql = str_printf("UPDATE %s SET %s = ? WHERE %s", escaped_table,
                         escaped_col, where_clause);
  free(escaped_table);
  free(escaped_col);
  free(where_clause);

  if (!sql) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return false;
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
  if (new_val->is_null) {
    sqlite3_bind_null(stmt, 1);
  } else {
    switch (new_val->type) {
    case DB_TYPE_INT:
      sqlite3_bind_int64(stmt, 1, new_val->int_val);
      break;
    case DB_TYPE_FLOAT:
      sqlite3_bind_double(stmt, 1, new_val->float_val);
      break;
    case DB_TYPE_TEXT:
      sqlite3_bind_text(stmt, 1, new_val->text.data, new_val->text.len,
                        SQLITE_TRANSIENT);
      break;
    case DB_TYPE_BLOB:
      sqlite3_bind_blob(stmt, 1, new_val->blob.data, new_val->blob.len,
                        SQLITE_TRANSIENT);
      break;
    default:
      sqlite3_bind_null(stmt, 1);
      break;
    }
  }

  /* Bind PK values (parameters 2..N+1) */
  for (size_t i = 0; i < num_pk_cols; i++) {
    const DbValue *pk_val = &pk_vals[i];
    int param_idx = (int)(i + 2);

    if (pk_val->is_null) {
      sqlite3_bind_null(stmt, param_idx);
    } else {
      switch (pk_val->type) {
      case DB_TYPE_INT:
        sqlite3_bind_int64(stmt, param_idx, pk_val->int_val);
        break;
      case DB_TYPE_FLOAT:
        sqlite3_bind_double(stmt, param_idx, pk_val->float_val);
        break;
      case DB_TYPE_TEXT:
        sqlite3_bind_text(stmt, param_idx, pk_val->text.data, pk_val->text.len,
                          SQLITE_TRANSIENT);
        break;
      default:
        sqlite3_bind_null(stmt, param_idx);
        break;
      }
    }
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
  if (!conn || !table || !pk_cols || !pk_vals || num_pk_cols == 0) {
    if (err)
      *err = str_dup("Invalid parameters");
    return false;
  }

  SqliteData *data = conn->driver_data;

  /* Escape identifiers to prevent SQL injection */
  char *escaped_table = str_escape_identifier_dquote(table);
  if (!escaped_table) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return false;
  }

  /* Build WHERE clause for composite primary key */
  char *where_clause = str_dup("");
  for (size_t i = 0; i < num_pk_cols; i++) {
    char *escaped_pk = str_escape_identifier_dquote(pk_cols[i]);
    if (!escaped_pk || !where_clause) {
      free(escaped_pk);
      free(where_clause);
      free(escaped_table);
      if (err)
        *err = str_dup("Memory allocation failed");
      return false;
    }
    char *new_where;
    if (i == 0) {
      new_where = str_printf("%s = ?", escaped_pk);
    } else {
      new_where = str_printf("%s AND %s = ?", where_clause, escaped_pk);
    }
    free(escaped_pk);
    free(where_clause);
    where_clause = new_where;
    if (!where_clause) {
      free(escaped_table);
      if (err)
        *err = str_dup("Memory allocation failed");
      return false;
    }
  }

  /* Build parameterized DELETE statement */
  char *sql =
      str_printf("DELETE FROM %s WHERE %s", escaped_table, where_clause);
  free(escaped_table);
  free(where_clause);

  if (!sql) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return false;
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
    const DbValue *pk_val = &pk_vals[i];
    int param_idx = (int)(i + 1);

    if (pk_val->is_null) {
      sqlite3_bind_null(stmt, param_idx);
    } else {
      switch (pk_val->type) {
      case DB_TYPE_INT:
        sqlite3_bind_int64(stmt, param_idx, pk_val->int_val);
        break;
      case DB_TYPE_FLOAT:
        sqlite3_bind_double(stmt, param_idx, pk_val->float_val);
        break;
      case DB_TYPE_TEXT:
        sqlite3_bind_text(stmt, param_idx, pk_val->text.data, pk_val->text.len,
                          SQLITE_TRANSIENT);
        break;
      default:
        sqlite3_bind_null(stmt, param_idx);
        break;
      }
    }
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

static void sqlite_free_result(ResultSet *rs) { db_result_free(rs); }

static void sqlite_free_schema(TableSchema *schema) { db_schema_free(schema); }

static void sqlite_free_string_list(char **list, size_t count) {
  if (!list)
    return;
  for (size_t i = 0; i < count; i++) {
    free(list[i]);
  }
  free(list);
}
