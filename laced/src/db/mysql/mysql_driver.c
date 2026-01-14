/*
 * Lace
 * MySQL/MariaDB driver - uses libmariadb C API
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../util/mem.h"
#include "../../util/str.h"
#include "../connstr.h"
#include "../db.h"
#include "../db_common.h"
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

/*
 * Consume any pending results on the connection.
 * This is necessary for multi-statement safety after UPDATE/DELETE.
 */
static void mysql_consume_pending_results(MYSQL *mysql) {
  for (int i = 0; i < MAX_RESULT_CONSUME_ITERATIONS &&
                  mysql_next_result(mysql) == 0;
       i++) {
    MYSQL_RES *res = mysql_store_result(mysql);
    if (res)
      mysql_free_result(res);
  }
}

/*
 * Finish a prepared statement: consume results, close, and cleanup connection.
 * Call this before freeing bind arrays.
 */
static void mysql_stmt_finish(MYSQL_STMT *stmt, MYSQL *mysql) {
  /* Consume any results from the statement before closing */
  for (int i = 0; i < MAX_RESULT_CONSUME_ITERATIONS &&
                  mysql_stmt_next_result(stmt) == 0;
       i++) {
    /* Just consume, no results expected from DML */
  }
  mysql_stmt_close(stmt);
  mysql_consume_pending_results(mysql);
}

/* Bind a DbValue to a MYSQL_BIND entry.
 * Caller must provide stable storage for int/float values and is_null/length.
 * int_storage/float_storage: only one is used based on type
 * len_storage: used for text/blob length
 * null_storage: stores is_null flag */
