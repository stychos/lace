/*
 * Lace
 * MySQL/MariaDB driver - uses libmariadb C API
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../util/str.h"
#include "../connstr.h"
#include "../db.h"
#include <ctype.h>
#include <errno.h>
#include <mysql/mysql.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum iterations for consuming pending results (prevents infinite loops) */
#define MAX_RESULT_CONSUME_ITERATIONS 1000

/* MySQL connection data */
typedef struct {
  MYSQL *mysql;
  char *database;
  bool is_mariadb; /* Connection scheme was mariadb:// */
} MySqlData;

/* Forward declarations */
static DbConnection *mysql_driver_connect(const char *connstr, char **err);
static void mysql_driver_disconnect(DbConnection *conn);
static bool mysql_driver_ping(DbConnection *conn);
static ConnStatus mysql_driver_status(DbConnection *conn);
static const char *mysql_driver_get_error(DbConnection *conn);
static char **mysql_driver_list_tables(DbConnection *conn, size_t *count,
                                       char **err);
static TableSchema *mysql_driver_get_table_schema(DbConnection *conn,
                                                  const char *table,
                                                  char **err);
static ResultSet *mysql_driver_query(DbConnection *conn, const char *sql,
                                     char **err);
static int64_t mysql_driver_exec(DbConnection *conn, const char *sql,
                                 char **err);
static ResultSet *mysql_driver_query_page(DbConnection *conn, const char *table,
                                          size_t offset, size_t limit,
                                          const char *order_by, bool desc,
                                          char **err);
static bool mysql_driver_update_cell(DbConnection *conn, const char *table,
                                     const char **pk_cols,
                                     const DbValue *pk_vals, size_t num_pk_cols,
                                     const char *col, const DbValue *new_val,
                                     char **err);
static bool mysql_driver_delete_row(DbConnection *conn, const char *table,
                                    const char **pk_cols,
                                    const DbValue *pk_vals, size_t num_pk_cols,
                                    char **err);
static void mysql_driver_free_result(ResultSet *rs);
static void mysql_driver_free_schema(TableSchema *schema);
static void mysql_driver_free_string_list(char **list, size_t count);
static void mysql_driver_library_cleanup(void);
static void *mysql_driver_prepare_cancel(DbConnection *conn);
static bool mysql_driver_cancel_query(DbConnection *conn, void *cancel_handle,
                                      char **err);
static void mysql_driver_free_cancel_handle(void *cancel_handle);
static int64_t mysql_driver_estimate_row_count(DbConnection *conn,
                                               const char *table, char **err);

/* Driver definitions - both mysql and mariadb use the same implementation */
DbDriver mysql_driver = {
    .name = "mysql",
    .display_name = "MySQL",
    .connect = mysql_driver_connect,
    .disconnect = mysql_driver_disconnect,
    .ping = mysql_driver_ping,
    .status = mysql_driver_status,
    .get_error = mysql_driver_get_error,
    .list_databases = NULL,
    .list_tables = mysql_driver_list_tables,
    .get_table_schema = mysql_driver_get_table_schema,
    .query = mysql_driver_query,
    .exec = mysql_driver_exec,
    .query_page = mysql_driver_query_page,
    .update_cell = mysql_driver_update_cell,
    .insert_row = NULL,
    .delete_row = mysql_driver_delete_row,
    .begin_transaction = NULL,
    .commit = NULL,
    .rollback = NULL,
    .free_result = mysql_driver_free_result,
    .free_schema = mysql_driver_free_schema,
    .free_string_list = mysql_driver_free_string_list,
    .prepare_cancel = mysql_driver_prepare_cancel,
    .cancel_query = mysql_driver_cancel_query,
    .free_cancel_handle = mysql_driver_free_cancel_handle,
    .estimate_row_count = mysql_driver_estimate_row_count,
    .library_cleanup = mysql_driver_library_cleanup,
};

DbDriver mariadb_driver = {
    .name = "mariadb",
    .display_name = "MariaDB",
    .connect = mysql_driver_connect,
    .disconnect = mysql_driver_disconnect,
    .ping = mysql_driver_ping,
    .status = mysql_driver_status,
    .get_error = mysql_driver_get_error,
    .list_databases = NULL,
    .list_tables = mysql_driver_list_tables,
    .get_table_schema = mysql_driver_get_table_schema,
    .query = mysql_driver_query,
    .exec = mysql_driver_exec,
    .query_page = mysql_driver_query_page,
    .update_cell = mysql_driver_update_cell,
    .insert_row = NULL,
    .delete_row = mysql_driver_delete_row,
    .begin_transaction = NULL,
    .commit = NULL,
    .rollback = NULL,
    .free_result = mysql_driver_free_result,
    .free_schema = mysql_driver_free_schema,
    .free_string_list = mysql_driver_free_string_list,
    .prepare_cancel = mysql_driver_prepare_cancel,
    .cancel_query = mysql_driver_cancel_query,
    .free_cancel_handle = mysql_driver_free_cancel_handle,
    .estimate_row_count = mysql_driver_estimate_row_count,
    .library_cleanup = mysql_driver_library_cleanup,
};

