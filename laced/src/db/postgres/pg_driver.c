/*
 * Lace
 * PostgreSQL driver - uses libpq C API
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
#include "../../log.h"
#include <ctype.h>
#include <errno.h>
#include <libpq-fe.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* PostgreSQL connection data */
typedef struct {
  PGconn *conn;
  char *database;
  DbLinkState link; /* Lost connection and lost transaction */
  bool had_tx;      /* A transaction was open after the last statement */
} PgData;

#define PG_TX_LOST_MSG                                                         \
  "Connection to the PostgreSQL server was lost; the open transaction was "    \
  "rolled back. Run ROLLBACK to acknowledge, then redo the work."

static void pg_notice_to_log(void *arg, const char *message) {
  (void)arg;
  LOG_DEBUG("PostgreSQL: %.*s", (int)strcspn(message, "\n"), message);
}

/* Forward declarations */
static DbConnection *pg_connect(const char *connstr, char **err);
static void pg_disconnect(DbConnection *conn);
static ResultSet *pg_query(DbConnection *conn, const char *sql, char **err);
static bool pg_ensure_ready(DbConnection *conn, const char *sql,
                            bool *answered, char **err);
static void *pg_prepare_cancel(DbConnection *conn);
static bool pg_cancel_query(DbConnection *conn, void *cancel_handle,
                            char **err);
static void pg_free_cancel_handle(void *cancel_handle);
static int pg_transaction_state(DbConnection *conn);
static void pg_connection_lost(DbConnection *conn);

/* Driver definition */
DbDriver postgres_driver = {
    .name = "postgres",
    .display_name = "PostgreSQL",
    .connect = pg_connect,
    .disconnect = pg_disconnect,
    .query = pg_query,
    .ensure_ready = pg_ensure_ready,
    .prepare_cancel = pg_prepare_cancel,
    .cancel_query = pg_cancel_query,
    .free_cancel_handle = pg_free_cancel_handle,
    .transaction_state = pg_transaction_state,
    .connection_lost = pg_connection_lost,
    .library_cleanup = NULL,
};