static void mysql_bind_value(MYSQL_BIND *bind, const DbValue *val,
                             long long *int_storage, double *float_storage,
                             unsigned long *len_storage, my_bool *null_storage) {
  memset(bind, 0, sizeof(*bind));
  *null_storage = val->is_null;
  bind->is_null = null_storage;

  if (val->is_null) {
    bind->buffer_type = MYSQL_TYPE_NULL;
    return;
  }

  switch (val->type) {
  case DB_TYPE_INT:
    bind->buffer_type = MYSQL_TYPE_LONGLONG;
    *int_storage = val->int_val;
    bind->buffer = int_storage;
    break;
  case DB_TYPE_FLOAT:
    bind->buffer_type = MYSQL_TYPE_DOUBLE;
    *float_storage = val->float_val;
    bind->buffer = float_storage;
    break;
  case DB_TYPE_BOOL:
    bind->buffer_type = MYSQL_TYPE_LONGLONG;
    *int_storage = val->bool_val ? 1 : 0;
    bind->buffer = int_storage;
    break;
  case DB_TYPE_BLOB:
    if (!val->blob.data || val->blob.len == 0) {
      /* Empty blob - bind as NULL */
      bind->buffer_type = MYSQL_TYPE_NULL;
      *null_storage = true;
    } else {
      bind->buffer_type = MYSQL_TYPE_BLOB;
      bind->buffer = (void *)val->blob.data;
      bind->buffer_length = val->blob.len;
      *len_storage = val->blob.len;
      bind->length = len_storage;
    }
    break;
  default:
    bind->buffer_type = MYSQL_TYPE_STRING;
    bind->buffer = val->text.data;
    bind->buffer_length = val->text.len;
    *len_storage = val->text.len;
    bind->length = len_storage;
    break;
  }
}

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
static bool mysql_driver_insert_row(DbConnection *conn, const char *table,
                                    const ColumnDef *cols, const DbValue *vals,
                                    size_t num_cols, char **err);
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
    .insert_row = mysql_driver_insert_row,
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
    .insert_row = mysql_driver_insert_row,
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
    return db_value_oversized_placeholder("DATA", lengths[col]);
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
      val.text.data = safe_malloc((size_t)lengths[col] + 1);
      val.type = DB_TYPE_TEXT;
      val.text.len = (size_t)lengths[col];
      memcpy(val.text.data, row[col], (size_t)lengths[col]);
      val.text.data[(size_t)lengths[col]] = '\0';
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
      val.text.data = safe_malloc((size_t)lengths[col] + 1);
      val.type = DB_TYPE_TEXT;
      val.text.len = (size_t)lengths[col];
      memcpy(val.text.data, row[col], (size_t)lengths[col]);
      val.text.data[(size_t)lengths[col]] = '\0';
    }
    break;
  }

  case DB_TYPE_BLOB:
    val.blob.data = safe_malloc((size_t)lengths[col]);
    val.type = DB_TYPE_BLOB;
    val.blob.len = (size_t)lengths[col];
    memcpy(val.blob.data, row[col], (size_t)lengths[col]);
    break;

  default:
    val.text.data = safe_malloc((size_t)lengths[col] + 1);
    val.type = DB_TYPE_TEXT;
    val.text.len = (size_t)lengths[col];
    memcpy(val.text.data, row[col], (size_t)lengths[col]);
    val.text.data[(size_t)lengths[col]] = '\0';
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
    err_set(err, "Not a MySQL/MariaDB connection string");
    return NULL;
  }

  /* Initialize MySQL library (safe to call multiple times) */
  mysql_library_init(0, NULL, NULL);

  MYSQL *mysql = mysql_init(NULL);
  if (!mysql) {
    connstr_free(cs);
    err_set(err, "Failed to initialize MySQL connection");
    return NULL;
  }

  /* Set connection timeout */
  unsigned int timeout = 10;
  if (mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0) {
    mysql_close(mysql);
    connstr_free(cs);
    err_set(err, "Failed to set connection timeout");
    return NULL;
  }

  /* Enable automatic reconnection */
  bool reconnect = true;
  if (mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect) != 0) {
    mysql_close(mysql);
    connstr_free(cs);
    err_set(err, "Failed to set reconnect option");
    return NULL;
  }

  /* Set character set */
  if (mysql_options(mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
    mysql_close(mysql);
    connstr_free(cs);
    err_set(err, "Failed to set character set");
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
    err_setf(err, "Connection failed: %s", mysql_error(mysql));
    mysql_close(mysql);
    connstr_free(cs);
    return NULL;
  }

  MySqlData *data = safe_calloc(1, sizeof(MySqlData));
  data->mysql = mysql;
  data->database = str_dup(database);
  data->is_mariadb = is_mariadb;

  DbConnection *conn = safe_calloc(1, sizeof(DbConnection));

  conn->driver = is_mariadb ? &mariadb_driver : &mysql_driver;
  conn->connstr = str_dup(connstr);
  conn->database = str_dup(database);
  conn->host = str_dup(host);
  conn->port = port;
  conn->user = str_dup(user);
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

  db_common_free_connection(conn);
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
  DB_REQUIRE_PARAMS_CONN(sql, conn, MySqlData, data, mysql, err, -1);

  if (mysql_query(data->mysql, sql) != 0) {
    err_set(err, mysql_error(data->mysql));
    return -1;
  }

  return (int64_t)mysql_affected_rows(data->mysql);
}