/* Map MySQL field type to DbValueType */
static DbValueType mysql_type_to_db_type(enum enum_field_types type) {
  switch (type) {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_YEAR:
    return DB_TYPE_INT;

  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
    return DB_TYPE_FLOAT;

  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
    return DB_TYPE_BLOB;

  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
    return DB_TYPE_TIMESTAMP;

  default:
    return DB_TYPE_TEXT;
  }
}

/* Get DbValue from result row */
static DbValue mysql_get_value(MYSQL_ROW row, unsigned long *lengths, int col,
                               MYSQL_FIELD *field) {
  DbValue val = {0};

  if (row[col] == NULL) {
    val.type = DB_TYPE_NULL;
    val.is_null = true;
    return val;
  }

  /* Validate length doesn't exceed SIZE_MAX (for +1 overflow safety) */
  if (lengths[col] > SIZE_MAX - 1) {
    val.type = DB_TYPE_NULL;
    val.is_null = true;
    return val;
  }

  /* For oversized fields, show placeholder text instead of loading data */
  if (lengths[col] > MAX_FIELD_SIZE) {
    char placeholder[64];
    snprintf(placeholder, sizeof(placeholder), "[DATA: %lu bytes]",
             (unsigned long)lengths[col]);
    val.text.data = str_dup(placeholder);
    if (val.text.data) {
      val.type = DB_TYPE_TEXT;
      val.text.len = strlen(val.text.data);
    } else {
      val.type = DB_TYPE_NULL;
      val.is_null = true;
    }
    return val;
  }

  val.is_null = false;
  DbValueType type = mysql_type_to_db_type(field->type);

  switch (type) {
  case DB_TYPE_INT: {
    char *endptr;
    errno = 0;
    long long parsed = strtoll(row[col], &endptr, 10);
    if (errno == 0 && endptr != row[col] && *endptr == '\0') {
      val.type = DB_TYPE_INT;
      val.int_val = parsed;
    } else {
      /* Conversion failed - store as text instead */
      val.text.data = malloc((size_t)lengths[col] + 1);
      if (val.text.data) {
        val.type = DB_TYPE_TEXT;
        val.text.len = (size_t)lengths[col];
        memcpy(val.text.data, row[col], (size_t)lengths[col]);
        val.text.data[(size_t)lengths[col]] = '\0';
      } else {
        val.type = DB_TYPE_NULL;
        val.is_null = true;
      }
    }
    break;
  }

  case DB_TYPE_FLOAT: {
    char *endptr;
    errno = 0;
    double parsed = strtod(row[col], &endptr);
    if (errno == 0 && endptr != row[col] && *endptr == '\0') {
      val.type = DB_TYPE_FLOAT;
      val.float_val = parsed;
    } else {
      /* Conversion failed - store as text instead */
      val.text.data = malloc((size_t)lengths[col] + 1);
      if (val.text.data) {
        val.type = DB_TYPE_TEXT;
        val.text.len = (size_t)lengths[col];
        memcpy(val.text.data, row[col], (size_t)lengths[col]);
        val.text.data[(size_t)lengths[col]] = '\0';
      } else {
        val.type = DB_TYPE_NULL;
        val.is_null = true;
      }
    }
    break;
  }

  case DB_TYPE_BLOB:
    val.blob.data = malloc((size_t)lengths[col]);
    if (val.blob.data) {
      val.type = DB_TYPE_BLOB;
      val.blob.len = (size_t)lengths[col];
      memcpy(val.blob.data, row[col], (size_t)lengths[col]);
    } else {
      /* Malloc failed - return null value */
      val.type = DB_TYPE_NULL;
      val.is_null = true;
      val.blob.len = 0;
    }
    break;

  default:
    val.text.data = malloc((size_t)lengths[col] + 1);
    if (val.text.data) {
      val.type = DB_TYPE_TEXT;
      val.text.len = (size_t)lengths[col];
      memcpy(val.text.data, row[col], (size_t)lengths[col]);
      val.text.data[(size_t)lengths[col]] = '\0';
    } else {
      /* Malloc failed - return null value */
      val.type = DB_TYPE_NULL;
      val.is_null = true;
      val.text.len = 0;
    }
    break;
  }

  return val;
}

