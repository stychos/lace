/*
 * Lace
 * PostgreSQL driver - uses libpq C API
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../util/str.h"
#include "../connstr.h"
#include "../db.h"
#include <ctype.h>
#include <errno.h>
#include <libpq-fe.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helper to safely cast size_t to int for libpq paramLengths.
 * Returns 0 on overflow (safe default for text format where length is ignored).
 * Note: paramLengths is only used for binary format; text format ignores it. */
static inline int safe_size_to_int(size_t sz) {
  if (sz > (size_t)INT_MAX) {
    return 0; /* Safe default - text format ignores paramLengths anyway */
  }
  return (int)sz;
}

/* Parse schema-qualified table name (schema.table or just table).
 * Returns schema (or "public" if unqualified) and table name.
 * Caller must free both returned strings. */
static bool pg_parse_table_name(const char *full_name, char **schema_out,
                                char **table_out) {
  if (!full_name || !schema_out || !table_out)
    return false;

  const char *dot = strchr(full_name, '.');
  if (dot) {
    /* Schema-qualified: schema.table */
    size_t schema_len = (size_t)(dot - full_name);
    *schema_out = str_ndup(full_name, schema_len);
    *table_out = str_dup(dot + 1);
  } else {
    /* Unqualified: assume public schema */
    *schema_out = str_dup("public");
    *table_out = str_dup(full_name);
  }

  if (!*schema_out || !*table_out) {
    free(*schema_out);
    free(*table_out);
    *schema_out = NULL;
    *table_out = NULL;
    return false;
  }
  return true;
}

/* Escape schema-qualified table name for SQL.
 * Returns "schema"."table" format. Caller must free. */