static bool mysql_driver_update_cell(DbConnection *conn, const char *table,
                                     const char **pk_cols,
                                     const DbValue *pk_vals, size_t num_pk_cols,
                                     const char *col, const DbValue *new_val,
                                     char **err) {
  DB_REQUIRE_PARAMS_CONN(table && pk_cols && pk_vals && num_pk_cols > 0 && col &&
                             new_val,
                         conn, MySqlData, data, mysql, err, false);

  /* Escape table name */
  char *escaped_table = db_common_escape_table(table, DB_QUOTE_BACKTICK, false);
  if (!escaped_table) {
    err_set(err, "Memory allocation failed");
    return false;
  }

  /* Build UPDATE statement using common helper */
  char *sql = db_common_build_update_sql(escaped_table, col, pk_cols,
                                         num_pk_cols, DB_QUOTE_BACKTICK, false, err);
  free(escaped_table);

  if (!sql) {
    return false; /* Error already set by helper */
  }

  MYSQL_STMT *stmt = mysql_stmt_init(data->mysql);
  if (!stmt) {
    free(sql);
    err_set(err, "Failed to initialize statement");
    return false;
  }

  if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
    err_set(err, mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    free(sql);
    return false;
  }
  free(sql);

  /* Allocate bind array: 1 new value + N pk values */
  size_t num_params = 1 + num_pk_cols;
  MYSQL_BIND *bind = safe_calloc(num_params, sizeof(MYSQL_BIND));
  long long *pk_ints = safe_calloc(num_pk_cols, sizeof(long long));
  double *pk_floats = safe_calloc(num_pk_cols, sizeof(double));
  my_bool *pk_is_nulls = safe_calloc(num_pk_cols, sizeof(my_bool));
  unsigned long *pk_lens = safe_calloc(num_pk_cols, sizeof(unsigned long));

  /* Parameter 1: new value */
  long long new_int = 0;
  double new_float = 0;
  my_bool new_is_null = 0;
  unsigned long new_len = 0;
  mysql_bind_value(&bind[0], new_val, &new_int, &new_float, &new_len,
                   &new_is_null);

  /* Parameters 2..N+1: primary key values */
  for (size_t i = 0; i < num_pk_cols; i++) {
    mysql_bind_value(&bind[i + 1], &pk_vals[i], &pk_ints[i], &pk_floats[i],
                     &pk_lens[i], &pk_is_nulls[i]);
  }

  bool success = true;
  if (mysql_stmt_bind_param(stmt, bind) != 0) {
    err_set(err, mysql_stmt_error(stmt));
    success = false;
  } else if (mysql_stmt_execute(stmt) != 0) {
    err_set(err, mysql_stmt_error(stmt));
    success = false;
  }

  mysql_stmt_finish(stmt, data->mysql);

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
  DB_REQUIRE_PARAMS_CONN(table && pk_cols && pk_vals && num_pk_cols > 0, conn,
                         MySqlData, data, mysql, err, false);

  /* Escape table name */
  char *escaped_table = db_common_escape_table(table, DB_QUOTE_BACKTICK, false);
  if (!escaped_table) {
    err_set(err, "Memory allocation failed");
    return false;
  }

  /* Build DELETE statement using common helper */
  char *sql = db_common_build_delete_sql(escaped_table, pk_cols, num_pk_cols,
                                         DB_QUOTE_BACKTICK, false, err);
  free(escaped_table);

  if (!sql) {
    return false; /* Error already set by helper */
  }

  MYSQL_STMT *stmt = mysql_stmt_init(data->mysql);
  if (!stmt) {
    free(sql);
    err_set(err, "Failed to initialize statement");
    return false;
  }

  if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
    err_set(err, mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    free(sql);
    return false;
  }
  free(sql);

  /* Allocate bind array for pk values */
  MYSQL_BIND *bind = safe_calloc(num_pk_cols, sizeof(MYSQL_BIND));
  long long *pk_ints = safe_calloc(num_pk_cols, sizeof(long long));
  double *pk_floats = safe_calloc(num_pk_cols, sizeof(double));
  my_bool *pk_is_nulls = safe_calloc(num_pk_cols, sizeof(my_bool));
  unsigned long *pk_lens = safe_calloc(num_pk_cols, sizeof(unsigned long));

  /* Bind primary key values */
  for (size_t i = 0; i < num_pk_cols; i++) {
    mysql_bind_value(&bind[i], &pk_vals[i], &pk_ints[i], &pk_floats[i],
                     &pk_lens[i], &pk_is_nulls[i]);
  }

  bool success = true;
  if (mysql_stmt_bind_param(stmt, bind) != 0) {
    err_set(err, mysql_stmt_error(stmt));
    success = false;
  } else if (mysql_stmt_execute(stmt) != 0) {
    err_set(err, mysql_stmt_error(stmt));
    success = false;
  }

  mysql_stmt_finish(stmt, data->mysql);

  free(bind);
  free(pk_ints);
  free(pk_floats);
  free(pk_is_nulls);
  free(pk_lens);

  return success;
}