static DbConnection *mysql_driver_connect(const char *connstr, char **err) {
  ConnString *cs = connstr_parse(connstr, err);
  if (!cs) {
    return NULL;
  }

  /* Accept both mysql:// and mariadb:// */
  bool is_mariadb = str_eq(cs->driver, "mariadb");
  if (!str_eq(cs->driver, "mysql") && !is_mariadb) {
    connstr_free(cs);
    if (err)
      *err = str_dup("Not a MySQL/MariaDB connection string");
    return NULL;
  }

  /* Initialize MySQL library (safe to call multiple times) */
  mysql_library_init(0, NULL, NULL);

  MYSQL *mysql = mysql_init(NULL);
  if (!mysql) {
    connstr_free(cs);
    if (err)
      *err = str_dup("Failed to initialize MySQL connection");
    return NULL;
  }

  /* Set connection timeout */
  unsigned int timeout = 10;
  if (mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0) {
    mysql_close(mysql);
    connstr_free(cs);
    if (err)
      *err = str_dup("Failed to set connection timeout");
    return NULL;
  }

  /* Enable automatic reconnection */
  bool reconnect = true;
  if (mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect) != 0) {
    mysql_close(mysql);
    connstr_free(cs);
    if (err)
      *err = str_dup("Failed to set reconnect option");
    return NULL;
  }

  /* Set character set */
  if (mysql_options(mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
    mysql_close(mysql);
    connstr_free(cs);
    if (err)
      *err = str_dup("Failed to set character set");
    return NULL;
  }

  const char *host = cs->host ? cs->host : "localhost";
  int port = cs->port > 0 ? cs->port : 3306;
  const char *user = cs->user ? cs->user : "root";
  const char *password = cs->password;
  const char *database = cs->database ? cs->database : "mysql";

  MYSQL *result =
      mysql_real_connect(mysql, host, user, password, database, port, NULL, 0);
  if (!result) {
    if (err)
      *err = str_printf("Connection failed: %s", mysql_error(mysql));
    mysql_close(mysql);
    connstr_free(cs);
    return NULL;
  }

  MySqlData *data = calloc(1, sizeof(MySqlData));
  if (!data) {
    mysql_close(mysql);
    connstr_free(cs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  data->mysql = mysql;
  data->database = str_dup(database);
  data->is_mariadb = is_mariadb;
  if (!data->database) {
    mysql_close(mysql);
    free(data);
    connstr_free(cs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  DbConnection *conn = calloc(1, sizeof(DbConnection));
  if (!conn) {
    mysql_close(mysql);
    free(data->database);
    free(data);
    connstr_free(cs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  conn->driver = is_mariadb ? &mariadb_driver : &mysql_driver;
  conn->connstr = str_dup(connstr);
  conn->database = str_dup(database);
  conn->host = str_dup(host);
  conn->port = port;
  conn->user = str_dup(user);

  /* Check all allocations succeeded */
  if (!conn->connstr || !conn->database || !conn->host || !conn->user) {
    mysql_close(mysql);
    free(data->database);
    free(data);
    str_secure_free(conn->connstr);
    free(conn->database);
    free(conn->host);
    free(conn->user);
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

static void mysql_driver_disconnect(DbConnection *conn) {
  if (!conn)
    return;

  MySqlData *data = conn->driver_data;
  if (data) {
    if (data->mysql) {
      mysql_close(data->mysql);
    }
    free(data->database);
    free(data);
  }

  str_secure_free(conn->connstr);
  free(conn->database);
  free(conn->host);
  free(conn->user);
  free(conn->last_error);
  free(conn);
}

static bool mysql_driver_ping(DbConnection *conn) {
  if (!conn)
    return false;

  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql)
    return false;

  return mysql_ping(data->mysql) == 0;
}

static ConnStatus mysql_driver_status(DbConnection *conn) {
  return conn ? conn->status : CONN_STATUS_DISCONNECTED;
}

static const char *mysql_driver_get_error(DbConnection *conn) {
  return conn ? conn->last_error : NULL;
}

static int64_t mysql_driver_exec(DbConnection *conn, const char *sql,
                                 char **err) {
  if (!conn || !sql) {
    if (err)
      *err = str_dup("Invalid parameters");
    return -1;
  }

  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql) {
    if (err)
      *err = str_dup("Not connected");
    return -1;
  }

  if (mysql_query(data->mysql, sql) != 0) {
    if (err)
      *err = str_dup(mysql_error(data->mysql));
    return -1;
  }

  return (int64_t)mysql_affected_rows(data->mysql);
}

static bool mysql_driver_update_cell(DbConnection *conn, const char *table,
                                     const char **pk_cols,
                                     const DbValue *pk_vals, size_t num_pk_cols,
                                     const char *col, const DbValue *new_val,
                                     char **err) {
  if (!conn || !table || !pk_cols || !pk_vals || num_pk_cols == 0 || !col ||
      !new_val) {
    if (err)
      *err = str_dup("Invalid parameters");
    return false;
  }

  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql) {
    if (err)
      *err = str_dup("Not connected");
    return false;
  }

  /* Escape identifiers to prevent SQL injection */
  char *escaped_table = str_escape_identifier_backtick(table);
  char *escaped_col = str_escape_identifier_backtick(col);
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
    char *escaped_pk = str_escape_identifier_backtick(pk_cols[i]);
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

  MYSQL_STMT *stmt = mysql_stmt_init(data->mysql);
  if (!stmt) {
    free(sql);
    if (err)
      *err = str_dup("Failed to initialize statement");
    return false;
  }

  if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
    if (err)
      *err = str_dup(mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    free(sql);
    return false;
  }
  free(sql);

  /* Allocate bind array: 1 new value + N pk values */
  size_t num_params = 1 + num_pk_cols;
  MYSQL_BIND *bind = calloc(num_params, sizeof(MYSQL_BIND));
  long long *pk_ints = calloc(num_pk_cols, sizeof(long long));
  double *pk_floats = calloc(num_pk_cols, sizeof(double));
  my_bool *pk_is_nulls = calloc(num_pk_cols, sizeof(my_bool));
  unsigned long *pk_lens = calloc(num_pk_cols, sizeof(unsigned long));

  if (!bind || !pk_ints || !pk_floats || !pk_is_nulls || !pk_lens) {
    free(bind);
    free(pk_ints);
    free(pk_floats);
    free(pk_is_nulls);
    free(pk_lens);
    mysql_stmt_close(stmt);
    if (err)
      *err = str_dup("Memory allocation failed");
    return false;
  }

  /* Parameter 1: new value */
  long long new_int = 0;
  double new_float = 0;
  my_bool new_is_null = new_val->is_null;
  unsigned long new_len = 0;

  if (new_val->is_null) {
    bind[0].buffer_type = MYSQL_TYPE_NULL;
  } else {
    switch (new_val->type) {
    case DB_TYPE_INT:
      bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
      new_int = new_val->int_val;
      bind[0].buffer = &new_int;
      break;
    case DB_TYPE_FLOAT:
      bind[0].buffer_type = MYSQL_TYPE_DOUBLE;
      new_float = new_val->float_val;
      bind[0].buffer = &new_float;
      break;
    case DB_TYPE_BLOB:
      bind[0].buffer_type = MYSQL_TYPE_BLOB;
      bind[0].buffer = (void *)new_val->blob.data;
      bind[0].buffer_length = new_val->blob.len;
      new_len = new_val->blob.len;
      bind[0].length = &new_len;
      break;
    default:
      bind[0].buffer_type = MYSQL_TYPE_STRING;
      bind[0].buffer = new_val->text.data;
      bind[0].buffer_length = new_val->text.len;
      new_len = new_val->text.len;
      bind[0].length = &new_len;
      break;
    }
  }
  bind[0].is_null = &new_is_null;

  /* Parameters 2..N+1: primary key values */
  for (size_t i = 0; i < num_pk_cols; i++) {
    const DbValue *pk_val = &pk_vals[i];
    pk_is_nulls[i] = pk_val->is_null;

    if (pk_val->is_null) {
      bind[i + 1].buffer_type = MYSQL_TYPE_NULL;
    } else {
      switch (pk_val->type) {
      case DB_TYPE_INT:
        bind[i + 1].buffer_type = MYSQL_TYPE_LONGLONG;
        pk_ints[i] = pk_val->int_val;
        bind[i + 1].buffer = &pk_ints[i];
        break;
      case DB_TYPE_FLOAT:
        bind[i + 1].buffer_type = MYSQL_TYPE_DOUBLE;
        pk_floats[i] = pk_val->float_val;
        bind[i + 1].buffer = &pk_floats[i];
        break;
      default:
        bind[i + 1].buffer_type = MYSQL_TYPE_STRING;
        bind[i + 1].buffer = pk_val->text.data;
        bind[i + 1].buffer_length = pk_val->text.len;
        pk_lens[i] = pk_val->text.len;
        bind[i + 1].length = &pk_lens[i];
        break;
      }
    }
    bind[i + 1].is_null = &pk_is_nulls[i];
  }

  bool success = true;
  if (mysql_stmt_bind_param(stmt, bind) != 0) {
    if (err)
      *err = str_dup(mysql_stmt_error(stmt));
    success = false;
  } else if (mysql_stmt_execute(stmt) != 0) {
    if (err)
      *err = str_dup(mysql_stmt_error(stmt));
    success = false;
  }

  /* Consume any results from the statement before closing */
  for (int loop_guard = 0; loop_guard < MAX_RESULT_CONSUME_ITERATIONS &&
                           mysql_stmt_next_result(stmt) == 0;
       loop_guard++) {
    /* Just consume, no results expected from UPDATE */
  }

  mysql_stmt_close(stmt);

  /* Clear any pending results on the connection (for multi-statement safety) */
  for (int loop_guard = 0; loop_guard < MAX_RESULT_CONSUME_ITERATIONS &&
                           mysql_next_result(data->mysql) == 0;
       loop_guard++) {
    MYSQL_RES *res = mysql_store_result(data->mysql);
    if (res)
      mysql_free_result(res);
  }

  free(bind);
  free(pk_ints);
  free(pk_floats);
  free(pk_is_nulls);
  free(pk_lens);

  return success;
}

static bool mysql_driver_delete_row(DbConnection *conn, const char *table,
                                    const char **pk_cols,
                                    const DbValue *pk_vals, size_t num_pk_cols,
                                    char **err) {
  if (!conn || !table || !pk_cols || !pk_vals || num_pk_cols == 0) {
    if (err)
      *err = str_dup("Invalid parameters");
    return false;
  }

  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql) {
    if (err)
      *err = str_dup("Not connected");
    return false;
  }

  /* Escape identifiers to prevent SQL injection */
  char *escaped_table = str_escape_identifier_backtick(table);
  if (!escaped_table) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return false;
  }

  /* Build WHERE clause for composite primary key */
  char *where_clause = str_dup("");
  for (size_t i = 0; i < num_pk_cols; i++) {
    char *escaped_pk = str_escape_identifier_backtick(pk_cols[i]);
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

  MYSQL_STMT *stmt = mysql_stmt_init(data->mysql);
  if (!stmt) {
    free(sql);
    if (err)
      *err = str_dup("Failed to initialize statement");
    return false;
  }

  if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
    if (err)
      *err = str_dup(mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    free(sql);
    return false;
  }
  free(sql);

  /* Allocate bind array for pk values */
  MYSQL_BIND *bind = calloc(num_pk_cols, sizeof(MYSQL_BIND));
  long long *pk_ints = calloc(num_pk_cols, sizeof(long long));
  double *pk_floats = calloc(num_pk_cols, sizeof(double));
  my_bool *pk_is_nulls = calloc(num_pk_cols, sizeof(my_bool));
  unsigned long *pk_lens = calloc(num_pk_cols, sizeof(unsigned long));

  if (!bind || !pk_ints || !pk_floats || !pk_is_nulls || !pk_lens) {
    free(bind);
    free(pk_ints);
    free(pk_floats);
    free(pk_is_nulls);
    free(pk_lens);
    mysql_stmt_close(stmt);
    if (err)
      *err = str_dup("Memory allocation failed");
    return false;
  }

  /* Bind primary key values */
  for (size_t i = 0; i < num_pk_cols; i++) {
    const DbValue *pk_val = &pk_vals[i];
    pk_is_nulls[i] = pk_val->is_null;

    if (pk_val->is_null) {
      bind[i].buffer_type = MYSQL_TYPE_NULL;
    } else {
      switch (pk_val->type) {
      case DB_TYPE_INT:
        bind[i].buffer_type = MYSQL_TYPE_LONGLONG;
        pk_ints[i] = pk_val->int_val;
        bind[i].buffer = &pk_ints[i];
        break;
      case DB_TYPE_FLOAT:
        bind[i].buffer_type = MYSQL_TYPE_DOUBLE;
        pk_floats[i] = pk_val->float_val;
        bind[i].buffer = &pk_floats[i];
        break;
      default:
        bind[i].buffer_type = MYSQL_TYPE_STRING;
        bind[i].buffer = pk_val->text.data;
        bind[i].buffer_length = pk_val->text.len;
        pk_lens[i] = pk_val->text.len;
        bind[i].length = &pk_lens[i];
        break;
      }
    }
    bind[i].is_null = &pk_is_nulls[i];
  }

  bool success = true;
  if (mysql_stmt_bind_param(stmt, bind) != 0) {
    if (err)
      *err = str_dup(mysql_stmt_error(stmt));
    success = false;
  } else if (mysql_stmt_execute(stmt) != 0) {
    if (err)
      *err = str_dup(mysql_stmt_error(stmt));
    success = false;
  }

  /* Consume any results from the statement before closing */
  for (int loop_guard = 0; loop_guard < MAX_RESULT_CONSUME_ITERATIONS &&
                           mysql_stmt_next_result(stmt) == 0;
       loop_guard++) {
    /* Just consume, no results expected from DELETE */
  }

  mysql_stmt_close(stmt);

  /* Clear any pending results on the connection (for multi-statement safety) */
  for (int loop_guard = 0; loop_guard < MAX_RESULT_CONSUME_ITERATIONS &&
                           mysql_next_result(data->mysql) == 0;
       loop_guard++) {
    MYSQL_RES *res = mysql_store_result(data->mysql);
    if (res)
      mysql_free_result(res);
  }

  free(bind);
  free(pk_ints);
  free(pk_floats);
  free(pk_is_nulls);
  free(pk_lens);

  return success;
}

static char **mysql_driver_list_tables(DbConnection *conn, size_t *count,
                                       char **err) {
  if (!conn || !count) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  *count = 0;

  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql) {
    if (err)
      *err = str_dup("Not connected");
    return NULL;
  }

  if (mysql_query(data->mysql, "SHOW TABLES") != 0) {
    if (err)
      *err = str_dup(mysql_error(data->mysql));
    return NULL;
  }

  MYSQL_RES *result = mysql_store_result(data->mysql);
  if (!result) {
    if (err)
      *err = str_dup(mysql_error(data->mysql));
    return NULL;
  }

  size_t num_rows = mysql_num_rows(result);

  /* Handle zero tables - return empty array (not NULL) to distinguish from
   * error */
  if (num_rows == 0) {
    mysql_free_result(result);
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

  char **tables = malloc(num_rows * sizeof(char *));
  if (!tables) {
    mysql_free_result(result);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    if (row[0]) {
      tables[*count] = str_dup(row[0]);
      if (!tables[*count]) {
        /* Cleanup on allocation failure */
        for (size_t j = 0; j < *count; j++) {
          free(tables[j]);
        }
        free(tables);
        mysql_free_result(result);
        if (err)
          *err = str_dup("Memory allocation failed");
        return NULL;
      }
      (*count)++;
    }
  }

  mysql_free_result(result);
  return tables;
}

static TableSchema *mysql_driver_get_table_schema(DbConnection *conn,
                                                  const char *table,
                                                  char **err) {
  if (!conn || !table) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql) {
    if (err)
      *err = str_dup("Not connected");
    return NULL;
  }

  /* Escape identifier to prevent SQL injection */
  char *escaped_table = str_escape_identifier_backtick(table);
  if (!escaped_table) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  char *sql = str_printf("DESCRIBE %s", escaped_table);
  free(escaped_table);
  if (!sql) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  if (mysql_query(data->mysql, sql) != 0) {
    if (err)
      *err = str_dup(mysql_error(data->mysql));
    free(sql);
    return NULL;
  }
  free(sql);

  MYSQL_RES *result = mysql_store_result(data->mysql);
  if (!result) {
    if (err)
      *err = str_dup(mysql_error(data->mysql));
    return NULL;
  }

  TableSchema *schema = calloc(1, sizeof(TableSchema));
  if (!schema) {
    mysql_free_result(result);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  schema->name = str_dup(table);
  if (!schema->name) {
    mysql_free_result(result);
    free(schema);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }
  size_t num_rows = mysql_num_rows(result);
  schema->columns = calloc(num_rows, sizeof(ColumnDef));
  if (!schema->columns) {
    mysql_free_result(result);
    db_schema_free(schema);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  /* DESCRIBE returns: Field, Type, Null, Key, Default, Extra */
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result)) && schema->num_columns < num_rows) {
    ColumnDef *col = &schema->columns[schema->num_columns];
    memset(col, 0, sizeof(ColumnDef));

    col->name = row[0] ? str_dup(row[0]) : NULL;
    col->type_name = row[1] ? str_dup(row[1]) : NULL;
    col->nullable = row[2] && (str_eq_nocase(row[2], "YES"));
    col->primary_key = row[3] && str_eq(row[3], "PRI");
    col->default_val = (row[4] && row[4][0] && !str_eq(row[4], "NULL"))
                           ? str_dup(row[4])
                           : NULL;

    /* Map type */
    if (col->type_name) {
      char *type_lower = str_dup(col->type_name);
      if (type_lower) {
        for (char *c = type_lower; *c; c++)
          *c = tolower((unsigned char)*c);

        if (strstr(type_lower, "int") || strstr(type_lower, "serial"))
          col->type = DB_TYPE_INT;
        else if (strstr(type_lower, "float") || strstr(type_lower, "double") ||
                 strstr(type_lower, "decimal") || strstr(type_lower, "numeric"))
          col->type = DB_TYPE_FLOAT;
        else if (strstr(type_lower, "bool") || str_eq(type_lower, "tinyint(1)"))
          col->type = DB_TYPE_BOOL;
        else if (strstr(type_lower, "blob") || strstr(type_lower, "binary"))
          col->type = DB_TYPE_BLOB;
        else if (strstr(type_lower, "date") || strstr(type_lower, "time"))
          col->type = DB_TYPE_TIMESTAMP;
        else
          col->type = DB_TYPE_TEXT;

        free(type_lower);
      }
    }

    schema->num_columns++;
  }

  mysql_free_result(result);
  return schema;
}

static ResultSet *mysql_driver_query(DbConnection *conn, const char *sql,
                                     char **err) {
  if (!conn || !sql) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql) {
    if (err)
      *err = str_dup("Not connected");
    return NULL;
  }

  if (mysql_query(data->mysql, sql) != 0) {
    if (err)
      *err = str_dup(mysql_error(data->mysql));
    return NULL;
  }

  MYSQL_RES *result = mysql_store_result(data->mysql);
  if (!result) {
    /* Check if this was an INSERT/UPDATE/DELETE (no result expected) */
    if (mysql_field_count(data->mysql) == 0) {
      /* Return empty result set for non-SELECT */
      ResultSet *rs = calloc(1, sizeof(ResultSet));
      return rs;
    }
    if (err)
      *err = str_dup(mysql_error(data->mysql));
    return NULL;
  }

  ResultSet *rs = calloc(1, sizeof(ResultSet));
  if (!rs) {
    mysql_free_result(result);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  /* Get column info */
  unsigned int num_fields = mysql_num_fields(result);
  MYSQL_FIELD *fields = mysql_fetch_fields(result);
  if (!fields && num_fields > 0) {
    mysql_free_result(result);
    free(rs);
    if (err)
      *err = str_dup("Failed to get field metadata");
    return NULL;
  }

  rs->num_columns = num_fields;
  /* Note: calloc handles overflow checking internally, and num_fields (unsigned
     int) cannot exceed SIZE_MAX/sizeof(ColumnDef) on any realistic platform */
  rs->columns = calloc(num_fields, sizeof(ColumnDef));
  if (!rs->columns) {
    mysql_free_result(result);
    free(rs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  for (unsigned int i = 0; i < num_fields; i++) {
    rs->columns[i].name = str_dup(fields[i].name);
    rs->columns[i].type = mysql_type_to_db_type(fields[i].type);
    rs->columns[i].nullable = !(fields[i].flags & NOT_NULL_FLAG);
    rs->columns[i].primary_key = (fields[i].flags & PRI_KEY_FLAG) != 0;
  }

  /* Get rows */
  size_t num_rows = mysql_num_rows(result);
  if (num_rows > 0) {
    rs->rows = calloc(num_rows, sizeof(Row));
    if (!rs->rows) {
      mysql_free_result(result);
      db_result_free(rs);
      if (err)
        *err = str_dup("Memory allocation failed");
      return NULL;
    }
  }

  MYSQL_ROW row;
  size_t allocated_rows = num_rows;
  while ((row = mysql_fetch_row(result))) {
    /* Check if we need to expand the rows array */
    if (rs->num_rows >= allocated_rows) {
      size_t new_size;
      if (allocated_rows == 0) {
        new_size = 16;
      } else {
        /* Check for overflow before doubling */
        new_size = allocated_rows * 2;
        if (new_size < allocated_rows || new_size > SIZE_MAX / sizeof(Row)) {
          mysql_free_result(result);
          db_result_free(rs);
          if (err)
            *err = str_dup("Row count overflow");
          return NULL;
        }
      }
      Row *new_rows = realloc(rs->rows, new_size * sizeof(Row));
      if (!new_rows) {
        mysql_free_result(result);
        db_result_free(rs);
        if (err)
          *err = str_dup("Memory allocation failed");
        return NULL;
      }
      /* Zero out the new rows */
      memset(new_rows + allocated_rows, 0,
             (new_size - allocated_rows) * sizeof(Row));
      rs->rows = new_rows;
      allocated_rows = new_size;
    }

    unsigned long *lengths = mysql_fetch_lengths(result);

    Row *r = &rs->rows[rs->num_rows];
    r->cells = calloc(num_fields, sizeof(DbValue));
    r->num_cells = num_fields;

    if (!r->cells) {
      mysql_free_result(result);
      db_result_free(rs);
      if (err)
        *err = str_dup("Memory allocation failed");
      return NULL;
    }

    for (unsigned int i = 0; i < num_fields; i++) {
      r->cells[i] = mysql_get_value(row, lengths, i, &fields[i]);
    }

    rs->num_rows++;
  }

  mysql_free_result(result);
  return rs;
}

static ResultSet *mysql_driver_query_page(DbConnection *conn, const char *table,
                                          size_t offset, size_t limit,
                                          const char *order_by, bool desc,
                                          char **err) {
  if (!conn || !table) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  /* Escape identifiers to prevent SQL injection */
  char *escaped_table = str_escape_identifier_backtick(table);
  if (!escaped_table) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  /* Build query */
  char *sql;
  if (order_by) {
    char *escaped_order = str_escape_identifier_backtick(order_by);
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

  ResultSet *rs = mysql_driver_query(conn, sql, err);
  free(sql);

  return rs;
}

static void mysql_driver_free_result(ResultSet *rs) { db_result_free(rs); }

static void mysql_driver_free_schema(TableSchema *schema) {
  db_schema_free(schema);
}

static void mysql_driver_free_string_list(char **list, size_t count) {
  if (!list)
    return;
  for (size_t i = 0; i < count; i++) {
    free(list[i]);
  }
  free(list);
}

static void mysql_driver_library_cleanup(void) {
  static bool cleaned_up = false;
  if (cleaned_up)
    return;
  cleaned_up = true;
  mysql_library_end();
}

/* Cancel handle stores the thread ID for KILL QUERY */
typedef struct {
  unsigned long thread_id;
} MySqlCancelHandle;

static void *mysql_driver_prepare_cancel(DbConnection *conn) {
  if (!conn)
    return NULL;
  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql)
    return NULL;

  MySqlCancelHandle *handle = malloc(sizeof(MySqlCancelHandle));
  if (!handle)
    return NULL;

  handle->thread_id = mysql_thread_id(data->mysql);
  return handle;
}

static bool mysql_driver_cancel_query(DbConnection *conn, void *cancel_handle,
                                      char **err) {
  if (!conn || !cancel_handle) {
    if (err)
      *err = str_dup("Invalid parameters");
    return false;
  }

  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql) {
    if (err)
      *err = str_dup("Not connected");
    return false;
  }

  MySqlCancelHandle *handle = (MySqlCancelHandle *)cancel_handle;

  /* Execute KILL QUERY on the same connection
   * Note: This works because mysql_query is thread-safe for different
   * connections, and KILL QUERY is a fast operation that can interrupt
   * a running query.
   * For full robustness, a separate connection would be needed. */
  char kill_sql[64];
  snprintf(kill_sql, sizeof(kill_sql), "KILL QUERY %lu", handle->thread_id);

  /* We use mysql_send_query which is non-blocking for the send part */
  if (mysql_query(data->mysql, kill_sql) != 0) {
    if (err)
      *err = str_dup(mysql_error(data->mysql));
    return false;
  }

  /* Consume any result from KILL command */
  MYSQL_RES *result = mysql_store_result(data->mysql);
  if (result)
    mysql_free_result(result);

  return true;
}

static void mysql_driver_free_cancel_handle(void *cancel_handle) {
  free(cancel_handle);
}

/* Approximate row count using information_schema */
static int64_t mysql_driver_estimate_row_count(DbConnection *conn,
                                               const char *table, char **err) {
  if (!conn || !table) {
    if (err)
      *err = str_dup("Invalid parameters");
    return -1;
  }

  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql) {
    if (err)
      *err = str_dup("Not connected");
    return -1;
  }

  /* Escape table name for query */
  char escaped_table[256];
  size_t table_len = strlen(table);
  if (table_len >= sizeof(escaped_table) / 2) {
    if (err)
      *err = str_dup("Table name too long");
    return -1;
  }
  mysql_real_escape_string(data->mysql, escaped_table, table, table_len);

  /* Query information_schema for approximate row count */
  char *sql =
      str_printf("SELECT TABLE_ROWS FROM information_schema.TABLES "
                 "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '%s'",
                 escaped_table);
  if (!sql) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return -1;
  }

  if (mysql_query(data->mysql, sql) != 0) {
    if (err)
      *err = str_dup(mysql_error(data->mysql));
    free(sql);
    return -1;
  }
  free(sql);

  MYSQL_RES *result = mysql_store_result(data->mysql);
  if (!result) {
    if (err)
      *err = str_dup(mysql_error(data->mysql));
    return -1;
  }

  int64_t count = -1;
  MYSQL_ROW row = mysql_fetch_row(result);
  if (row && row[0]) {
    errno = 0;
    char *endptr;
    long long parsed = strtoll(row[0], &endptr, 10);
    if (errno == 0 && endptr != row[0] && parsed >= 0) {
      count = parsed;
    }
  }

  mysql_free_result(result);
  return count;
}
