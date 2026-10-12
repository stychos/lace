/*
 * Lace
 * MySQL/MariaDB driver - uses libmariadb C API
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "util/mem.h"
#include "util/str.h"
#include "util/connstr.h"
#include "../db.h"
#include "../db_common.h"
#include "../../../../liblace/include/lace.h"
#include <ctype.h>
#include <errno.h>
#include <mysql.h>
#include <errmsg.h> /* CR_SERVER_GONE_ERROR, CR_SERVER_LOST */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Maximum iterations for consuming pending results (prevents infinite loops) */
#define MAX_RESULT_CONSUME_ITERATIONS 1000

/* MySQL connection data */
typedef struct {
  MYSQL *mysql;
  char *database;
  bool is_mariadb; /* Connection scheme was mariadb:// */
  time_t last_used; /* For the liveness check before a statement */
  DbLinkState link; /* Lost connection and lost transaction */
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

/* Forward declarations */
static DbConnection *mysql_driver_connect(const char *connstr, char **err);
static void mysql_driver_disconnect(DbConnection *conn);
static ResultSet *mysql_driver_query(DbConnection *conn, const char *sql,
                                     char **err);
static bool mysql_driver_ensure_ready(DbConnection *conn, const char *sql,
                                      bool *answered, char **err);
static void mysql_driver_library_cleanup(void);
static void *mysql_driver_prepare_cancel(DbConnection *conn);
static bool mysql_driver_cancel_query(DbConnection *conn, void *cancel_handle,
                                      char **err);
static void mysql_driver_free_cancel_handle(void *cancel_handle);
static void mysql_driver_library_init(void);
static int mysql_driver_transaction_state(DbConnection *conn);
static void mysql_driver_connection_lost(DbConnection *conn);
static void mysql_driver_cancel_handle_finished(void *cancel_handle);

/* Driver definitions - both mysql and mariadb use the same implementation */
DbDriver mysql_driver = {
    .name = "mysql",
    .display_name = "MySQL",
    .connect = mysql_driver_connect,
    .disconnect = mysql_driver_disconnect,
    .query = mysql_driver_query,
    .ensure_ready = mysql_driver_ensure_ready,
    .prepare_cancel = mysql_driver_prepare_cancel,
    .cancel_query = mysql_driver_cancel_query,
    .free_cancel_handle = mysql_driver_free_cancel_handle,
    .cancel_handle_finished = mysql_driver_cancel_handle_finished,
    .transaction_state = mysql_driver_transaction_state,
    .connection_lost = mysql_driver_connection_lost,
    .library_init = mysql_driver_library_init,
    .library_cleanup = mysql_driver_library_cleanup,
};

DbDriver mariadb_driver = {
    .name = "mariadb",
    .display_name = "MariaDB",
    .connect = mysql_driver_connect,
    .disconnect = mysql_driver_disconnect,
    .query = mysql_driver_query,
    .ensure_ready = mysql_driver_ensure_ready,
    .prepare_cancel = mysql_driver_prepare_cancel,
    .cancel_query = mysql_driver_cancel_query,
    .free_cancel_handle = mysql_driver_free_cancel_handle,
    .cancel_handle_finished = mysql_driver_cancel_handle_finished,
    .transaction_state = mysql_driver_transaction_state,
    .connection_lost = mysql_driver_connection_lost,
    .library_init = mysql_driver_library_init,
    .library_cleanup = mysql_driver_library_cleanup,
};

/* Binary charset ID in MySQL/MariaDB */
#define MYSQL_BINARY_CHARSET 63

/* Map MySQL field type to DbValueType.
 * For BLOB-like types, we need the charset to distinguish BLOB from TEXT.
 * In MySQL/MariaDB, TEXT types (TINYTEXT, TEXT, MEDIUMTEXT, LONGTEXT) share
 * the same type codes as BLOB types - the difference is the charset:
 * - charset 63 (binary) = actual BLOB (binary data)
 * - any other charset = TEXT (character data) */
static DbValueType mysql_type_to_db_type(enum enum_field_types type,
                                         unsigned int charsetnr) {
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
    return DB_TYPE_FLOAT;

  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
    return DB_TYPE_DECIMAL; /* Exact: a double would round it */

  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_VARCHAR:
    /* BINARY/VARBINARY carry the binary charset: bytes, not text (as text
     * they were cut at the first NUL and compared as strings) */
    return (charsetnr == MYSQL_BINARY_CHARSET) ? DB_TYPE_BLOB : DB_TYPE_TEXT;

  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
    /* Check charset: binary = BLOB, otherwise it's TEXT */
    return (charsetnr == MYSQL_BINARY_CHARSET) ? DB_TYPE_BLOB : DB_TYPE_TEXT;

  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
    return DB_TYPE_TIMESTAMP;

  case MYSQL_TYPE_BIT:      /* Bits come as raw bytes (b'0' is a NUL byte) */
  case MYSQL_TYPE_GEOMETRY: /* Well-known binary */
    return DB_TYPE_BLOB;

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
  if (lengths[col] > LACE_MAX_FIELD_SIZE) {
    return db_value_oversized_placeholder("DATA", lengths[col]);
  }

  val.is_null = false;
  DbValueType type = mysql_type_to_db_type(field->type, field->charsetnr);

  switch (type) {
  case DB_TYPE_INT: {
    char *endptr;
    errno = 0;
    long long parsed = strtoll(row[col], &endptr, 10);
    if (errno == 0 && endptr != row[col] && *endptr == '\0') {
      val.type = DB_TYPE_INT;
      val.int_val = parsed;
    } else {
      /* Beyond int64 (BIGINT UNSIGNED): keep the exact digits as a DECIMAL,
       * written back unquoted (MySQL compares a quoted one as a double) */
      val.text.data = safe_malloc((size_t)lengths[col] + 1);
      val.type = DB_TYPE_DECIMAL;
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
    val.blob.data = safe_malloc((size_t)lengths[col] ? (size_t)lengths[col] : 1);
    val.type = DB_TYPE_BLOB;
    val.blob.len = (size_t)lengths[col];
    memcpy(val.blob.data, row[col], (size_t)lengths[col]);
    break;

  case DB_TYPE_DECIMAL:
    val.text.data = safe_malloc((size_t)lengths[col] + 1);
    val.type = DB_TYPE_DECIMAL;
    val.text.len = (size_t)lengths[col];
    memcpy(val.text.data, row[col], (size_t)lengths[col]);
    val.text.data[(size_t)lengths[col]] = '\0';
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

/* Open a connection for a parsed connection string. reconnect enables
 * MYSQL_OPT_RECONNECT; timeout_sec bounds connect (and, for short-lived
 * side connections without reconnect, reads and writes too). */
static MYSQL *mysql_open(const ConnString *cs, unsigned int timeout_sec,
                         bool reconnect, char **err) {
  MYSQL *mysql = mysql_init(NULL);
  if (!mysql) {
    err_set(err, "Failed to initialize MySQL connection");
    return NULL;
  }

  if (mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec) != 0) {
    mysql_close(mysql);
    err_set(err, "Failed to set connection timeout");
    return NULL;
  }

  if (!reconnect) {
    /* Side connections must not hang on a dead server */
    mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, &timeout_sec);
    mysql_options(mysql, MYSQL_OPT_WRITE_TIMEOUT, &timeout_sec);
  }

  /* Never reconnect behind our back: a silent reconnect drops the open
   * transaction, temp tables and session settings, and the next COMMIT
   * "succeeds" as a no-op. mysql_driver_query reconnects only when no
   * transaction was open. */
  (void)reconnect;
  my_bool reconnect_opt = 0;
  if (mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect_opt) != 0) {
    mysql_close(mysql);
    err_set(err, "Failed to set reconnect option");
    return NULL;
  }

  /* Set character set */
  if (mysql_options(mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
    mysql_close(mysql);
    err_set(err, "Failed to set character set");
    return NULL;
  }

  const char *host = cs->host ? cs->host : "localhost";
  int port = cs->port > 0 ? cs->port : 3306;
  const char *user = cs->user ? cs->user : "root";
  const char *database = cs->database ? cs->database : "mysql";

  /* CLIENT_FOUND_ROWS: affected rows count matched rows, so an UPDATE that
   * sets a value it already had still reports 1 (lets callers tell "row
   * not found" from "nothing changed"). CLIENT_MULTI_STATEMENTS: a string
   * of several statements runs them all, as on SQLite and PostgreSQL
   * (mysql_driver_query reads every result). */
  /* Through an SSH tunnel: its Unix socket (host "localhost" makes the
   * client use it) */
  const char *tunnel_socket = connstr_get_option(cs, "lace_socket");
  if (tunnel_socket)
    host = "localhost";
  if (!mysql_real_connect(mysql, host, user, cs->password, database, port,
                          tunnel_socket,
                          CLIENT_FOUND_ROWS | CLIENT_MULTI_STATEMENTS)) {
    err_setf(err, "Connection failed: %s", mysql_error(mysql));
    mysql_close(mysql);
    return NULL;
  }
  return mysql;
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

  const char *host = cs->host ? cs->host : "localhost";
  int port = cs->port > 0 ? cs->port : 3306;
  const char *user = cs->user ? cs->user : "root";
  const char *database = cs->database ? cs->database : "mysql";

  MYSQL *mysql = mysql_open(cs, LACED_MYSQL_CONNECT_TIMEOUT_S, true, err);
  if (!mysql) {
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

/* Collect the rows of the current (streamed) result set; frees result */
static ResultSet *mysql_collect_rows(DbConnection *conn, MySqlData *data,
                                    MYSQL_RES *result, bool may_cancel,
                                    char **err) {
  ResultSet *rs = db_result_alloc_empty();

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
    rs->columns[i].type =
        mysql_type_to_db_type(fields[i].type, fields[i].charsetnr);
    rs->columns[i].nullable = !(fields[i].flags & NOT_NULL_FLAG);
    rs->columns[i].primary_key = (fields[i].flags & PRI_KEY_FLAG) != 0;
  }

  /* Get rows (count unknown while streaming) */
  DbRowCollector rows;
  db_rows_init(&rows, rs);
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    unsigned long *lengths = mysql_fetch_lengths(result);
    if (!lengths) {
      /* mysql_fetch_lengths can return NULL on error */
      mysql_free_result(result);
      db_result_free(rs);
      err_set(err, "Failed to get field lengths");
      return NULL;
    }
    DbValue *cells = db_rows_add(&rows, num_fields);
    if (!cells) {
      /* Limit reached: stop the server too (freeing a streamed result
       * otherwise reads every remaining row), unless more statements
       * follow: then the rest is read and dropped */
      void *kill = may_cancel ? mysql_driver_prepare_cancel(conn) : NULL;
      if (kill) {
        char *kerr = NULL;
        mysql_driver_cancel_query(conn, kill, &kerr);
        free(kerr);
        mysql_driver_free_cancel_handle(kill);
      }
      break;
    }
    for (unsigned int i = 0; i < num_fields; i++) {
      cells[i] = mysql_get_value(row, lengths, i, &fields[i]);
      db_rows_count(&rows, lengths[i]);
    }
  }

  /* A NULL row is the end, or a failure mid-stream (not after our kill) */
  if (!rs->truncated && mysql_errno(data->mysql) != 0) {
    err_set(err, mysql_error(data->mysql));
    mysql_free_result(result);
    db_result_free(rs);
    return NULL;
  }

  mysql_free_result(result);
  return rs;
}

#define MYSQL_TX_LOST_MSG                                                      \
  "Connection to the MySQL server was lost; the open transaction was rolled " \
  "back by the server. Run ROLLBACK to acknowledge, then redo the work."

static bool mysql_reopen(DbConnection *conn, MySqlData *data, char **err) {
  ConnString *cs = connstr_parse(conn->connstr, err);
  if (!cs) {
    return false;
  }
  MYSQL *fresh = mysql_open(cs, LACED_MYSQL_CONNECT_TIMEOUT_S, true, err);
  connstr_free(cs);
  if (!fresh) {
    return false;
  }
  mysql_close(data->mysql);
  data->mysql = fresh;
  db_link_restored(&data->link);
  return true;
}

/* After a failed statement: was the connection itself lost? (DbLinkState
 * decides what the next statement does) */
static void mysql_note_failure(MySqlData *data, bool had_tx, unsigned int e) {
  if (e == CR_SERVER_GONE_ERROR || e == CR_SERVER_LOST)
    db_link_lost(&data->link, had_tx);
}

/* Before a statement: make sure there is a live connection to run it on.
 * A connection idle for a while is pinged first (wait_timeout, server
 * restart); a lost one is reopened only when no transaction was open. */
static bool mysql_ensure_alive(DbConnection *conn, MySqlData *data,
                               const char *sql, bool *answered, char **err) {
  *answered = false;
  time_t now = time(NULL);
  bool idle = data->last_used != 0 && now - data->last_used > LACED_MYSQL_IDLE_PING_S;
  data->last_used = now;
  if (!data->link.broken && idle && mysql_ping(data->mysql) != 0)
    db_link_lost(&data->link,
                 (data->mysql->server_status & SERVER_STATUS_IN_TRANS) != 0);
  switch (db_link_gate(&data->link, sql, MYSQL_TX_LOST_MSG, err)) {
  case DB_LINK_REFUSE:
    return false;
  case DB_LINK_ACK:
    /* Nothing is left to roll back: the ROLLBACK is answered */
    *answered = mysql_reopen(conn, data, err);
    return *answered;
  case DB_LINK_RECONNECT:
    return mysql_reopen(conn, data, err);
  case DB_LINK_READY:
    break;
  }
  return true;
}

/* DbDriver hook: before a statement, and before its cancel handle (the
 * server thread id) is taken, so a reopened connection is the one KILL
 * QUERY addresses */
static bool mysql_driver_ensure_ready(DbConnection *conn, const char *sql,
                                      bool *answered, char **err) {
  *answered = false;
  DB_REQUIRE_PARAMS_CONN(sql, conn, MySqlData, data, mysql, err, false);
  return mysql_ensure_alive(conn, data, sql, answered, err);
}

/* Run sql and read every result it produces (CALL, multi-result statements):
 * leaving one unread breaks the connection with "Commands out of sync".
 * The last result that has rows (SELECT, CALL's SELECT, ... RETURNING),
 * else the affected-row count of the last statement (rows_affected). */
static ResultSet *mysql_driver_query(DbConnection *conn, const char *sql,
                                     char **err) {
  DB_REQUIRE_PARAMS_CONN(sql, conn, MySqlData, data, mysql, err, NULL);

  bool had_tx = (data->mysql->server_status & SERVER_STATUS_IN_TRANS) != 0;
  if (mysql_query(data->mysql, sql) != 0) {
    err_set(err, mysql_error(data->mysql));
    mysql_note_failure(data, had_tx, mysql_errno(data->mysql));
    if (data->link.tx_lost)
      err_set(err, MYSQL_TX_LOST_MSG);
    return NULL;
  }

  /* At the row/byte limit the rest of a lone statement that only reads is
   * killed (fast). Otherwise the rest of the result is read and dropped:
   * later statements of the string must still run, and killing a writing
   * statement (INSERT ... RETURNING) would undo its write while the reply
   * reports success. */
  bool may_cancel = lace_sql_single_statement(sql, LACE_DRIVER_MYSQL) &&
                    lace_sql_read_only(sql, LACE_DRIVER_MYSQL);
  ResultSet *rows = NULL;
  int64_t last_affected = 0;
  bool limited = false;
  char *error = NULL;
  unsigned int error_code = 0; /* mysql_next_result clears the client's */
  for (;;) {
    if (mysql_field_count(data->mysql) > 0) {
      /* Stream the rows (use_result), so the limits bound what is held */
      MYSQL_RES *result = mysql_use_result(data->mysql);
      if (!result) {
        if (!limited && !error) {
          error = str_dup(mysql_error(data->mysql));
          error_code = mysql_errno(data->mysql);
        }
      } else if (limited || error) {
        mysql_free_result(result); /* Drain */
      } else {
        char *cerr = NULL;
        ResultSet *rs = mysql_collect_rows(conn, data, result, may_cancel, &cerr);
        if (!rs) {
          error = cerr ? cerr : str_dup("Failed to read result");
          error_code = mysql_errno(data->mysql);
        } else {
          db_result_free(rows);
          rows = rs;
          limited = rs->truncated && may_cancel;
        }
      }
    } else if (!limited && !error) {
      last_affected = (int64_t)mysql_affected_rows(data->mysql);
    }
    int more = mysql_next_result(data->mysql);
    if (more != 0) {
      if (more > 0 && !limited && !error) {
        error = str_dup(mysql_error(data->mysql));
        error_code = mysql_errno(data->mysql);
      }
      break;
    }
  }

  if (error) {
    db_result_free(rows);
    mysql_note_failure(data, had_tx, error_code);
    err_set(err, data->link.tx_lost ? MYSQL_TX_LOST_MSG : error);
    free(error);
    return NULL;
  }
  if (rows) {
    return rows;
  }
  ResultSet *rs = db_result_alloc_empty();
  rs->rows_affected = last_affected;
  return rs;
}

static void mysql_driver_library_cleanup(void) {
  static bool cleaned_up = false;
  if (cleaned_up)
    return;
  cleaned_up = true;
  mysql_library_end();
}

/* mysql_library_init is not thread-safe: run it once before worker threads
 * start (connects run on workers) */
static void mysql_driver_library_init(void) { mysql_library_init(0, NULL, NULL); }

/* Cancel handle: the server thread running the query, plus what is needed to
 * reach the server on a separate connection. KILL QUERY cannot go over the
 * query's own connection, which is busy until the query returns. */
typedef struct {
  unsigned long thread_id;
  char *connstr;        /* Copy; may hold a password */
  atomic_bool finished; /* Query returned; a pending KILL must not be sent */
} MySqlCancelHandle;

static void *mysql_driver_prepare_cancel(DbConnection *conn) {
  if (!conn || !conn->connstr)
    return NULL;
  MySqlData *data = conn->driver_data;
  if (!data || !data->mysql)
    return NULL;

  char *connstr = str_dup(conn->connstr);
  if (!connstr)
    return NULL;

  MySqlCancelHandle *handle = safe_calloc(1, sizeof(MySqlCancelHandle));
  handle->thread_id = mysql_thread_id(data->mysql);
  handle->connstr = connstr;
  atomic_init(&handle->finished, false);
  return handle;
}

static bool mysql_driver_cancel_query(DbConnection *conn, void *cancel_handle,
                                      char **err) {
  (void)conn; /* Busy with the query on another thread; never touch it */
  if (!cancel_handle) {
    err_set(err, "Invalid parameters");
    return false;
  }

  MySqlCancelHandle *handle = (MySqlCancelHandle *)cancel_handle;
  if (atomic_load(&handle->finished) || handle->thread_id == 0)
    return true;

  ConnString *cs = connstr_parse(handle->connstr, err);
  if (!cs)
    return false;
  MYSQL *side = mysql_open(cs, LACED_MYSQL_CANCEL_TIMEOUT_S, false, err);
  connstr_free(cs);
  if (!side)
    return false;

  bool ok = true;
  /* Connecting is the slow part; re-check so a cancel that arrives as the
   * query finishes does not interrupt the connection's next statement. */
  if (!atomic_load(&handle->finished)) {
    char kill_sql[64];
    snprintf(kill_sql, sizeof(kill_sql), "KILL QUERY %lu", handle->thread_id);
    if (mysql_query(side, kill_sql) != 0) {
      /* 1094 ER_NO_SUCH_THREAD: the query already ended */
      if (mysql_errno(side) != 1094) {
        err_set(err, mysql_error(side));
        ok = false;
      }
    } else {
      mysql_consume_pending_results(side);
    }
  }

  mysql_close(side);
  return ok;
}

static void mysql_driver_cancel_handle_finished(void *cancel_handle) {
  if (cancel_handle)
    atomic_store(&((MySqlCancelHandle *)cancel_handle)->finished, true);
}

static void mysql_driver_free_cancel_handle(void *cancel_handle) {
  MySqlCancelHandle *handle = (MySqlCancelHandle *)cancel_handle;
  if (!handle)
    return;
  str_secure_free(handle->connstr);
  free(handle);
}

static void mysql_driver_connection_lost(DbConnection *conn) {
  MySqlData *data = conn ? conn->driver_data : NULL;
  if (!data || !data->mysql)
    return;
  db_link_lost(&data->link,
               !data->link.broken &&
                   (data->mysql->server_status & SERVER_STATUS_IN_TRANS));
}

static int mysql_driver_transaction_state(DbConnection *conn) {
  MySqlData *data = conn ? conn->driver_data : NULL;
  if (data && data->link.tx_lost)
    return 1; /* Until ROLLBACK acknowledges the loss */
  return data && data->mysql && !data->link.broken &&
                 (data->mysql->server_status & SERVER_STATUS_IN_TRANS)
             ? 1
             : 0;
}