static char *pg_escape_table_name(const char *full_name) {
  char *schema = NULL, *table = NULL;
  if (!pg_parse_table_name(full_name, &schema, &table))
    return NULL;

  char *escaped_schema = str_escape_identifier_dquote(schema);
  char *escaped_table = str_escape_identifier_dquote(table);
  free(schema);
  free(table);

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

/* PostgreSQL connection data */
typedef struct {
  PGconn *conn;
  char *database;
} PgData;

/* Forward declarations */
static DbConnection *pg_connect(const char *connstr, char **err);
static void pg_disconnect(DbConnection *conn);
static bool pg_ping(DbConnection *conn);
static ConnStatus pg_status(DbConnection *conn);
static const char *pg_get_error(DbConnection *conn);
static char **pg_list_tables(DbConnection *conn, size_t *count, char **err);
static TableSchema *pg_get_table_schema(DbConnection *conn, const char *table,
                                        char **err);
static ResultSet *pg_query(DbConnection *conn, const char *sql, char **err);
static int64_t pg_exec(DbConnection *conn, const char *sql, char **err);
static ResultSet *pg_query_page(DbConnection *conn, const char *table,
                                size_t offset, size_t limit,
                                const char *order_by, bool desc, char **err);
static bool pg_update_cell(DbConnection *conn, const char *table,
                           const char **pk_cols, const DbValue *pk_vals,
                           size_t num_pk_cols, const char *col,
                           const DbValue *new_val, char **err);
static bool pg_delete_row(DbConnection *conn, const char *table,
                          const char **pk_cols, const DbValue *pk_vals,
                          size_t num_pk_cols, char **err);
static void pg_free_result(ResultSet *rs);
static void pg_free_schema(TableSchema *schema);
static void pg_free_string_list(char **list, size_t count);
static void *pg_prepare_cancel(DbConnection *conn);
static bool pg_cancel_query(DbConnection *conn, void *cancel_handle,
                            char **err);
static void pg_free_cancel_handle(void *cancel_handle);
static int64_t pg_estimate_row_count(DbConnection *conn, const char *table,
                                     char **err);

/* Driver definition */
DbDriver postgres_driver = {
    .name = "postgres",
    .display_name = "PostgreSQL",
    .connect = pg_connect,
    .disconnect = pg_disconnect,
    .ping = pg_ping,
    .status = pg_status,
    .get_error = pg_get_error,
    .list_databases = NULL,
    .list_tables = pg_list_tables,
    .get_table_schema = pg_get_table_schema,
    .query = pg_query,
    .exec = pg_exec,
    .query_page = pg_query_page,
    .update_cell = pg_update_cell,
    .insert_row = NULL,
    .delete_row = pg_delete_row,
    .begin_transaction = NULL,
    .commit = NULL,
    .rollback = NULL,
    .free_result = pg_free_result,
    .free_schema = pg_free_schema,
    .free_string_list = pg_free_string_list,
    .prepare_cancel = pg_prepare_cancel,
    .cancel_query = pg_cancel_query,
    .free_cancel_handle = pg_free_cancel_handle,
    .estimate_row_count = pg_estimate_row_count,
    .library_cleanup = NULL,
};

/* Map PostgreSQL OID to DbValueType */
static DbValueType pg_oid_to_db_type(Oid oid) {
  switch (oid) {
  case 20: /* int8 */
  case 21: /* int2 */
  case 23: /* int4 */
  case 26: /* oid */
    return DB_TYPE_INT;

  case 700:  /* float4 */
  case 701:  /* float8 */
  case 1700: /* numeric */
    return DB_TYPE_FLOAT;

  case 16: /* bool */
    return DB_TYPE_BOOL;

  case 17: /* bytea */
    return DB_TYPE_BLOB;

  case 1082: /* date */
  case 1083: /* time */
  case 1114: /* timestamp */
  case 1184: /* timestamptz */
    return DB_TYPE_TIMESTAMP;

  default:
    return DB_TYPE_TEXT;
  }
}

/* Get DbValue from PGresult */
static DbValue pg_get_value(PGresult *res, int row, int col, Oid oid) {
  DbValue val;
  memset(&val, 0, sizeof(val));

  if (PQgetisnull(res, row, col)) {
    val.type = DB_TYPE_NULL;
    val.is_null = true;
    return val;
  }

  val.is_null = false;
  char *value = PQgetvalue(res, row, col);
  int len = PQgetlength(res, row, col);

  /* Validate length is non-negative */
  if (len < 0) {
    val.type = DB_TYPE_NULL;
    val.is_null = true;
    return val;
  }

  /* For oversized fields, show placeholder text instead of loading data */
  if ((size_t)len > MAX_FIELD_SIZE) {
    char placeholder[64];
    snprintf(placeholder, sizeof(placeholder), "[DATA: %d bytes]", len);
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

  DbValueType type = pg_oid_to_db_type(oid);

  switch (type) {
  case DB_TYPE_INT: {
    char *endptr;
    errno = 0;
    long long parsed = strtoll(value, &endptr, 10);
    if (errno == 0 && endptr != value && *endptr == '\0') {
      val.type = DB_TYPE_INT;
      val.int_val = parsed;
    } else {
      /* Conversion failed - store as text instead */
      val.text.data = malloc(len + 1);
      if (val.text.data) {
        val.type = DB_TYPE_TEXT;
        val.text.len = len;
        memcpy(val.text.data, value, len);
        val.text.data[len] = '\0';
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
    double parsed = strtod(value, &endptr);
    if (errno == 0 && endptr != value && *endptr == '\0') {
      val.type = DB_TYPE_FLOAT;
      val.float_val = parsed;
    } else {
      /* Conversion failed - store as text instead */
      val.text.data = malloc(len + 1);
      if (val.text.data) {
        val.type = DB_TYPE_TEXT;
        val.text.len = len;
        memcpy(val.text.data, value, len);
        val.text.data[len] = '\0';
      } else {
        val.type = DB_TYPE_NULL;
        val.is_null = true;
      }
    }
    break;
  }

  case DB_TYPE_BOOL:
    val.type = DB_TYPE_BOOL;
    val.bool_val = (value[0] == 't' || value[0] == 'T' || value[0] == '1');
    break;

  case DB_TYPE_BLOB:
    /* PostgreSQL bytea in text format needs unescaping */
    if (len >= 2 && value[0] == '\\' && value[1] == 'x') {
      /* Hex format: \xDEADBEEF - must have even number of hex digits */
      size_t hex_chars = len - 2;
      if (hex_chars % 2 != 0) {
        /* Odd number of hex digits - malformed, treat as raw */
        val.blob.data = malloc(len);
        if (val.blob.data) {
          val.type = DB_TYPE_BLOB;
          memcpy(val.blob.data, value, len);
          val.blob.len = len;
        } else {
          val.type = DB_TYPE_NULL;
          val.is_null = true;
        }
        break;
      }
      size_t hex_len = hex_chars / 2;
      if (hex_len == 0) {
        /* Empty blob \x */
        val.type = DB_TYPE_BLOB;
        val.blob.data = NULL;
        val.blob.len = 0;
        break;
      }
      /* Validate all hex characters first */
      bool valid_hex = true;
      for (size_t i = 0; i < hex_chars; i++) {
        if (!isxdigit((unsigned char)value[2 + i])) {
          valid_hex = false;
          break;
        }
      }
      if (!valid_hex) {
        /* Invalid hex characters - treat as raw data */
        val.blob.data = malloc(len);
        if (val.blob.data) {
          val.type = DB_TYPE_BLOB;
          memcpy(val.blob.data, value, len);
          val.blob.len = len;
        } else {
          val.type = DB_TYPE_NULL;
          val.is_null = true;
        }
        break;
      }
      val.blob.data = malloc(hex_len);
      if (val.blob.data) {
        val.type = DB_TYPE_BLOB;
        val.blob.len = hex_len;
        for (size_t i = 0; i < hex_len; i++) {
          /* Convert two hex chars to byte - already validated above */
          char hex_pair[3] = {value[2 + i * 2], value[2 + i * 2 + 1], '\0'};
          val.blob.data[i] = (uint8_t)strtoul(hex_pair, NULL, 16);
        }
      } else {
        /* Malloc failed - mark as null */
        val.type = DB_TYPE_NULL;
        val.is_null = true;
      }
    } else {
      /* Escape format or raw */
      val.blob.data = malloc(len);
      if (val.blob.data) {
        val.type = DB_TYPE_BLOB;
        memcpy(val.blob.data, value, len);
        val.blob.len = len;
      } else {
        /* Malloc failed - mark as null */
        val.type = DB_TYPE_NULL;
        val.is_null = true;
      }
    }
    break;

  default:
    val.text.data = malloc(len + 1);
    if (val.text.data) {
      val.type = DB_TYPE_TEXT;
      val.text.len = len;
      memcpy(val.text.data, value, len);
      val.text.data[len] = '\0';
    } else {
      /* Malloc failed - mark as null */
      val.type = DB_TYPE_NULL;
      val.is_null = true;
    }
    break;
  }

  return val;
}

static DbConnection *pg_connect(const char *connstr, char **err) {
  ConnString *cs = connstr_parse(connstr, err);
  if (!cs) {
    return NULL;
  }

  if (!str_eq(cs->driver, "postgres") && !str_eq(cs->driver, "postgresql") &&
      !str_eq(cs->driver, "pg")) {
    connstr_free(cs);
    if (err)
      *err = str_dup("Not a PostgreSQL connection string");
    return NULL;
  }

  /* Build libpq connection using PQconnectdbParams for proper escaping */
  const char *host = cs->host ? cs->host : "localhost";
  int port = cs->port > 0 ? cs->port : 5432;
  const char *user = cs->user ? cs->user : "postgres";
  const char *password = cs->password;
  const char *database = cs->database ? cs->database : "postgres";

  /* Convert port to string */
  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", port);

  /* Use PQconnectdbParams which handles escaping of special characters */
  const char *keywords[6];
  const char *values[6];
  int idx = 0;

  keywords[idx] = "host";
  values[idx++] = host;
  keywords[idx] = "port";
  values[idx++] = port_str;
  keywords[idx] = "user";
  values[idx++] = user;
  keywords[idx] = "dbname";
  values[idx++] = database;
  if (password) {
    keywords[idx] = "password";
    values[idx++] = password;
  }
  keywords[idx] = NULL;
  values[idx] = NULL;

  PGconn *pgconn = PQconnectdbParams(keywords, values, 0);

  if (PQstatus(pgconn) != CONNECTION_OK) {
    if (err)
      *err = str_printf("Connection failed: %s", PQerrorMessage(pgconn));
    PQfinish(pgconn);
    connstr_free(cs);
    return NULL;
  }

  /* Set client encoding to UTF-8 */
  PQsetClientEncoding(pgconn, "UTF8");

  PgData *data = calloc(1, sizeof(PgData));
  if (!data) {
    PQfinish(pgconn);
    connstr_free(cs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  data->conn = pgconn;
  data->database = str_dup(database);
  if (!data->database) {
    PQfinish(pgconn);
    free(data);
    connstr_free(cs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  DbConnection *conn = calloc(1, sizeof(DbConnection));
  if (!conn) {
    PQfinish(pgconn);
    free(data->database);
    free(data);
    connstr_free(cs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  conn->driver = &postgres_driver;
  conn->connstr = str_dup(connstr);
  conn->database = str_dup(database);
  conn->host = str_dup(host);
  conn->port = port;
  conn->user = str_dup(user);

  /* Check all allocations succeeded */
  if (!conn->connstr || !conn->database || !conn->host || !conn->user) {
    PQfinish(pgconn);
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

static void pg_disconnect(DbConnection *conn) {
  if (!conn)
    return;

  PgData *data = conn->driver_data;
  if (data) {
    if (data->conn) {
      PQfinish(data->conn);
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

static bool pg_ping(DbConnection *conn) {
  if (!conn)
    return false;

  PgData *data = conn->driver_data;
  if (!data || !data->conn)
    return false;

  /* Check connection status */
  if (PQstatus(data->conn) != CONNECTION_OK) {
    /* Try to reset */
    PQreset(data->conn);
    return PQstatus(data->conn) == CONNECTION_OK;
  }

  return true;
}

static ConnStatus pg_status(DbConnection *conn) {
  return conn ? conn->status : CONN_STATUS_DISCONNECTED;
}

static const char *pg_get_error(DbConnection *conn) {
  return conn ? conn->last_error : NULL;
}

static int64_t pg_exec(DbConnection *conn, const char *sql, char **err) {
  if (!conn || !sql) {
    if (err)
      *err = str_dup("Invalid parameters");
    return -1;
  }

  PgData *data = conn->driver_data;
  if (!data || !data->conn) {
    if (err)
      *err = str_dup("Not connected");
    return -1;
  }

  PGresult *res = PQexec(data->conn, sql);
  ExecStatusType status = PQresultStatus(res);

  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    if (err)
      *err = str_dup(PQerrorMessage(data->conn));
    PQclear(res);
    return -1;
  }

  char *affected = PQcmdTuples(res);
  int64_t count = 0;
  if (affected && *affected) {
    char *endptr;
    errno = 0;
    long long parsed = strtoll(affected, &endptr, 10);
    if (errno == 0 && endptr != affected) {
      count = parsed;
    }
  }
  PQclear(res);
  return count;
}

static bool pg_update_cell(DbConnection *conn, const char *table,
                           const char **pk_cols, const DbValue *pk_vals,
                           size_t num_pk_cols, const char *col,
                           const DbValue *new_val, char **err) {
  if (!conn || !table || !pk_cols || !pk_vals || num_pk_cols == 0 || !col ||
      !new_val) {
    if (err)
      *err = str_dup("Invalid parameters");
    return false;
  }

  /* Validate num_pk_cols fits in int for libpq (with room for +1) */
  if (num_pk_cols > (size_t)(INT_MAX - 1)) {
    if (err)
      *err = str_dup("Too many primary key columns");
    return false;
  }

  PgData *data = conn->driver_data;
  if (!data || !data->conn) {
    if (err)
      *err = str_dup("Not connected");
    return false;
  }

  /* Escape identifiers to prevent SQL injection */
  char *escaped_table = pg_escape_table_name(table);
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
      new_where = str_printf("%s = $%zu", escaped_pk, i + 2);
    } else {
      new_where =
          str_printf("%s AND %s = $%zu", where_clause, escaped_pk, i + 2);
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
  char *sql = str_printf("UPDATE %s SET %s = $1 WHERE %s", escaped_table,
                         escaped_col, where_clause);
  free(escaped_table);
  free(escaped_col);
  free(where_clause);

  if (!sql) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return false;
  }

  /* Prepare parameter values: 1 new value + N pk values */
  size_t num_params = 1 + num_pk_cols;
  /* PostgreSQL supports max 65535 parameters */
  if (num_params > 65535) {
    free(sql);
    if (err)
      *err = str_dup("Too many parameters (PostgreSQL limit: 65535)");
    return false;
  }
  const char **paramValues = malloc(sizeof(char *) * num_params);
  int *paramLengths = malloc(sizeof(int) * num_params);
  int *paramFormats = calloc(num_params, sizeof(int)); /* All text format (0) */
  char **pk_bufs = calloc(num_pk_cols, sizeof(char *));
  char *new_str = NULL;

  if (!paramValues || !paramLengths || !paramFormats || !pk_bufs) {
    free(paramValues);
    free(paramLengths);
    free(paramFormats);
    free(pk_bufs);
    free(sql);
    if (err)
      *err = str_dup("Memory allocation failed");
    return false;
  }

  /* Parameter 1: new value (heap-allocated for safety) */
  if (new_val->is_null) {
    paramValues[0] = NULL;
    paramLengths[0] = 0;
  } else {
    switch (new_val->type) {
    case DB_TYPE_INT:
      new_str = str_printf("%lld", (long long)new_val->int_val);
      paramValues[0] = new_str;
      paramLengths[0] = new_str ? safe_size_to_int(strlen(new_str)) : 0;
      break;
    case DB_TYPE_FLOAT:
      new_str = str_printf("%g", new_val->float_val);
      paramValues[0] = new_str;
      paramLengths[0] = new_str ? safe_size_to_int(strlen(new_str)) : 0;
      break;
    case DB_TYPE_BOOL:
      new_str = str_dup(new_val->bool_val ? "t" : "f");
      paramValues[0] = new_str;
      paramLengths[0] = new_str ? 1 : 0;
      break;
    case DB_TYPE_BLOB:
      new_str = str_dup("\\x");
      paramValues[0] = new_str;
      paramLengths[0] = new_str ? safe_size_to_int(strlen(new_str)) : 0;
      break;
    default:
      paramValues[0] = new_val->text.data;
      paramLengths[0] = safe_size_to_int(new_val->text.len);
      break;
    }
  }

  /* Parameters 2..N+1: primary key values */
  for (size_t i = 0; i < num_pk_cols; i++) {
    const DbValue *pk_val = &pk_vals[i];
    if (pk_val->is_null) {
      paramValues[i + 1] = NULL;
      paramLengths[i + 1] = 0;
    } else {
      switch (pk_val->type) {
      case DB_TYPE_INT:
        pk_bufs[i] = str_printf("%lld", (long long)pk_val->int_val);
        paramValues[i + 1] = pk_bufs[i];
        paramLengths[i + 1] =
            pk_bufs[i] ? safe_size_to_int(strlen(pk_bufs[i])) : 0;
        break;
      case DB_TYPE_FLOAT:
        pk_bufs[i] = str_printf("%g", pk_val->float_val);
        paramValues[i + 1] = pk_bufs[i];
        paramLengths[i + 1] =
            pk_bufs[i] ? safe_size_to_int(strlen(pk_bufs[i])) : 0;
        break;
      default:
        paramValues[i + 1] = pk_val->text.data;
        paramLengths[i + 1] = safe_size_to_int(pk_val->text.len);
        break;
      }
    }
  }

  PGresult *res =
      PQexecParams(data->conn, sql, safe_size_to_int(num_params), NULL,
                   paramValues, paramLengths, paramFormats, 0);
  free(sql);
  free(new_str);
  for (size_t i = 0; i < num_pk_cols; i++)
    free(pk_bufs[i]);
  free(pk_bufs);
  free(paramValues);
  free(paramLengths);
  free(paramFormats);

  if (!res) {
    if (err)
      *err = str_dup(PQerrorMessage(data->conn));
    return false;
  }

  ExecStatusType status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK) {
    if (err)
      *err = str_dup(PQerrorMessage(data->conn));
    PQclear(res);
    return false;
  }

  PQclear(res);
  return true;
}

static bool pg_delete_row(DbConnection *conn, const char *table,
                          const char **pk_cols, const DbValue *pk_vals,
                          size_t num_pk_cols, char **err) {
  if (!conn || !table || !pk_cols || !pk_vals || num_pk_cols == 0) {
    if (err)
      *err = str_dup("Invalid parameters");
    return false;
  }

  /* PostgreSQL supports max 65535 parameters */
  if (num_pk_cols > 65535) {
    if (err)
      *err = str_dup("Too many primary key columns (PostgreSQL limit: 65535)");
    return false;
  }

  PgData *data = conn->driver_data;
  if (!data || !data->conn) {
    if (err)
      *err = str_dup("Not connected");
    return false;
  }

  /* Escape schema-qualified table name */
  char *escaped_table = pg_escape_table_name(table);
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
      new_where = str_printf("%s = $%zu", escaped_pk, i + 1);
    } else {
      new_where =
          str_printf("%s AND %s = $%zu", where_clause, escaped_pk, i + 1);
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

  /* Prepare parameter values */
  const char **paramValues = malloc(sizeof(char *) * num_pk_cols);
  int *paramLengths = malloc(sizeof(int) * num_pk_cols);
  int *paramFormats =
      calloc(num_pk_cols, sizeof(int)); /* All text format (0) */
  char **pk_bufs = calloc(num_pk_cols, sizeof(char *));

  if (!paramValues || !paramLengths || !paramFormats || !pk_bufs) {
    free(paramValues);
    free(paramLengths);
    free(paramFormats);
    free(pk_bufs);
    free(sql);
    if (err)
      *err = str_dup("Memory allocation failed");
    return false;
  }

  /* Fill in primary key values */
  for (size_t i = 0; i < num_pk_cols; i++) {
    const DbValue *pk_val = &pk_vals[i];
    if (pk_val->is_null) {
      paramValues[i] = NULL;
      paramLengths[i] = 0;
    } else {
      switch (pk_val->type) {
      case DB_TYPE_INT:
        pk_bufs[i] = str_printf("%lld", (long long)pk_val->int_val);
        if (!pk_bufs[i]) {
          for (size_t j = 0; j < i; j++)
            free(pk_bufs[j]);
          free(pk_bufs);
          free(paramValues);
          free(paramLengths);
          free(paramFormats);
          free(sql);
          if (err)
            *err = str_dup("Memory allocation failed");
          return false;
        }
        paramValues[i] = pk_bufs[i];
        paramLengths[i] = safe_size_to_int(strlen(pk_bufs[i]));
        break;
      case DB_TYPE_FLOAT:
        pk_bufs[i] = str_printf("%g", pk_val->float_val);
        if (!pk_bufs[i]) {
          for (size_t j = 0; j < i; j++)
            free(pk_bufs[j]);
          free(pk_bufs);
          free(paramValues);
          free(paramLengths);
          free(paramFormats);
          free(sql);
          if (err)
            *err = str_dup("Memory allocation failed");
          return false;
        }
        paramValues[i] = pk_bufs[i];
        paramLengths[i] = safe_size_to_int(strlen(pk_bufs[i]));
        break;
      default:
        paramValues[i] = pk_val->text.data;
        paramLengths[i] = safe_size_to_int(pk_val->text.len);
        break;
      }
    }
  }

  PGresult *res =
      PQexecParams(data->conn, sql, safe_size_to_int(num_pk_cols), NULL,
                   paramValues, paramLengths, paramFormats, 0);
  free(sql);
  for (size_t i = 0; i < num_pk_cols; i++)
    free(pk_bufs[i]);
  free(pk_bufs);
  free(paramValues);
  free(paramLengths);
  free(paramFormats);

  if (!res) {
    if (err)
      *err = str_dup(PQerrorMessage(data->conn));
    return false;
  }

  ExecStatusType status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK) {
    if (err)
      *err = str_dup(PQerrorMessage(data->conn));
    PQclear(res);
    return false;
  }

  PQclear(res);
  return true;
}

static char **pg_list_tables(DbConnection *conn, size_t *count, char **err) {
  if (!conn || !count) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  *count = 0;

  PgData *data = conn->driver_data;
  if (!data || !data->conn) {
    if (err)
      *err = str_dup("Not connected");
    return NULL;
  }

  /* List tables from all user schemas, prefixing with schema name when not
   * 'public' */
  const char *sql =
      "SELECT CASE WHEN schemaname = 'public' THEN tablename "
      "ELSE schemaname || '.' || tablename END AS full_name "
      "FROM pg_tables "
      "WHERE schemaname NOT IN ('pg_catalog', 'information_schema') "
      "ORDER BY schemaname, tablename";
  PGresult *res = PQexec(data->conn, sql);

  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    if (err)
      *err = str_dup(PQerrorMessage(data->conn));
    PQclear(res);
    return NULL;
  }

  int num_rows = PQntuples(res);
  if (num_rows < 0) {
    PQclear(res);
    if (err)
      *err = str_dup("Invalid row count from database");
    return NULL;
  }

  /* Handle zero tables - return empty array (not NULL) to distinguish from
   * error */
  if (num_rows == 0) {
    PQclear(res);
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
    PQclear(res);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  for (int i = 0; i < num_rows; i++) {
    tables[i] = str_dup(PQgetvalue(res, i, 0));
    if (!tables[i]) {
      /* Free already allocated strings on failure */
      for (int j = 0; j < i; j++) {
        free(tables[j]);
      }
      free(tables);
      PQclear(res);
      if (err)
        *err = str_dup("Memory allocation failed");
      return NULL;
    }
    (*count)++;
  }

  PQclear(res);
  return tables;
}

static TableSchema *pg_get_table_schema(DbConnection *conn, const char *table,
                                        char **err) {
  if (!conn || !table) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  PgData *data = conn->driver_data;
  if (!data || !data->conn) {
    if (err)
      *err = str_dup("Not connected");
    return NULL;
  }

  /* Parse schema-qualified table name */
  char *schema_name = NULL, *table_name = NULL;
  if (!pg_parse_table_name(table, &schema_name, &table_name)) {
    if (err)
      *err = str_dup("Failed to parse table name");
    return NULL;
  }

  /* Query column information */
  const char *sql =
      "SELECT column_name, data_type, is_nullable, column_default "
      "FROM information_schema.columns "
      "WHERE table_schema = $1 AND table_name = $2 "
      "ORDER BY ordinal_position";

  const char *paramValues[2] = {schema_name, table_name};
  PGresult *res =
      PQexecParams(data->conn, sql, 2, NULL, paramValues, NULL, NULL, 0);

  free(schema_name);
  free(table_name);

  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    if (err)
      *err = str_dup(PQerrorMessage(data->conn));
    PQclear(res);
    return NULL;
  }

  TableSchema *schema = calloc(1, sizeof(TableSchema));
  if (!schema) {
    PQclear(res);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  schema->name = str_dup(table);
  if (!schema->name) {
    PQclear(res);
    free(schema);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }
  int num_rows = PQntuples(res);
  if (num_rows < 0) {
    PQclear(res);
    db_schema_free(schema);
    if (err)
      *err = str_dup("Invalid row count from database");
    return NULL;
  }
  schema->columns =
      calloc(num_rows > 0 ? (size_t)num_rows : 1, sizeof(ColumnDef));
  if (!schema->columns) {
    PQclear(res);
    db_schema_free(schema);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  for (int i = 0; i < num_rows; i++) {
    ColumnDef *col = &schema->columns[schema->num_columns];
    memset(col, 0, sizeof(ColumnDef));

    col->name = str_dup(PQgetvalue(res, i, 0));
    col->type_name = str_dup(PQgetvalue(res, i, 1));
    if (!col->name || !col->type_name) {
      /* Free partially allocated column before cleanup */
      free(col->name);
      free(col->type_name);
      PQclear(res);
      db_schema_free(schema);
      if (err)
        *err = str_dup("Memory allocation failed");
      return NULL;
    }
    col->nullable = str_eq_nocase(PQgetvalue(res, i, 2), "YES");

    char *default_val = PQgetvalue(res, i, 3);
    col->default_val =
        (default_val && *default_val) ? str_dup(default_val) : NULL;

    /* Map type - guard against NULL type_name */
    char *type = col->type_name;
    if (!type) {
      col->type = DB_TYPE_TEXT;
    } else if (strstr(type, "int") || strstr(type, "serial")) {
      col->type = DB_TYPE_INT;
    } else if (strstr(type, "float") || strstr(type, "double") ||
               strstr(type, "numeric") || strstr(type, "decimal")) {
      col->type = DB_TYPE_FLOAT;
    } else if (strstr(type, "bool")) {
      col->type = DB_TYPE_BOOL;
    } else if (strstr(type, "bytea")) {
      col->type = DB_TYPE_BLOB;
    } else if (strstr(type, "timestamp") || strstr(type, "date") ||
               strstr(type, "time")) {
      col->type = DB_TYPE_TIMESTAMP;
    } else {
      col->type = DB_TYPE_TEXT;
    }

    schema->num_columns++;
  }

  PQclear(res);

  /* Get primary key info */
  sql = "SELECT a.attname FROM pg_index i "
        "JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = "
        "ANY(i.indkey) "
        "WHERE i.indrelid = $1::regclass AND i.indisprimary";

  paramValues[0] = table;
  res = PQexecParams(data->conn, sql, 1, NULL, paramValues, NULL, NULL, 0);

  if (PQresultStatus(res) == PGRES_TUPLES_OK) {
    int pk_rows = PQntuples(res);
    for (int i = 0; i < pk_rows; i++) {
      char *pk_col = PQgetvalue(res, i, 0);
      for (size_t j = 0; j < schema->num_columns; j++) {
        if (str_eq(schema->columns[j].name, pk_col)) {
          schema->columns[j].primary_key = true;
          break;
        }
      }
    }
  }

  PQclear(res);
  return schema;
}

static ResultSet *pg_query(DbConnection *conn, const char *sql, char **err) {
  if (!conn || !sql) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  PgData *data = conn->driver_data;
  if (!data || !data->conn) {
    if (err)
      *err = str_dup("Not connected");
    return NULL;
  }

  PGresult *res = PQexec(data->conn, sql);
  ExecStatusType status = PQresultStatus(res);

  if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
    if (err)
      *err = str_dup(PQerrorMessage(data->conn));
    PQclear(res);
    return NULL;
  }

  ResultSet *rs = calloc(1, sizeof(ResultSet));
  if (!rs) {
    PQclear(res);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  if (status == PGRES_COMMAND_OK) {
    /* Non-SELECT statement */
    PQclear(res);
    return rs;
  }

  /* Get column info */
  int num_fields = PQnfields(res);
  if (num_fields < 0) {
    PQclear(res);
    free(rs);
    if (err)
      *err = str_dup("Invalid column count from database");
    return NULL;
  }
  rs->num_columns = (size_t)num_fields;
  rs->columns =
      calloc(num_fields > 0 ? (size_t)num_fields : 1, sizeof(ColumnDef));
  if (!rs->columns) {
    PQclear(res);
    free(rs);
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  for (int i = 0; i < num_fields; i++) {
    const char *fname = PQfname(res, i);
    rs->columns[i].name = fname ? str_dup(fname) : str_dup("?");
    if (!rs->columns[i].name) {
      PQclear(res);
      db_result_free(rs);
      if (err)
        *err = str_dup("Memory allocation failed");
      return NULL;
    }
    rs->columns[i].type = pg_oid_to_db_type(PQftype(res, i));
  }

  /* Get rows */
  int num_rows = PQntuples(res);
  if (num_rows < 0) {
    PQclear(res);
    db_result_free(rs);
    if (err)
      *err = str_dup("Invalid row count from database");
    return NULL;
  }
  /* Limit result set size to prevent unbounded memory growth */
  int max_rows = conn->max_result_rows > 0 ? (int)conn->max_result_rows
                                           : MAX_RESULT_ROWS;
  if (num_rows > max_rows) {
    num_rows = max_rows;
  }
  if (num_rows > 0) {
    rs->rows = calloc((size_t)num_rows, sizeof(Row));
    if (!rs->rows) {
      PQclear(res);
      db_result_free(rs);
      if (err)
        *err = str_dup("Memory allocation failed");
      return NULL;
    }
  }

  for (int r = 0; r < num_rows; r++) {
    Row *row = &rs->rows[rs->num_rows];
    row->cells =
        calloc(num_fields > 0 ? (size_t)num_fields : 1, sizeof(DbValue));
    row->num_cells = (size_t)num_fields;

    if (!row->cells) {
      PQclear(res);
      db_result_free(rs);
      if (err)
        *err = str_dup("Memory allocation failed");
      return NULL;
    }

    for (int c = 0; c < num_fields; c++) {
      row->cells[c] = pg_get_value(res, r, c, PQftype(res, c));
    }

    rs->num_rows++;
  }

  PQclear(res);
  return rs;
}

static ResultSet *pg_query_page(DbConnection *conn, const char *table,
                                size_t offset, size_t limit,
                                const char *order_by, bool desc, char **err) {
  if (!conn || !table) {
    if (err)
      *err = str_dup("Invalid parameters");
    return NULL;
  }

  /* Escape schema-qualified table name */
  char *escaped_table = pg_escape_table_name(table);
  if (!escaped_table) {
    if (err)
      *err = str_dup("Memory allocation failed");
    return NULL;
  }

  /* Build query */
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

  ResultSet *rs = pg_query(conn, sql, err);
  free(sql);

  return rs;
}

static void pg_free_result(ResultSet *rs) { db_result_free(rs); }

static void pg_free_schema(TableSchema *schema) { db_schema_free(schema); }

static void pg_free_string_list(char **list, size_t count) {
  if (!list)
    return;
  for (size_t i = 0; i < count; i++) {
    free(list[i]);
  }
  free(list);
}

/* Query cancellation support */
static void *pg_prepare_cancel(DbConnection *conn) {
  if (!conn)
    return NULL;
  PgData *data = conn->driver_data;
  if (!data || !data->conn)
    return NULL;
  return PQgetCancel(data->conn);
}

static bool pg_cancel_query(DbConnection *conn, void *cancel_handle,
                            char **err) {
  (void)conn; /* Not needed for PostgreSQL cancel */
  if (!cancel_handle) {
    if (err)
      *err = str_dup("Invalid cancel handle");
    return false;
  }

  PGcancel *cancel = (PGcancel *)cancel_handle;
  char errbuf[256];

  if (PQcancel(cancel, errbuf, sizeof(errbuf)) == 0) {
    if (err)
      *err = str_dup(errbuf);
    return false;
  }
  return true;
}

static void pg_free_cancel_handle(void *cancel_handle) {
  if (cancel_handle) {
    PQfreeCancel((PGcancel *)cancel_handle);
  }
}

/* Approximate row count using pg_class.reltuples */
static int64_t pg_estimate_row_count(DbConnection *conn, const char *table,
                                     char **err) {
  if (!conn || !table) {
    if (err)
      *err = str_dup("Invalid parameters");
    return -1;
  }

  PgData *data = conn->driver_data;
  if (!data || !data->conn) {
    if (err)
      *err = str_dup("Not connected");
    return -1;
  }

  /* Parse schema-qualified table name */
  char *schema_name = NULL, *table_name = NULL;
  if (!pg_parse_table_name(table, &schema_name, &table_name)) {
    if (err)
      *err = str_dup("Failed to parse table name");
    return -1;
  }

  /* Query pg_class for approximate row count */
  const char *sql = "SELECT reltuples::bigint FROM pg_class c "
                    "JOIN pg_namespace n ON n.oid = c.relnamespace "
                    "WHERE n.nspname = $1 AND c.relname = $2";

  const char *params[2] = {schema_name, table_name};
  PGresult *res = PQexecParams(data->conn, sql, 2, NULL, params, NULL, NULL, 0);

  free(schema_name);
  free(table_name);

  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    if (err)
      *err = str_dup(PQerrorMessage(data->conn));
    PQclear(res);
    return -1;
  }

  int64_t count = -1;
  if (PQntuples(res) > 0 && !PQgetisnull(res, 0, 0)) {
    const char *val = PQgetvalue(res, 0, 0);
    if (val && *val) {
      errno = 0;
      char *endptr;
      long long parsed = strtoll(val, &endptr, 10);
      /* reltuples can be -1 if never analyzed, treat as unavailable */
      if (errno == 0 && endptr != val && parsed >= 0) {
        count = parsed;
      }
    }
  }

  PQclear(res);
  return count;
}