/* Parameter value for PQexecParams - tracks allocated buffer separately */
typedef struct {
  const char *value; /* Pointer for paramValues array (NULL for SQL NULL) */
  char *allocated;   /* Buffer to free (NULL if pointing to text.data) */
  int length;        /* Length for paramLengths array */
} PgParamValue;

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
    return DB_TYPE_FLOAT;

  case 1700: /* numeric: exact, any precision; a double would round it */
    return DB_TYPE_DECIMAL;

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

  /* Validate value pointer and length */
  if (!value || len < 0) {
    val.type = DB_TYPE_NULL;
    val.is_null = true;
    return val;
  }

  /* For oversized fields, show placeholder text instead of loading data */
  if ((size_t)len > LACE_MAX_FIELD_SIZE) {
    return db_value_oversized_placeholder("DATA", (size_t)len);
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
      val.text.data = safe_malloc(len + 1);
      val.type = DB_TYPE_TEXT;
      val.text.len = len;
      memcpy(val.text.data, value, len);
      val.text.data[len] = '\0';
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
      val.text.data = safe_malloc(len + 1);
      val.type = DB_TYPE_TEXT;
      val.text.len = len;
      memcpy(val.text.data, value, len);
      val.text.data[len] = '\0';
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
        val.blob.data = safe_malloc(len);
        val.type = DB_TYPE_BLOB;
        memcpy(val.blob.data, value, len);
        val.blob.len = len;
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
        val.blob.data = safe_malloc(len);
        val.type = DB_TYPE_BLOB;
        memcpy(val.blob.data, value, len);
        val.blob.len = len;
        break;
      }
      val.blob.data = safe_malloc(hex_len);
      val.type = DB_TYPE_BLOB;
      val.blob.len = hex_len;
      for (size_t i = 0; i < hex_len; i++) {
        /* Convert two hex chars to byte - already validated above */
        char hex_pair[3] = {value[2 + i * 2], value[2 + i * 2 + 1], '\0'};
        val.blob.data[i] = (uint8_t)strtoul(hex_pair, NULL, 16);
      }
    } else {
      /* Escape format (bytea_output = escape): \ooo octets and \\ */
      size_t n = 0;
      unsigned char *bytes = PQunescapeBytea((const unsigned char *)value, &n);
      val.type = DB_TYPE_BLOB;
      val.blob.data = safe_malloc(n ? n : 1);
      if (bytes)
        memcpy(val.blob.data, bytes, n);
      val.blob.len = bytes ? n : 0;
      PQfreemem(bytes);
    }
    break;

  case DB_TYPE_DECIMAL:
    val.text.data = safe_malloc(len + 1);
    val.type = DB_TYPE_DECIMAL;
    val.text.len = len;
    memcpy(val.text.data, value, len);
    val.text.data[len] = '\0';
    break;

  default:
    val.text.data = safe_malloc(len + 1);
    val.type = DB_TYPE_TEXT;
    val.text.len = len;
    memcpy(val.text.data, value, len);
    val.text.data[len] = '\0';
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
    err_set(err, "Not a PostgreSQL connection string");
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

  /* Through an SSH tunnel: its socket's directory (libpq connects to
   * <dir>/.s.PGSQL.<port>) */
  char *socket_dir = NULL;
  const char *tunnel_socket = connstr_get_option(cs, "lace_socket");
  if (tunnel_socket && strrchr(tunnel_socket, '/')) {
    socket_dir = str_ndup(tunnel_socket,
                          (size_t)(strrchr(tunnel_socket, '/') - tunnel_socket));
    host = socket_dir;
  }

  /* Use PQconnectdbParams which handles escaping of special characters.
   * connect_timeout bounds connects and PQreset (an unreachable host would
   * otherwise wait for the OS TCP timeout, with no way to cancel);
   * client_encoding as a parameter survives PQreset, a SET would not */
  char timeout_str[16];
  snprintf(timeout_str, sizeof(timeout_str), "%d", LACED_PG_CONNECT_TIMEOUT_S);
  const char *keywords[8];
  const char *values[8];
  int idx = 0;

  keywords[idx] = "host";
  values[idx++] = host;
  keywords[idx] = "connect_timeout";
  values[idx++] = timeout_str;
  keywords[idx] = "client_encoding";
  values[idx++] = "UTF8";
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
    err_setf(err, "Connection failed: %s", PQerrorMessage(pgconn));
    PQfinish(pgconn);
    free(socket_dir);
    connstr_free(cs);
    return NULL;
  }

  /* Set client encoding to UTF-8 */
  PQsetClientEncoding(pgconn, "UTF8");
  /* Server notices go to the daemon log, not libpq's default stderr
   * (kept by PQreset) */
  PQsetNoticeProcessor(pgconn, pg_notice_to_log, NULL);

  PgData *data = safe_calloc(1, sizeof(PgData));
  data->conn = pgconn;
  data->database = str_dup(database);

  DbConnection *conn = safe_calloc(1, sizeof(DbConnection));

  conn->driver = &postgres_driver;
  conn->connstr = str_dup(connstr);
  conn->database = str_dup(database);
  conn->host = str_dup(host);
  conn->port = port;
  conn->user = str_dup(user);
  conn->driver_data = data;

  free(socket_dir); /* host may point into it until here */
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

  db_common_free_connection(conn);
}

/* Append the rows of res to the collector's result (columns are taken from
 * res if it has none), honouring the limits. Returns false once a limit was
 * hit. */
static bool pg_append_rows(DbRowCollector *rows, PGresult *res) {
  ResultSet *rs = rows->rs;
  int num_fields = PQnfields(res);
  if (num_fields < 0) {
    num_fields = 0;
  }
  if (!rs->columns && num_fields > 0) {
    rs->num_columns = (size_t)num_fields;
    rs->columns = safe_calloc((size_t)num_fields, sizeof(ColumnDef));
    for (int i = 0; i < num_fields; i++) {
      const char *fname = PQfname(res, i);
      rs->columns[i].name = fname ? str_dup(fname) : str_dup("?");
      rs->columns[i].type = pg_oid_to_db_type(PQftype(res, i));
    }
  }
  int n = PQntuples(res);
  for (int r = 0; r < n; r++) {
    DbValue *cells = db_rows_add(rows, (size_t)num_fields);
    if (!cells)
      return false;
    for (int c = 0; c < num_fields; c++) {
      cells[c] = pg_get_value(res, r, c, PQftype(res, c));
      db_rows_count(rows, (size_t)PQgetlength(res, r, c));
    }
  }
  return true;
}