static bool mysql_driver_insert_row(DbConnection *conn, const char *table,
                                    const ColumnDef *cols, const DbValue *vals,
                                    size_t num_cols, char **err) {
  DB_REQUIRE_PARAMS_CONN(table && cols && vals && num_cols > 0, conn, MySqlData,
                         data, mysql, err, false);

  /* Escape table name */
  char *escaped_table = db_common_escape_table(table, DB_QUOTE_BACKTICK, false);
  if (!escaped_table) {
    err_set(err, "Memory allocation failed");
    return false;
  }

  /* Build INSERT statement using common helper */
  DbInsertLists lists;
  char *sql = db_common_build_insert_sql(escaped_table, cols, vals, num_cols,
                                         DB_QUOTE_BACKTICK, false, &lists, err);
  free(escaped_table);

  if (!sql) {
    return false; /* Error already set by helper */
  }

  /* Handle case where all columns are auto_increment */
  if (lists.num_params == 0) {
    db_common_free_insert_lists(&lists);

    if (mysql_query(data->mysql, sql) != 0) {
      err_set(err, mysql_error(data->mysql));
      free(sql);
      return false;
    }
    free(sql);
    return true;
  }

  MYSQL_STMT *stmt = mysql_stmt_init(data->mysql);
  if (!stmt) {
    free(sql);
    db_common_free_insert_lists(&lists);
    err_set(err, "Failed to initialize statement");
    return false;
  }

  if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
    err_set(err, mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    free(sql);
    db_common_free_insert_lists(&lists);
    return false;
  }
  free(sql);

  /* Allocate bind array and value storage */
  MYSQL_BIND *bind = safe_calloc(lists.num_params, sizeof(MYSQL_BIND));
  long long *val_ints = safe_calloc(lists.num_params, sizeof(long long));
  double *val_floats = safe_calloc(lists.num_params, sizeof(double));
  my_bool *val_is_nulls = safe_calloc(lists.num_params, sizeof(my_bool));
  unsigned long *val_lens = safe_calloc(lists.num_params, sizeof(unsigned long));

  /* Bind values using column map from common helper */
  for (size_t i = 0; i < lists.num_params; i++) {
    size_t col_idx = lists.col_map[i];
    mysql_bind_value(&bind[i], &vals[col_idx], &val_ints[i],
                     &val_floats[i], &val_lens[i], &val_is_nulls[i]);
  }

  db_common_free_insert_lists(&lists);

  bool success = true;
  if (mysql_stmt_bind_param(stmt, bind) != 0) {
    err_set(err, mysql_stmt_error(stmt));
    success = false;
  } else if (mysql_stmt_execute(stmt) != 0) {
    err_set(err, mysql_stmt_error(stmt));
    success = false;
  }

  mysql_stmt_finish(stmt, data->mysql);

  free(bind);
  free(val_ints);
  free(val_floats);
  free(val_is_nulls);
  free(val_lens);

  return success;
}

static char **mysql_driver_list_tables(DbConnection *conn, size_t *count,
                                       char **err) {
  DB_REQUIRE_PARAMS_CONN(count, conn, MySqlData, data, mysql, err, NULL);
  *count = 0;

  if (mysql_query(data->mysql, "SHOW TABLES") != 0) {
    err_set(err, mysql_error(data->mysql));
    return NULL;
  }

  MYSQL_RES *result = mysql_store_result(data->mysql);
  if (!result) {
    err_set(err, mysql_error(data->mysql));
    return NULL;
  }

  size_t num_rows = mysql_num_rows(result);

  /* Handle zero tables - return empty array (not NULL) to distinguish from
   * error */
  if (num_rows == 0) {
    mysql_free_result(result);
    char **tables = safe_calloc(1, sizeof(char *));
    tables[0] = NULL; /* NULL-terminated empty array */
    *count = 0;
    return tables;
  }

  char **tables = safe_calloc(num_rows, sizeof(char *));

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    if (row[0]) {
      tables[*count] = str_dup(row[0]);
      (*count)++;
    }
  }

  mysql_free_result(result);
  return tables;
}