/* Run sql and collect rows one at a time (single-row mode), so a huge
 * result is never held whole in libpq's memory, and stop at the row/byte
 * limits instead of after the fact. With several statements: the last one
 * that produced rows (SELECT, ... RETURNING), else the row count of the last
 * statement. A statement without rows returns an empty result carrying its
 * row count (rows_affected), so callers decide by shape, not by keyword. */
static ResultSet *pg_query_run(PgData *data, const char *sql, char **err);

static void pg_connection_lost(DbConnection *conn) {
  PgData *data = conn ? conn->driver_data : NULL;
  if (!data)
    return;
  db_link_lost(&data->link, data->had_tx);
}

/* PQreset, for a connection lost or found dead */
static bool pg_reset(PgData *data, char **err) {
  PQreset(data->conn);
  if (PQstatus(data->conn) != CONNECTION_OK) {
    data->link.broken = true;
    err_setf(err, "Reconnect failed: %s", PQerrorMessage(data->conn));
    return false;
  }
  db_link_restored(&data->link);
  data->had_tx = false;
  return true;
}

/* Before a statement: a lost connection (server restart, network, SSH
 * tunnel) is reset here, unless a transaction went with it (DbLinkState).
 * Runs before the cancel handle is taken, so the handle addresses the
 * session the statement runs on. */
static bool pg_ensure_ready(DbConnection *conn, const char *sql,
                            bool *answered, char **err) {
  *answered = false;
  DB_REQUIRE_PARAMS_CONN(sql, conn, PgData, data, conn, err, false);

  if (PQstatus(data->conn) != CONNECTION_OK)
    db_link_lost(&data->link, data->had_tx);
  switch (db_link_gate(&data->link, sql, PG_TX_LOST_MSG, err)) {
  case DB_LINK_REFUSE:
    return false;
  case DB_LINK_ACK:
    /* Nothing is left to roll back: the ROLLBACK is answered */
    *answered = pg_reset(data, err);
    return *answered;
  case DB_LINK_RECONNECT:
    return pg_reset(data, err);
  case DB_LINK_READY:
    break;
  }
  return true;
}

/* Run sql (after pg_ensure_ready). A connection lost during the statement
 * fails it; the next pg_ensure_ready resets it. */
static ResultSet *pg_query(DbConnection *conn, const char *sql, char **err) {
  DB_REQUIRE_PARAMS_CONN(sql, conn, PgData, data, conn, err, NULL);

  ResultSet *rs = pg_query_run(data, sql, err);
  if (PQstatus(data->conn) != CONNECTION_OK) {
    db_link_lost(&data->link, data->had_tx);
    if (data->link.tx_lost) {
      err_set(err, PG_TX_LOST_MSG);
      db_result_free(rs);
      return NULL;
    }
  }
  data->had_tx = PQtransactionStatus(data->conn) != PQTRANS_IDLE &&
                 PQtransactionStatus(data->conn) != PQTRANS_UNKNOWN;
  return rs;
}