static TableSchema *mysql_driver_get_table_schema(DbConnection *conn,
                                                  const char *table,
                                                  char **err) {
  DB_REQUIRE_PARAMS_CONN(table, conn, MySqlData, data, mysql, err, NULL);

  /* Escape identifier to prevent SQL injection */
  char *escaped_table = str_escape_identifier_backtick(table);
  if (!escaped_table) {
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  char *sql = str_printf("DESCRIBE %s", escaped_table);
  free(escaped_table);
  if (!sql) {
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  if (mysql_query(data->mysql, sql) != 0) {
    err_set(err, mysql_error(data->mysql));
    free(sql);
    return NULL;
  }
  free(sql);

  MYSQL_RES *result = mysql_store_result(data->mysql);
  if (!result) {
    err_set(err, mysql_error(data->mysql));
    return NULL;
  }

  TableSchema *schema = safe_calloc(1, sizeof(TableSchema));

  schema->name = str_dup(table);
  size_t num_rows = mysql_num_rows(result);
  schema->columns = safe_calloc(num_rows, sizeof(ColumnDef));

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
    /* Extra column (row[5]) contains "auto_increment" for auto-increment cols
     */
    col->auto_increment = row[5] && strcasestr(row[5], "auto_increment");

    /* Map type */
    if (col->type_name) {
      char *type_lower = str_dup(col->type_name);
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

    schema->num_columns++;
  }

  mysql_free_result(result);

  /* Get index info using SHOW INDEX */
  escaped_table = str_escape_identifier_backtick(table);
  if (escaped_table) {
    sql = str_printf("SHOW INDEX FROM %s", escaped_table);
    free(escaped_table);

    if (sql && mysql_query(data->mysql, sql) == 0) {
      MYSQL_RES *idx_result = mysql_store_result(data->mysql);
      if (idx_result) {
        /* Count unique index names */
        size_t num_idx_rows = mysql_num_rows(idx_result);
        if (num_idx_rows > 0) {
          /* Collect unique index names first */
          char **idx_names = safe_calloc(num_idx_rows, sizeof(char *));
          size_t num_unique_idx = 0;

          {
            MYSQL_ROW idx_row;
            while ((idx_row = mysql_fetch_row(idx_result))) {
              /* Column 2 is Key_name */
              char *idx_name = idx_row[2];
              bool found = false;
              for (size_t j = 0; j < num_unique_idx; j++) {
                if (str_eq(idx_names[j], idx_name)) {
                  found = true;
                  break;
                }
              }
              if (!found && num_unique_idx < num_idx_rows) {
                idx_names[num_unique_idx++] = idx_name;
              }
            }

            schema->indexes = safe_calloc(num_unique_idx, sizeof(IndexDef));
            {
              mysql_data_seek(idx_result, 0);

              for (size_t i = 0; i < num_unique_idx; i++) {
                IndexDef *idx = &schema->indexes[schema->num_indexes];
                memset(idx, 0, sizeof(IndexDef));
                idx->name = str_dup(idx_names[i]);

                /* Count columns for this index and collect properties */
                mysql_data_seek(idx_result, 0);
                size_t col_count = 0;
                while ((idx_row = mysql_fetch_row(idx_result))) {
                  if (str_eq(idx_row[2], idx_names[i])) {
                    if (col_count == 0) {
                      /* First row: get index properties */
                      /* Column 1: Non_unique (0=unique) */
                      idx->unique = idx_row[1] && idx_row[1][0] == '0';
                      idx->primary = str_eq(idx_row[2], "PRIMARY");
                      /* Column 10: Index_type */
                      idx->type = idx_row[10] ? str_dup(idx_row[10]) : NULL;
                    }
                    col_count++;
                  }
                }

                if (col_count > 0) {
                  idx->columns = safe_calloc(col_count, sizeof(char *));
                  mysql_data_seek(idx_result, 0);
                  size_t c = 0;
                  while ((idx_row = mysql_fetch_row(idx_result)) &&
                         c < col_count) {
                    if (str_eq(idx_row[2], idx_names[i])) {
                      /* Column 4: Column_name */
                      idx->columns[c++] =
                          idx_row[4] ? str_dup(idx_row[4]) : NULL;
                    }
                  }
                  idx->num_columns = c;
                }

                schema->num_indexes++;
              }
            }
            free(idx_names);
          }
        }
        mysql_free_result(idx_result);
      }
    }
    free(sql);
  }

  /* Get foreign key info from information_schema */
  /* Need to get database name for the query */
  const char *db_name = data->database;
  if (db_name) {
    sql = str_printf(
        "SELECT "
        "  kcu.CONSTRAINT_NAME, "
        "  kcu.COLUMN_NAME, "
        "  kcu.REFERENCED_TABLE_NAME, "
        "  kcu.REFERENCED_COLUMN_NAME, "
        "  rc.DELETE_RULE, "
        "  rc.UPDATE_RULE "
        "FROM information_schema.KEY_COLUMN_USAGE kcu "
        "JOIN information_schema.REFERENTIAL_CONSTRAINTS rc "
        "  ON kcu.CONSTRAINT_NAME = rc.CONSTRAINT_NAME "
        "  AND kcu.CONSTRAINT_SCHEMA = rc.CONSTRAINT_SCHEMA "
        "WHERE kcu.TABLE_SCHEMA = '%s' AND kcu.TABLE_NAME = '%s' "
        "  AND kcu.REFERENCED_TABLE_NAME IS NOT NULL "
        "ORDER BY kcu.CONSTRAINT_NAME, kcu.ORDINAL_POSITION",
        db_name, table);

    if (sql && mysql_query(data->mysql, sql) == 0) {
      MYSQL_RES *fk_result = mysql_store_result(data->mysql);
      if (fk_result) {
        size_t num_fk_rows = mysql_num_rows(fk_result);
        if (num_fk_rows > 0) {
          /* Count unique constraint names */
          char **fk_names = safe_calloc(num_fk_rows, sizeof(char *));
          size_t num_unique_fk = 0;

          {
            MYSQL_ROW fk_row;
            while ((fk_row = mysql_fetch_row(fk_result))) {
              char *fk_name = fk_row[0];
              bool found = false;
              for (size_t j = 0; j < num_unique_fk; j++) {
                if (str_eq(fk_names[j], fk_name)) {
                  found = true;
                  break;
                }
              }
              if (!found && num_unique_fk < num_fk_rows) {
                fk_names[num_unique_fk++] = fk_name;
              }
            }

            schema->foreign_keys = safe_calloc(num_unique_fk, sizeof(ForeignKeyDef));
            {
              for (size_t i = 0; i < num_unique_fk; i++) {
                ForeignKeyDef *fk =
                    &schema->foreign_keys[schema->num_foreign_keys];
                memset(fk, 0, sizeof(ForeignKeyDef));
                fk->name = str_dup(fk_names[i]);

                /* Count columns for this FK */
                mysql_data_seek(fk_result, 0);
                size_t col_count = 0;
                while ((fk_row = mysql_fetch_row(fk_result))) {
                  if (str_eq(fk_row[0], fk_names[i])) {
                    if (col_count == 0) {
                      /* First row: get FK properties */
                      fk->ref_table = fk_row[2] ? str_dup(fk_row[2]) : NULL;
                      fk->on_delete = fk_row[4] ? str_dup(fk_row[4]) : NULL;
                      fk->on_update = fk_row[5] ? str_dup(fk_row[5]) : NULL;
                    }
                    col_count++;
                  }
                }

                if (col_count > 0) {
                  fk->columns = safe_calloc(col_count, sizeof(char *));
                  fk->ref_columns = safe_calloc(col_count, sizeof(char *));
                  mysql_data_seek(fk_result, 0);
                  size_t c = 0;
                  while ((fk_row = mysql_fetch_row(fk_result)) &&
                         c < col_count) {
                    if (str_eq(fk_row[0], fk_names[i])) {
                      fk->columns[c] = fk_row[1] ? str_dup(fk_row[1]) : NULL;
                      fk->ref_columns[c] =
                          fk_row[3] ? str_dup(fk_row[3]) : NULL;
                      c++;
                    }
                  }
                  fk->num_columns = c;
                  fk->num_ref_columns = c;
                }

                schema->num_foreign_keys++;
              }
            }
            free(fk_names);
          }
        }
        mysql_free_result(fk_result);
      }
    }
    free(sql);
  }

  return schema;
}

static ResultSet *mysql_driver_query(DbConnection *conn, const char *sql,
                                     char **err) {
  DB_REQUIRE_PARAMS_CONN(sql, conn, MySqlData, data, mysql, err, NULL);

  if (mysql_query(data->mysql, sql) != 0) {
    err_set(err, mysql_error(data->mysql));
    return NULL;
  }

  MYSQL_RES *result = mysql_store_result(data->mysql);
  if (!result) {
    /* Check if this was an INSERT/UPDATE/DELETE (no result expected) */
    if (mysql_field_count(data->mysql) == 0) {
      /* Return empty result set for non-SELECT */
      ResultSet *rs = db_result_alloc_empty();
      return rs;
    }
    err_set(err, mysql_error(data->mysql));
    return NULL;
  }

  ResultSet *rs = db_result_alloc_empty();
  if (!rs) {
    mysql_free_result(result);
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  /* Get column info */
  unsigned int num_fields = mysql_num_fields(result);
  MYSQL_FIELD *fields = mysql_fetch_fields(result);
  if (!fields && num_fields > 0) {
    mysql_free_result(result);
    free(rs);
    err_set(err, "Failed to get field metadata");
    return NULL;
  }

  rs->num_columns = num_fields;
  rs->columns = safe_calloc(num_fields, sizeof(ColumnDef));

  for (unsigned int i = 0; i < num_fields; i++) {
    rs->columns[i].name = str_dup(fields[i].name);
    rs->columns[i].type = mysql_type_to_db_type(fields[i].type);
    rs->columns[i].nullable = !(fields[i].flags & NOT_NULL_FLAG);
    rs->columns[i].primary_key = (fields[i].flags & PRI_KEY_FLAG) != 0;
  }

  /* Get rows */
  size_t num_rows = mysql_num_rows(result);
  /* Limit result set size to prevent unbounded memory growth */
  size_t max_rows = conn->max_result_rows > 0 ? conn->max_result_rows
                                              : (size_t)MAX_RESULT_ROWS;
  if (num_rows > max_rows) {
    num_rows = max_rows;
  }
  if (num_rows > 0) {
    rs->rows = safe_calloc(num_rows, sizeof(Row));
  }

  MYSQL_ROW row;
  size_t allocated_rows = num_rows;
  while ((row = mysql_fetch_row(result))) {
    /* Stop if we've reached the maximum result set size */
    if (rs->num_rows >= max_rows) {
      break;
    }
    /* Check if we need to expand the rows array */
    if (rs->num_rows >= allocated_rows) {
      size_t new_size = allocated_rows == 0 ? 16 : allocated_rows * 2;
      Row *new_rows = safe_reallocarray(rs->rows, new_size, sizeof(Row));
      /* Zero out the new rows */
      memset(new_rows + allocated_rows, 0,
             (new_size - allocated_rows) * sizeof(Row));
      rs->rows = new_rows;
      allocated_rows = new_size;
    }

    unsigned long *lengths = mysql_fetch_lengths(result);
    if (!lengths) {
      /* mysql_fetch_lengths can return NULL on error */
      mysql_free_result(result);
      db_result_free(rs);
      err_set(err, "Failed to get field lengths");
      return NULL;
    }

    Row *r = &rs->rows[rs->num_rows];
    r->cells = safe_calloc(num_fields, sizeof(DbValue));
    r->num_cells = num_fields;

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
  DB_REQUIRE(table, err, NULL);

  /* Escape table name */
  char *escaped_table = db_common_escape_table(table, DB_QUOTE_BACKTICK, false);
  if (!escaped_table) {
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  /* Build paginated query using common helper */
  char *sql = db_common_build_query_page_sql(escaped_table, offset, limit,
                                             order_by, desc, DB_QUOTE_BACKTICK, err);
  free(escaped_table);

  if (!sql) {
    return NULL; /* Error already set by helper */
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
  db_common_free_string_list(list, count);
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

  MySqlCancelHandle *handle = safe_malloc(sizeof(MySqlCancelHandle));
  handle->thread_id = mysql_thread_id(data->mysql);
  return handle;
}

static bool mysql_driver_cancel_query(DbConnection *conn, void *cancel_handle,
                                      char **err) {
  if (!conn || !cancel_handle) {
    err_set(err, "Invalid parameters");
    return false;
  }

  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql) {
    err_set(err, "Not connected");
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
    err_set(err, mysql_error(data->mysql));
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
  DB_REQUIRE_PARAMS_CONN(table, conn, MySqlData, data, mysql, err, -1);

  /* Escape table name for query */
  char escaped_table[256];
  size_t table_len = strlen(table);
  /* mysql_real_escape_string can expand to 2*len+1 in worst case */
  if (table_len * 2 + 1 > sizeof(escaped_table)) {
    err_set(err, "Table name too long");
    return -1;
  }
  mysql_real_escape_string(data->mysql, escaped_table, table, table_len);

  /* Query information_schema for approximate row count */
  char *sql =
      str_printf("SELECT TABLE_ROWS FROM information_schema.TABLES "
                 "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '%s'",
                 escaped_table);
  if (!sql) {
    err_set(err, "Memory allocation failed");
    return -1;
  }

  if (mysql_query(data->mysql, sql) != 0) {
    err_set(err, mysql_error(data->mysql));
    free(sql);
    return -1;
  }
  free(sql);

  MYSQL_RES *result = mysql_store_result(data->mysql);
  if (!result) {
    err_set(err, mysql_error(data->mysql));
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