static ResultSet *pg_query_run(PgData *data, const char *sql, char **err) {
  if (!PQsendQuery(data->conn, sql)) {
    err_set(err, PQerrorMessage(data->conn));
    return NULL;
  }
  PQsetSingleRowMode(data->conn); /* Applies to the first statement */

  /* At the row/byte limit, a lone statement outside a transaction is
   * cancelled (fast, and nothing else is affected). Otherwise the rest of
   * that result is read and dropped: a cancel would fail the user's
   * transaction, or roll back the other statements of the string (one
   * implicit transaction) and skip the later ones while reporting success. */
  /* ...and a statement that writes (INSERT ... RETURNING, a data-modifying
   * WITH) is never cancelled: that would roll its write back */
  bool may_cancel = !data->had_tx &&
                    lace_sql_single_statement(sql, LACE_DRIVER_POSTGRES) &&
                    lace_sql_read_only(sql, LACE_DRIVER_POSTGRES);
  ResultSet *rows = NULL;  /* Last complete row result */
  ResultSet *cur = NULL;   /* Rows of the statement being read */
  int64_t last_affected = 0;
  DbRowCollector collector = {0};
  bool limited = false;    /* Cancelled at a limit: discard the rest */
  bool skipping = false;   /* At a limit: drop this statement's other rows */
  bool commit_rolled_back = false;
  char *error = NULL;

  PGresult *res;
  while ((res = PQgetResult(data->conn)) != NULL) {
    ExecStatusType status = PQresultStatus(res);
    if (limited || error) {
      PQclear(res); /* Drain what is left */
      continue;
    }
    if (status == PGRES_FATAL_ERROR || status == PGRES_BAD_RESPONSE ||
        status == PGRES_NONFATAL_ERROR) {
      const char *msg = PQresultErrorMessage(res);
      error = str_dup(msg && msg[0] ? msg : PQerrorMessage(data->conn));
      PQclear(res);
      continue;
    }
    if (skipping && (status == PGRES_SINGLE_TUPLE || status == PGRES_TUPLES_OK)) {
      if (status == PGRES_TUPLES_OK)
        skipping = false; /* That statement is done: the next one counts */
      PQclear(res);
      continue;
    }
    if (status == PGRES_SINGLE_TUPLE || status == PGRES_TUPLES_OK) {
      if (!cur) {
        cur = db_result_alloc_empty();
        db_rows_init(&collector, cur);
      }
      bool full = !pg_append_rows(&collector, res);
      if (full && may_cancel) {
        /* Limit reached: stop the server instead of reading the rest */
        limited = true;
        PGcancel *cancel = PQgetCancel(data->conn);
        if (cancel) {
          char ebuf[256];
          PQcancel(cancel, ebuf, sizeof(ebuf));
          PQfreeCancel(cancel);
        }
      } else if (full && status != PGRES_TUPLES_OK) {
        skipping = true;
      }
      if (status == PGRES_TUPLES_OK || full) {
        /* This statement's rows are complete */
        db_result_free(rows);
        rows = cur;
        cur = NULL;
      }
    } else {
      /* PGRES_COMMAND_OK and friends: no rows, a row count */
      const char *n = PQcmdTuples(res);
      last_affected = (n && *n) ? strtoll(n, NULL, 10) : 0;
      const char *tag = PQcmdStatus(res);
      if (tag && strcmp(tag, "ROLLBACK") == 0 &&
          (db_common_sql_starts_with_word(sql, "COMMIT") || db_common_sql_starts_with_word(sql, "END"))) {
        /* COMMIT of a transaction that had already failed: the server
         * rolled it back and says so only in the command tag */
        commit_rolled_back = true;
      }
    }
    PQclear(res);
  }
  if (cur) {
    db_result_free(rows);
    rows = cur;
  }

  if (error || commit_rolled_back) {
    db_result_free(rows);
    err_set(err, error ? error
                       : "COMMIT did not commit: the transaction had failed "
                         "earlier and was rolled back");
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
    err_set(err, "Invalid cancel handle");
    return false;
  }

  PGcancel *cancel = (PGcancel *)cancel_handle;
  char errbuf[256];

  if (PQcancel(cancel, errbuf, sizeof(errbuf)) == 0) {
    err_set(err, errbuf);
    return false;
  }
  return true;
}

static void pg_free_cancel_handle(void *cancel_handle) {
  if (cancel_handle) {
    PQfreeCancel((PGcancel *)cancel_handle);
  }
}

static int pg_transaction_state(DbConnection *conn) {
  PgData *data = conn ? conn->driver_data : NULL;
  if (!data || !data->conn) {
    return 0;
  }
  if (data->link.tx_lost) {
    return 1; /* Until ROLLBACK acknowledges the loss */
  }
  switch (PQtransactionStatus(data->conn)) {
  case PQTRANS_INTRANS:
  case PQTRANS_ACTIVE:
    return 1;
  case PQTRANS_INERROR:
    return 2;
  default:
    return 0;
  }
}
