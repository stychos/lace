/*
 * liblace - Lace Client Library
 * Client implementation - daemon spawning and IPC
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../include/lace.h"
#include "../include/util/connstr.h"
#include "../include/util/mem.h"
#include "../include/util/str.h"
#include "rpc.h"
#include "client_internal.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Default timeout in milliseconds */
#define DEFAULT_TIMEOUT_MS 30000

/* Socket utilities (implemented in socket.c) */

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/* Per-thread last error. A shared client->last_error was freed by one
 * thread while another was copying it. */
static pthread_key_t g_error_key;
static pthread_once_t g_error_once = PTHREAD_ONCE_INIT;

static void error_key_init(void) {
  pthread_key_create(&g_error_key, free);
}

/* Per-thread copy of the last SQL statement sent by lace_exec() */
static pthread_key_t g_sql_key;
static pthread_once_t g_sql_once = PTHREAD_ONCE_INIT;

/* The SQL may hold secrets (cell values, ALTER USER ... PASSWORD): wipe it */
static void sql_free(void *sql) { str_secure_free(sql); }

static void sql_key_init(void) {
  pthread_key_create(&g_sql_key, sql_free);
}

static void set_thread_last_sql(const char *sql) {
  pthread_once(&g_sql_once, sql_key_init);
  str_secure_free(pthread_getspecific(g_sql_key));
  pthread_setspecific(g_sql_key, sql ? str_dup(sql) : NULL);
}

const char *lace_last_sql(void) {
  pthread_once(&g_sql_once, sql_key_init);
  return pthread_getspecific(g_sql_key);
}

/* Per-thread transaction state reported with the last lace_exec() reply,
 * stored offset by 2 so that "unset" (NULL) reads as -1 */
static pthread_key_t g_tx_key;
static pthread_once_t g_tx_once = PTHREAD_ONCE_INIT;

static void tx_key_init(void) {
  pthread_key_create(&g_tx_key, NULL);
}

void lace_set_thread_tx_state(int state) {
  pthread_once(&g_tx_once, tx_key_init);
  pthread_setspecific(g_tx_key, (void *)(intptr_t)(state + 2));
}

int lace_last_transaction_state(void) {
  pthread_once(&g_tx_once, tx_key_init);
  intptr_t v = (intptr_t)pthread_getspecific(g_tx_key);
  return v == 0 ? -1 : (int)(v - 2);
}

/* Per-thread "I am using conn_ids from generation N of client C" */
typedef struct {
  const lace_client_t *client;
  uint64_t generation;
} ExpectedGeneration;

static pthread_key_t g_expect_key;
static pthread_once_t g_expect_once = PTHREAD_ONCE_INIT;

static void expect_key_init(void) {
  pthread_key_create(&g_expect_key, free);
}

void lace_expect_generation(lace_client_t *client, uint64_t generation) {
  pthread_once(&g_expect_once, expect_key_init);
  ExpectedGeneration *e = pthread_getspecific(g_expect_key);
  if (!e) {
    e = safe_calloc(1, sizeof(*e));
    pthread_setspecific(g_expect_key, e);
  }
  e->client = client;
  e->generation = generation;
}

bool lace_thread_generation_mismatch(const lace_client_t *client) {
  pthread_once(&g_expect_once, expect_key_init);
  const ExpectedGeneration *e = pthread_getspecific(g_expect_key);
  return e && e->client == client && e->generation != client->generation;
}

void lace_set_thread_error(const char *msg) {
  pthread_once(&g_error_once, error_key_init);
  free(pthread_getspecific(g_error_key));
  pthread_setspecific(g_error_key, msg ? str_dup(msg) : NULL);
}

/* Set error message */
/* Record why a call failed (msg NULL: the code's standard text) and return
 * the code: every failing public call leaves a message for
 * lace_client_error() */
static int lace_fail(int code, const char *msg) {
  lace_set_thread_error(msg ? msg : lace_error_message(code));
  return code;
}

#define NOT_CONNECTED_MSG "Not connected to the lace daemon"

static void set_error(lace_client_t *client, const char *msg) {
  (void)client;
  lace_set_thread_error(msg);
}

/* Map a driver name (connection string scheme or the daemon's name) to the
 * dialect. Same names connstr.c and the daemon accept ("pg" fell through to
 * SQLite once: sqlite_master queries and SQLite literals on PostgreSQL).
 * Returns false for a name it does not know. */
static bool driver_from_name(const char *name, LaceDriver *out) {
  if (str_eq(name, "sqlite")) {
    *out = LACE_DRIVER_SQLITE;
  } else if (str_eq(name, "postgres") || str_eq(name, "postgresql") ||
             str_eq(name, "pg")) {
    *out = LACE_DRIVER_POSTGRES;
  } else if (str_eq(name, "mysql")) {
    *out = LACE_DRIVER_MYSQL;
  } else if (str_eq(name, "mariadb")) {
    *out = LACE_DRIVER_MARIADB;
  } else {
    return false;
  }
  return true;
}

/* Parse driver type from connection string */
static LaceDriver parse_driver(const char *connstr) {
  ConnString *cs = connstr_parse(connstr, NULL);
  LaceDriver driver = LACE_DRIVER_SQLITE; /* A bare file path is SQLite */
  if (cs) {
    driver_from_name(cs->driver, &driver);
    connstr_free(cs);
  }
  return driver;
}

/* Track a new connection */
/* The connection table is used from the TUI's main and worker threads, and
 * reset when the daemon is replaced: always under client->lock. */
static void track_connection(lace_client_t *client, int conn_id, LaceDriver driver) {
  pthread_mutex_lock(&client->lock);
  for (size_t i = 0; i < client->num_connections; i++) {
    if (client->connections[i].conn_id == conn_id) {
      client->connections[i].driver = driver; /* Daemon reused the id */
      pthread_mutex_unlock(&client->lock);
      return;
    }
  }
  if (client->num_connections == LACE_CLIENT_MAX_CONNECTIONS) {
    /* Full: forget the oldest entry; a lookup asks the daemon again */
    memmove(&client->connections[0], &client->connections[1],
            (LACE_CLIENT_MAX_CONNECTIONS - 1) * sizeof(client->connections[0]));
    client->num_connections--;
  }
  client->connections[client->num_connections].conn_id = conn_id;
  client->connections[client->num_connections].driver = driver;
  client->num_connections++;
  pthread_mutex_unlock(&client->lock);
}

/* Remove a tracked connection */
static void untrack_connection(lace_client_t *client, int conn_id) {
  pthread_mutex_lock(&client->lock);
  for (size_t i = 0; i < client->num_connections; i++) {
    if (client->connections[i].conn_id == conn_id) {
      /* Shift remaining entries */
      for (size_t j = i; j + 1 < client->num_connections; j++) {
        client->connections[j] = client->connections[j + 1];
      }
      client->num_connections--;
      break;
    }
  }
  pthread_mutex_unlock(&client->lock);
}

/* Get the SQL dialect of a connection. Connections this client opened are
 * cached; any other (opened by another client of the shared daemon, or
 * evicted from the cache) is looked up in the daemon's connection list.
 * Guessing would quote and build literals for the wrong database. */
static int get_connection_driver(lace_client_t *client, int conn_id,
                                 LaceDriver *out) {
  bool found = false;
  pthread_mutex_lock(&client->lock);
  for (size_t i = 0; i < client->num_connections; i++) {
    if (client->connections[i].conn_id == conn_id) {
      *out = client->connections[i].driver;
      found = true;
      break;
    }
  }
  pthread_mutex_unlock(&client->lock);
  if (found) {
    return LACE_OK;
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "connections", NULL, &result);
  if (err != LACE_OK) {
    return err;
  }
  cJSON *item = NULL;
  cJSON_ArrayForEach(item, result) {
    cJSON *id = cJSON_GetObjectItem(item, "id");
    cJSON *driver = cJSON_GetObjectItem(item, "driver");
    if (cJSON_IsNumber(id) && id->valueint == conn_id &&
        cJSON_IsString(driver) && driver_from_name(driver->valuestring, out)) {
      found = true;
      break;
    }
  }
  cJSON_Delete(result);
  if (!found) {
    set_error(client, "Unknown connection id");
    return LACE_ERR_INVALID_CONN_ID;
  }
  track_connection(client, conn_id, *out);
  return LACE_OK;
}

char *lace_quote_identifier(LaceDriver driver, const char *name) {
  /* MySQL/MariaDB quote with backticks, PostgreSQL/SQLite with double
   * quotes; a quote inside the name is doubled */
  return driver == LACE_DRIVER_MYSQL || driver == LACE_DRIVER_MARIADB
             ? str_escape_identifier_backtick(name)
             : str_escape_identifier_dquote(name);
}

/* A reference part needs quoting when it would not parse back as bare */
static bool ref_part_needs_quotes(const char *part) {
  return !part[0] || strchr(part, '.') || strchr(part, '"');
}

static void ref_append_part(StringBuilder *sb, const char *part) {
  if (!ref_part_needs_quotes(part)) {
    sb_append(sb, part);
    return;
  }
  sb_append_char(sb, '"');
  for (const char *c = part; *c; c++) {
    if (*c == '"')
      sb_append_char(sb, '"');
    sb_append_char(sb, *c);
  }
  sb_append_char(sb, '"');
}

char *lace_table_ref(const char *schema, const char *name) {
  if (!name)
    return NULL;
  StringBuilder *sb = sb_new(strlen(name) + (schema ? strlen(schema) : 0) + 8);
  if (!sb)
    return NULL;
  if (schema) {
    ref_append_part(sb, schema);
    sb_append_char(sb, '.');
  }
  ref_append_part(sb, name);
  return sb_finish(sb);
}

/* Parse one reference part at *p into a new string and advance *p past it.
 * Returns NULL on a malformed part. */
static char *ref_parse_part(const char **p) {
  const char *s = *p;
  StringBuilder *sb = sb_new(32);
  if (!sb)
    return NULL;
  if (*s == '"') {
    s++;
    for (;;) {
      if (!*s) {
        sb_free(sb);
        return NULL; /* Unterminated quote */
      }
      if (*s == '"') {
        if (s[1] != '"')
          break;
        s++; /* Doubled quote */
      }
      sb_append_char(sb, *s++);
    }
    s++; /* Closing quote */
  } else {
    if (!*s || *s == '.') {
      sb_free(sb);
      return NULL;
    }
    while (*s && *s != '.' && *s != '"')
      sb_append_char(sb, *s++);
  }
  *p = s;
  return sb_finish(sb);
}

/* Split a PostgreSQL reference. *schema is NULL for an unqualified one.
 * A string that does not parse is taken as a plain table name, so names
 * saved before references existed keep working. */
static bool ref_split(const char *ref, char **schema, char **name) {
  *schema = NULL;
  *name = NULL;
  const char *p = ref;
  char *first = ref_parse_part(&p);
  if (first && !*p) {
    *name = first;
    return true;
  }
  if (first && *p == '.') {
    p++;
    char *second = ref_parse_part(&p);
    if (second && !*p) {
      *schema = first;
      *name = second;
      return true;
    }
    free(second);
  }
  free(first);
  *name = str_dup(ref);
  return *name != NULL;
}

char *lace_quote_table(LaceDriver driver, const char *ref) {
  if (!ref)
    return NULL;
  if (driver != LACE_DRIVER_POSTGRES)
    return lace_quote_identifier(driver, ref);

  char *schema = NULL, *name = NULL;
  if (!ref_split(ref, &schema, &name))
    return NULL;
  char *qname = lace_quote_identifier(driver, name);
  char *result = NULL;
  if (schema) {
    char *qschema = lace_quote_identifier(driver, schema);
    if (qschema && qname)
      result = str_printf("%s.%s", qschema, qname);
    free(qschema);
    free(qname);
  } else {
    result = qname;
  }
  free(schema);
  free(name);
  return result;
}


/* Encode a string as a SQL literal for the given dialect (caller frees).
 * Single quotes are doubled, which is valid in every dialect and every
 * MySQL sql_mode. Backslash is the problem: MySQL treats it as an escape
 * unless NO_BACKSLASH_ESCAPES is set, so no quoted form is right for both
 * modes. MySQL strings containing a backslash are therefore sent as a
 * character-set-introduced hex literal (_utf8mb4 X'..'), which means the
 * same thing in every mode and still converts like a string in numeric
 * context. PostgreSQL has the same problem with standard_conforming_strings
 * off, so its strings containing a backslash use the E'' form, where the
 * backslash is always an escape and is doubled. Embedded NUL bytes cannot be
 * represented and end the value. */
static char *sql_string_literal(const char *value, LaceDriver driver) {
  if (!value) {
    return str_dup("NULL");
  }
  bool mysql = driver == LACE_DRIVER_MYSQL || driver == LACE_DRIVER_MARIADB;
  if (mysql && strchr(value, '\\')) {
    static const char hex[] = "0123456789abcdef";
    size_t len = strlen(value);
    StringBuilder *hb = sb_new(len * 2 + 16);
    if (!hb) {
      return NULL;
    }
    sb_append(hb, "_utf8mb4 X'");
    for (size_t i = 0; i < len; i++) {
      unsigned char ch = (unsigned char)value[i];
      sb_append_char(hb, hex[ch >> 4]);
      sb_append_char(hb, hex[ch & 0x0f]);
    }
    sb_append_char(hb, '\'');
    return sb_finish(hb);
  }
  bool pg_escape = driver == LACE_DRIVER_POSTGRES && strchr(value, '\\');
  StringBuilder *sb = sb_new(strlen(value) + 4);
  if (!sb) {
    return NULL;
  }
  if (pg_escape) {
    sb_append_char(sb, 'E');
  }
  sb_append_char(sb, '\'');
  for (const char *p = value; *p; p++) {
    if (*p == '\'' || (pg_escape && *p == '\\')) {
      sb_append_char(sb, *p);
    }
    sb_append_char(sb, *p);
  }
  sb_append_char(sb, '\'');
  return sb_finish(sb);
}

/* Past whitespace and -- / # / block comments */
static const char *skip_space_comments(const char *p) {
  for (;;) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f')
      p++;
    if ((p[0] == '-' && p[1] == '-') || p[0] == '#') {
      while (*p && *p != '\n')
        p++;
    } else if (p[0] == '/' && p[1] == '*') {
      const char *end = strstr(p + 2, "*/");
      p = end ? end + 2 : p + strlen(p);
    } else {
      return p;
    }
  }
}

/* The word at p, case-insensitively, followed by a non-word character */
static const char *match_word(const char *p, const char *word) {
  size_t n = strlen(word);
  if (strncasecmp(p, word, n) != 0)
    return NULL;
  char c = p[n];
  if (isalnum((unsigned char)c) || c == '_' || c == '$')
    return NULL;
  return p + n;
}

bool lace_sql_is_rollback(const char *sql) {
  if (!sql)
    return false;
  const char *p = skip_space_comments(sql);
  const char *q = match_word(p, "ROLLBACK");
  if (!q)
    q = match_word(p, "ABORT");
  if (!q)
    return false;
  p = skip_space_comments(q);
  if ((q = match_word(p, "WORK")) || (q = match_word(p, "TRANSACTION")))
    p = skip_space_comments(q);
  if (*p == ';')
    p = skip_space_comments(p + 1);
  return *p == '\0';
}

size_t lace_sql_skip(const char *s, int driver, bool *is_comment) {
  bool mysql = driver == LACE_DRIVER_MYSQL || driver == LACE_DRIVER_MARIADB;
  bool sqlite = driver == LACE_DRIVER_SQLITE;
  bool postgres = driver == LACE_DRIVER_POSTGRES;
  const char *p = s;
  char c = *p;
  *is_comment = false;
  /* PostgreSQL E'...': backslash escapes, as in every MySQL string */
  bool escapes = false;
  if (postgres && (c == 'E' || c == 'e') && p[1] == '\'') {
    escapes = true;
    p++;
    c = *p;
  }
  if (c == '\'' || c == '"' || ((mysql || sqlite) && c == '`')) {
    char quote = c;
    escapes = escapes || (mysql && quote != '`');
    p++;
    while (*p) {
      if (escapes && *p == '\\' && p[1]) {
        p += 2;
        continue;
      }
      if (*p == quote) {
        if (p[1] == quote) { /* Doubled quote = literal quote */
          p += 2;
          continue;
        }
        break;
      }
      p++;
    }
    if (*p)
      p++;
    return (size_t)(p - s);
  }
  if (sqlite && c == '[') {
    while (*p && *p != ']')
      p++;
    if (*p)
      p++;
    return (size_t)(p - s);
  }
  /* MySQL only takes "--" as a comment when whitespace or a control
   * character follows ("a=b--1" is a minus of a minus) */
  bool dashes = c == '-' && p[1] == '-' &&
                (!mysql || p[2] == '\0' || (unsigned char)p[2] <= ' ');
  if (dashes || (c == '#' && mysql)) {
    while (*p && *p != '\n')
      p++;
    *is_comment = true;
    return (size_t)(p - s);
  }
  if (c == '/' && p[1] == '*') {
    p += 2;
    while (*p && !(p[0] == '*' && p[1] == '/'))
      p++;
    if (*p)
      p += 2;
    *is_comment = true;
    return (size_t)(p - s);
  }
  if (postgres && c == '$' && !isdigit((unsigned char)p[1])) {
    /* $tag$ where tag is empty or [A-Za-z_][A-Za-z0-9_]* ($1 is a
     * parameter). A '$' inside an identifier (x$y$) is not one either: the
     * walkers below consume identifiers whole. */
    const char *t = p + 1;
    while (*t && (isalnum((unsigned char)*t) || *t == '_'))
      t++;
    if (*t == '$') {
      size_t taglen = (size_t)(t - p) + 1;
      const char *close = t + 1;
      while (*close && strncmp(close, p, taglen) != 0)
        close++;
      p = *close ? close + taglen : close;
      return (size_t)(p - s);
    }
  }
  return 0;
}

/* Length of the identifier or keyword at s ([A-Za-z_][A-Za-z0-9_$]*), or 0.
 * Walkers step over it whole, so a '$' inside it never opens a $tag$. An
 * E'...' string is the lexer's, not a word. */
size_t lace_sql_word_length(const char *s, int driver) {
  if (!(isalpha((unsigned char)*s) || *s == '_'))
    return 0;
  if (driver == LACE_DRIVER_POSTGRES && (*s == 'E' || *s == 'e') && s[1] == '\'')
    return 0;
  const char *p = s + 1;
  while (isalnum((unsigned char)*p) || *p == '_' || *p == '$')
    p++;
  return (size_t)(p - s);
}

size_t lace_sql_statement_length(const char *p, int driver) {
  const char *s = p;
  while (*s && *s != ';') {
    bool comment;
    size_t skip = lace_sql_skip(s, driver, &comment);
    if (!skip)
      skip = lace_sql_word_length(s, driver);
    s += skip ? skip : 1;
  }
  return (size_t)(s - p);
}

/* Writing verbs: INSERT/UPDATE/DELETE/MERGE/REPLACE/UPSERT, or INTO */
static bool sql_word_writes(const char *w, size_t len) {
  static const char *const verbs[] = {"INSERT", "UPDATE", "DELETE", "MERGE",
                                      "REPLACE", "UPSERT", "INTO", NULL};
  for (int i = 0; verbs[i]; i++) {
    if (strlen(verbs[i]) == len && strncasecmp(w, verbs[i], len) == 0)
      return true;
  }
  return false;
}

bool lace_sql_read_only(const char *sql, int driver) {
  if (!sql)
    return false;
  static const char *const readers[] = {"SELECT", "WITH", "VALUES", "TABLE",
                                        "SHOW", "EXPLAIN", "DESCRIBE", "DESC",
                                        NULL};
  bool first = true;
  bool explain = false;
  /* Where a statement's verb can stand: the start, after '(' (a WITH body
   * or subquery) and after ';'. Elsewhere UPDATE, MERGE or UPSERT are
   * names (PostgreSQL leaves them unreserved: SELECT merge FROM t) */
  bool verb_position = true;
  for (const char *p = sql; *p;) {
    bool comment;
    size_t skip = lace_sql_skip(p, driver, &comment);
    if (skip) {
      p += skip;
      if (!comment)
        verb_position = false;
      continue;
    }
    size_t len = lace_sql_word_length(p, driver);
    if (!len) {
      if (*p == '(' || *p == ';')
        verb_position = true;
      else if (!isspace((unsigned char)*p))
        verb_position = false;
      p++;
      continue;
    }
    if (first) {
      bool reads = false;
      for (int i = 0; readers[i] && !reads; i++)
        reads = strlen(readers[i]) == len && strncasecmp(p, readers[i], len) == 0;
      if (!reads)
        return false;
      explain = len == 7 && strncasecmp(p, "EXPLAIN", 7) == 0;
      first = false;
    }
    /* INTO is reserved everywhere: SELECT ... INTO writes (a table, a file,
     * variables). A writing verb counts in verb position, or anywhere after
     * EXPLAIN (whose options come before the statement), unless it is a
     * function call (MySQL INSERT(), REPLACE()). */
    bool into = len == 4 && strncasecmp(p, "INTO", 4) == 0;
    if (into) {
      return false;
    }
    if ((verb_position || explain) && sql_word_writes(p, len)) {
      const char *n = p + len;
      while (isspace((unsigned char)*n))
        n++;
      if (*n != '(')
        return false;
    }
    verb_position = false;
    p += len;
  }
  return !first;
}

bool lace_sql_single_statement(const char *sql, int driver) {
  if (!sql)
    return true;
  const char *p = sql + lace_sql_statement_length(sql, driver);
  while (*p) {
    bool comment = false;
    size_t skip = lace_sql_skip(p, driver, &comment);
    if (skip && comment) {
      p += skip;
    } else if (*p == ';' || isspace((unsigned char)*p)) {
      p++;
    } else {
      return false;
    }
  }
  return true;
}

char *lace_sql_string_literal(LaceDriver driver, const char *text) {
  return sql_string_literal(text, driver);
}

/* Format a double as a SQL numeric literal (caller frees).
 * - Shortest form that round-trips (%.15g when exact, else %.17g), so 0.1
 *   stays "0.1" and still equals a DECIMAL 0.10 key.
 * - Always uses '.', whatever LC_NUMERIC the frontend set (the TUI calls
 *   setlocale(LC_ALL, ""), which made "1,5" under de_DE/ru_RU).
 * - Infinity/NaN have no numeric literal; they are sent as strings that
 *   PostgreSQL accepts for float columns. */
static char *sql_float_literal(double v, LaceDriver driver) {
  if (isnan(v)) {
    return str_dup(driver == LACE_DRIVER_POSTGRES ? "'NaN'" : "NULL");
  }
  if (isinf(v)) {
    if (driver == LACE_DRIVER_POSTGRES) {
      return str_dup(v > 0 ? "'Infinity'" : "'-Infinity'");
    }
    return str_dup("NULL");
  }
  char buf[64];
  lace_format_double(v, buf, sizeof(buf)); /* Same digits the user sees */
  return str_dup(buf);
}

/* Encode bytes as a hex BLOB literal for the given dialect (caller frees). */
static char *sql_blob_literal(const uint8_t *data, size_t len, LaceDriver driver) {
  static const char hex[] = "0123456789abcdef";
  StringBuilder *sb = sb_new(len * 2 + 16);
  if (!sb) {
    return NULL;
  }
  if (driver == LACE_DRIVER_POSTGRES) {
    /* E'' so the backslash means the same whatever standard_conforming_strings
     * is set to */
    sb_append(sb, "E'\\\\x");
  } else {
    sb_append(sb, "X'");
  }
  for (size_t i = 0; i < len; i++) {
    sb_append_char(sb, hex[data[i] >> 4]);
    sb_append_char(sb, hex[data[i] & 0x0f]);
  }
  sb_append(sb, driver == LACE_DRIVER_POSTGRES ? "'::bytea" : "'");
  return sb_finish(sb);
}

/* Convert LaceValue to a SQL literal for the given dialect (caller frees) */
static char *value_to_sql(const LaceValue *val, LaceDriver driver);

char *lace_value_to_sql_literal(const LaceValue *val, LaceDriver driver) {
  return value_to_sql(val, driver);
}

static char *value_to_sql(const LaceValue *val, LaceDriver driver) {
  if (!val || val->is_null) return str_dup("NULL");
  if (val->omitted) return NULL; /* Only a placeholder label: never write it */

  char buf[64];
  switch (val->type) {
  case LACE_TYPE_NULL:
    return str_dup("NULL");
  case LACE_TYPE_INT:
    snprintf(buf, sizeof(buf), "%lld", (long long)val->int_val);
    return str_dup(buf);
  case LACE_TYPE_FLOAT:
    return sql_float_literal(val->float_val, driver);
  case LACE_TYPE_DECIMAL: {
    /* Exact digits unquoted (a numeric literal, compared exactly); anything
     * else (NaN, ...) as a string the database converts */
    const char *t = val->text.data ? val->text.data : "0";
    LaceValue probe;
    bool numeric = lace_value_from_edit_text(t, LACE_TYPE_DECIMAL, &probe, NULL) &&
                   probe.type == LACE_TYPE_DECIMAL;
    lace_value_free(&probe);
    return numeric ? str_dup(t) : sql_string_literal(t, driver);
  }
  case LACE_TYPE_TEXT:
  case LACE_TYPE_DATE:
  case LACE_TYPE_TIMESTAMP:
    return sql_string_literal(val->text.data, driver);
  case LACE_TYPE_BOOL:
    if (driver == LACE_DRIVER_SQLITE) {
      return str_dup(val->bool_val ? "1" : "0");
    }
    return str_dup(val->bool_val ? "TRUE" : "FALSE");
  case LACE_TYPE_BLOB:
    return sql_blob_literal(val->blob.data, val->blob.len, driver);
  default:
    return str_dup("NULL");
  }
}

/* Append "col1 = v1 AND col2 = v2 ..." for a primary key. A NULL key value
 * uses IS NULL so it still identifies the row. */
static bool append_pk_where(StringBuilder *sb, const LacePkValue *pk,
                            size_t num_pk, LaceDriver driver) {
  for (size_t i = 0; i < num_pk; i++) {
    char *col = lace_quote_identifier(driver, pk[i].column);
    bool is_null = pk[i].value.is_null || pk[i].value.type == LACE_TYPE_NULL;
    char *val = is_null ? NULL : value_to_sql(&pk[i].value, driver);
    if (!col || (!is_null && !val)) {
      free(col);
      free(val);
      return false;
    }
    sb_printf(sb, "%s%s", i > 0 ? " AND " : "", col);
    if (is_null) {
      sb_append(sb, " IS NULL");
    } else {
      sb_append(sb, " = ");
      sb_append(sb, val);
    }
    free(col);
    free(val);
  }
  return sb_ok(sb);
}

/* Run a built mutation statement; frees sql. */
/* How many rows a mutation must change */
typedef enum {
  EXPECT_ONE,     /* UPDATE/DELETE of one row by its key */
  EXPECT_SOME,    /* INSERT */
} MutationExpect;

/* Run a mutation and check its row count: a "success" that changed no row
 * (the row was changed or deleted elsewhere, or its key did not match
 * exactly) or several rows (the key is not unique) must not look like an
 * edit that worked. MySQL connections use CLIENT_FOUND_ROWS, so an UPDATE
 * to the same value still counts its row. */
static int exec_mutation(lace_client_t *client, int conn_id, char *sql,
                         MutationExpect expect) {
  if (!sql) {
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }
  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);
  str_secure_free(sql); /* May contain sensitive cell data */
  if (err == LACE_OK && result) {
    int64_t n = result->rows_affected;
    if (n == 0) {
      lace_set_thread_error(expect == EXPECT_ONE
                                ? "No row was changed: it no longer matches its "
                                  "key (changed or deleted elsewhere?)"
                                : "No row was inserted");
      err = LACE_ERR_ROW_NOT_FOUND;
    } else if (expect == EXPECT_ONE && n > 1) {
      char msg[160];
      snprintf(msg, sizeof(msg),
               "%lld rows were changed: the key is not unique", (long long)n);
      lace_set_thread_error(msg);
      err = LACE_ERR_MULTIPLE_ROWS;
    }
  }
  lace_result_free(result);
  return err;
}

/* "dir/name" if it is an executable file (allocated), else NULL */
static char *executable_at(const char *dir, const char *rel) {
  char *path = str_printf("%s/%s", dir, rel);
  if (path && lace_os_is_executable(path)) {
    char *real = lace_os_real_path(path);
    if (real) {
      free(path);
      return real;
    }
    return path;
  }
  free(path);
  return NULL;
}

/* Find the daemon executable: the given path, else next to (or near, in a
 * source tree) the running program, else common places, else PATH */
static char *find_daemon(const char *daemon_path) {
  if (daemon_path) {
    return lace_os_is_executable(daemon_path) ? str_dup(daemon_path) : NULL;
  }
  const char *name = lace_os_daemon_name();

  char *exe_dir = lace_os_exe_dir();
  if (exe_dir) {
    const char *near[] = {
        "../../../../laced/build", /* gui/gtk/build/ -> laced/build/ */
        "../../../laced/build",    /* tui/ncurses/build/ -> laced/build/ */
        "../../laced/build",       /* tui/build/ -> laced/build/ */
        "../laced/build",          /* Same level as laced/ */
        ".",                       /* Same directory as the program */
        NULL,
    };
    for (int i = 0; near[i]; i++) {
      char *dir = str_printf("%s/%s", exe_dir, near[i]);
      char *found = dir ? executable_at(dir, name) : NULL;
      free(dir);
      if (found) {
        free(exe_dir);
        return found;
      }
    }
    free(exe_dir);
  }

  const char *places[] = {"./laced/build", "./build", "../laced/build",
                          "../../laced/build", "/usr/local/bin", "/usr/bin",
                          NULL};
  for (int i = 0; places[i]; i++) {
    char *found = executable_at(places[i], name);
    if (found) {
      return found;
    }
  }

  const char *path_env = getenv("PATH");
  char *copy = path_env ? str_dup(path_env) : NULL;
  char sep = lace_os_path_list_separator();
  for (char *dir = copy; dir && *dir;) {
    char *end = strchr(dir, sep);
    if (end) {
      *end = '\0';
    }
    char *found = *dir ? executable_at(dir, name) : NULL;
    if (found) {
      free(copy);
      return found;
    }
    dir = end ? end + 1 : NULL;
  }
  free(copy);
  return NULL;
}

/* Spawn a private daemon over pipes (its stdin and stdout). It exits when
 * its stdin closes, i.e. with this client. */
/* Start a private stdio daemon and make the client a stdio client: the
 * pipe pair is its one and only link (nothing else can reach the daemon,
 * so calls take turns on it). The link is returned, not pooled. */
static LaceLink *start_stdio_daemon(lace_client_t *client, const char *daemon_path) {
  char *daemon_exe = find_daemon(daemon_path);
  if (!daemon_exe) {
    set_error(client, "Daemon executable not found");
    return NULL;
  }
  const char *argv[] = {daemon_exe, "--stdio", NULL};
  LaceOsHandle to_daemon = LACE_OS_INVALID, from_daemon = LACE_OS_INVALID;
  LaceOsProcess pid = lace_os_spawn_piped(daemon_exe, argv, &to_daemon, &from_daemon);
  free(daemon_exe);
  if (!pid) {
    set_error(client, "Failed to start the daemon");
    return NULL;
  }
  LaceLink *link = lace_link_new(from_daemon, to_daemon, false);
  pthread_mutex_lock(&client->lock);
  client->single_link = true;
  client->conn_mode = LACE_CONN_AUTO;
  link->epoch = client->link_epoch;
  pthread_mutex_unlock(&client->lock);
  pthread_mutex_lock(&client->write_lock); /* Where notifications read it */
  client->stdio_wfd = to_daemon;
  pthread_mutex_unlock(&client->write_lock);
  client->daemon_pid = pid;
  client->owns_daemon = true;
  return link;
}

static bool spawn_daemon_stdio(lace_client_t *client, const char *daemon_path) {
  LaceLink *link = start_stdio_daemon(client, daemon_path);
  if (!link) {
    return false;
  }
  pthread_mutex_lock(&client->lock);
  link->next = client->idle_links;
  client->idle_links = link;
  client->num_idle++;
  client->num_links++;
  pthread_mutex_unlock(&client->lock);
  client->connected = true;

  /* Verify daemon is responsive */
  cJSON *result = NULL;
  int err = lace_rpc_call(client, "ping", NULL, &result);
  cJSON_Delete(result);
  if (err != LACE_OK) {
    lace_client_close_streams(client);
    client->connected = false;
    lace_os_process_stop(client->daemon_pid, false);
    client->daemon_pid = 0;
    set_error(client, "Daemon not responding");
    return false;
  }
  return true;
}

/* Spawn a socket daemon on the default path. It manages its own lifetime
 * (--idle-timeout, else until signalled); other frontends may share it. */
static bool spawn_daemon_unix(lace_client_t *client, const char *daemon_path,
                              char **socket_path_out) {
  char *daemon_exe = find_daemon(daemon_path);
  if (!daemon_exe) {
    set_error(client, "Daemon executable not found");
    return false;
  }
  char *socket_path = lace_get_default_socket_path();
  if (!socket_path) {
    free(daemon_exe);
    set_error(client, "No default socket path on this system");
    return false;
  }
  char timeout_str[16];
  snprintf(timeout_str, sizeof(timeout_str), "%d", client->idle_timeout);
  const char *argv[] = {daemon_exe, "--unix", socket_path,
                        client->idle_timeout > 0 ? "--idle-timeout" : NULL,
                        timeout_str, NULL};
  LaceOsProcess pid = lace_os_spawn_detached(daemon_exe, argv);
  free(daemon_exe);
  if (!pid) {
    free(socket_path);
    set_error(client, "Failed to start the daemon");
    return false;
  }

  /* Wait for the daemon to create the socket (up to 5 seconds) */
  for (int waited = 0;; waited += 50) {
    if (lace_daemon_is_running(socket_path)) {
      break;
    }
    if (lace_os_process_exited(pid)) {
      free(socket_path);
      set_error(client, "Daemon process exited unexpectedly");
      return false;
    }
    if (waited >= 5000) {
      lace_os_process_stop(pid, false);
      free(socket_path);
      set_error(client, "Timeout waiting for daemon to start");
      return false;
    }
    lace_os_sleep_ms(50);
  }
  *socket_path_out = socket_path;
  client->daemon_pid = pid;
  return true;
}

/* Spawn a TCP daemon and connect to it */
static bool spawn_daemon_tcp(lace_client_t *client, const char *daemon_path,
                             const char *host, int port) {
  char *daemon_exe = find_daemon(daemon_path);
  if (!daemon_exe) {
    set_error(client, "Daemon executable not found");
    return false;
  }
  if (port <= 0) {
    port = LACE_DEFAULT_PORT;
  }
  char addr_str[128];
  if (host && host[0]) {
    snprintf(addr_str, sizeof(addr_str), "%s:%d", host, port);
  } else {
    snprintf(addr_str, sizeof(addr_str), "%d", port);
  }
  char timeout_str[16];
  snprintf(timeout_str, sizeof(timeout_str), "%d", client->idle_timeout);
  const char *argv[] = {daemon_exe, "--tcp", addr_str,
                        client->idle_timeout > 0 ? "--idle-timeout" : NULL,
                        timeout_str, NULL};
  LaceOsProcess pid = lace_os_spawn_detached(daemon_exe, argv);
  free(daemon_exe);
  if (!pid) {
    set_error(client, "Failed to start the daemon");
    return false;
  }

  /* Wait for it to listen (up to a second) */
  const char *connect_host = (host && host[0]) ? host : "127.0.0.1";
  LaceOsHandle fd = LACE_OS_INVALID;
  for (int tries = 0; tries < 50 && fd == LACE_OS_INVALID; tries++) {
    fd = lace_connect_tcp(connect_host, port, 100);
    if (fd == LACE_OS_INVALID) {
      if (lace_os_process_exited(pid)) {
        set_error(client, "Daemon failed to start");
        return false;
      }
      lace_os_sleep_ms(20);
    }
  }
  if (fd == LACE_OS_INVALID) {
    lace_os_process_stop(pid, false);
    set_error(client, "Daemon startup timeout");
    return false;
  }
  if (!lace_client_attach_socket(client, fd, true)) {
    lace_os_process_stop(pid, false);
    set_error(client, "Failed to set up daemon link");
    return false;
  }

  client->daemon_pid = pid;
  client->owns_daemon = false; /* Daemon manages its own lifecycle (idle timeout / signal) */
  client->conn_mode = LACE_CONN_TCP;
  free(client->tcp_host);
  client->tcp_host = str_dup(connect_host);
  client->tcp_port = port;

  /* Verify daemon is responsive */
  cJSON *result = NULL;
  int err = lace_rpc_call(client, "ping", NULL, &result);
  cJSON_Delete(result);
  if (err != LACE_OK) {
    lace_client_close_streams(client);
    client->connected = false;
    lace_os_process_stop(pid, false);
    client->daemon_pid = 0;
    set_error(client, "Daemon not responding");
    return false;
  }
  return true;
}

/* ==========================================================================
 * Client Lifecycle
 * ========================================================================== */

/* Random id for "hello": the daemon keeps this client's database
 * connections while any of its links is open */
static void make_client_id(char *out, size_t out_size) {
  unsigned char raw[16] = {0};
  if (!lace_os_random(raw, sizeof(raw))) {
    uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)lace_os_now_ms() << 20) ^
                    (lace_os_process_id() << 40) ^ (uint64_t)(uintptr_t)out;
    for (size_t i = 0; i < sizeof(raw); i++) {
      seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
      raw[i] = (unsigned char)(seed >> 56);
    }
  }
  static const char hex[] = "0123456789abcdef";
  size_t k = 0;
  for (size_t i = 0; i < sizeof(raw) && k + 2 < out_size; i++) {
    out[k++] = hex[raw[i] >> 4];
    out[k++] = hex[raw[i] & 15];
  }
  out[k] = '\0';
}

void lace_client_init_common(lace_client_t *client) {
  client->timeout_ms = DEFAULT_TIMEOUT_MS;
  client->query_timeout_ms = 0; /* Queries may run long; they can be cancelled */
  client->daemon_pid = 0;
  pthread_mutex_init(&client->spawn_lock, NULL);
  pthread_mutex_init(&client->lock, NULL);
  pthread_mutex_init(&client->write_lock, NULL);
  client->stdio_wfd = LACE_OS_INVALID;
  pthread_cond_init(&client->link_released, NULL);
  make_client_id(client->client_id, sizeof(client->client_id));
}

static lace_client_t *client_alloc(LaceConnMode mode) {
  lace_client_t *client = safe_calloc(1, sizeof(lace_client_t));
  lace_client_init_common(client);
  client->conn_mode = mode;
  return client;
}

LaceLink *lace_link_new(LaceOsHandle rfd, LaceOsHandle wfd, bool is_socket) {
  LaceLink *l = safe_calloc(1, sizeof(LaceLink));
  l->rfd = rfd;
  l->wfd = wfd;
  l->is_socket = is_socket;
  l->next_id = 1;
  return l;
}

void lace_client_forget_stdio(lace_client_t *client, const LaceLink *link) {
  if (!link || link->is_socket)
    return;
  pthread_mutex_lock(&client->write_lock);
  if (client->stdio_wfd == link->wfd)
    client->stdio_wfd = -1;
  pthread_mutex_unlock(&client->write_lock);
}

void lace_link_free(LaceLink *link) {
  if (!link) {
    return;
  }
  lace_os_close(link->rfd, link->is_socket);
  if (link->wfd != link->rfd) {
    lace_os_close(link->wfd, link->is_socket);
  }
  free(link->buf);
  free(link);
}

void lace_client_close_streams(lace_client_t *client) {
  pthread_mutex_lock(&client->lock);
  LaceLink *l = client->idle_links;
  client->idle_links = NULL;
  client->num_links -= client->num_idle;
  client->num_idle = 0;
  pthread_cond_broadcast(&client->link_released);
  pthread_mutex_unlock(&client->lock);
  while (l) {
    LaceLink *next = l->next;
    lace_client_forget_stdio(client, l);
    lace_link_free(l);
    l = next;
  }
}

bool lace_client_attach_socket(lace_client_t *client, LaceOsHandle fd, bool tcp) {
  if (fd == LACE_OS_INVALID) {
    return false;
  }
  LaceLink *l = lace_link_new(fd, fd, true);
  l->tcp = tcp;
  pthread_mutex_lock(&client->lock);
  l->epoch = client->link_epoch;
  l->next = client->idle_links;
  client->idle_links = l;
  client->num_idle++;
  client->num_links++;
  pthread_mutex_unlock(&client->lock);
  client->connected = true;
  return true;
}

static void kill_spawned_daemon(lace_client_t *client) {
  if (client->daemon_pid) {
    lace_os_process_stop(client->daemon_pid, false);
    client->daemon_pid = 0;
  }
}

/* Spawn a daemon and connect to it. Leaves an error on the client on failure. */
static void spawn_and_attach(lace_client_t *client, const char *daemon_path,
                             int timeout_ms) {
  char *spawned_path = NULL;
  if (!spawn_daemon_unix(client, daemon_path, &spawned_path)) {
    return; /* Error already set */
  }
  LaceOsHandle fd = lace_connect_unix(spawned_path, timeout_ms);
  if (fd == LACE_OS_INVALID || !lace_client_attach_socket(client, fd, false)) {
    kill_spawned_daemon(client);
    set_error(client, "Failed to connect to spawned daemon");
  } else {
    lace_client_set_socket_path(client, spawned_path);
    client->conn_mode = LACE_CONN_UNIX;
  }
  free(spawned_path);
}

/* LACE_CONN_AUTO: attach to a running daemon first, spawn only if none
 * answers */
static lace_client_t *create_auto(const char *daemon_path,
                                  LaceSpawnMode spawn_mode, int idle_timeout) {
  lace_client_t *client = client_alloc(LACE_CONN_AUTO);
  client->idle_timeout = idle_timeout;
  client->detect = spawn_mode == LACE_SPAWN_DETECT;
  /* A respawn after a crash must start the same binary */
  client->daemon_path = daemon_path ? str_dup(daemon_path) : NULL;

  /* Prefer an already-running daemon */
  if (spawn_mode == LACE_SPAWN_UNIX || spawn_mode == LACE_SPAWN_DETECT) {
    char *socket_path = lace_get_default_socket_path();
    if (socket_path && lace_daemon_is_running(socket_path)) {
      LaceOsHandle fd = lace_connect_unix(socket_path, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
      if (fd != LACE_OS_INVALID && lace_client_attach_socket(client, fd, false)) {
        lace_client_set_socket_path(client, socket_path);
        free(socket_path);
        client->conn_mode = LACE_CONN_UNIX;
        return client;
      }
    }
    free(socket_path);
  }
  if (spawn_mode == LACE_SPAWN_TCP || spawn_mode == LACE_SPAWN_DETECT) {
    LaceOsHandle fd = lace_connect_tcp(NULL, LACE_DEFAULT_PORT,
                                       LACE_DEFAULT_CONNECT_TIMEOUT_MS);
    if (fd != LACE_OS_INVALID && lace_client_attach_socket(client, fd, true)) {
      client->conn_mode = LACE_CONN_TCP;
      client->tcp_host = str_dup("127.0.0.1");
      client->tcp_port = LACE_DEFAULT_PORT;
      if (spawn_mode != LACE_SPAWN_DETECT) {
        return client;
      }
      /* Detect: a listener counts only once it passed the handshake (one
       * whose token we cannot read is not a daemon we can use) */
      cJSON *pong = NULL;
      int rc = lace_rpc_call(client, "ping", NULL, &pong);
      cJSON_Delete(pong);
      if (rc == LACE_OK) {
        return client;
      }
      lace_client_close_streams(client);
      client->connected = false;
      client->conn_mode = LACE_CONN_AUTO;
    }
  }

  /* No running daemon - spawn one with the preferred mode (detect: a
   * private one, which leaves nothing running behind it) */
  switch (spawn_mode) {
  case LACE_SPAWN_STDIO:
  case LACE_SPAWN_DETECT:
    spawn_daemon_stdio(client, daemon_path);
    break;
  case LACE_SPAWN_TCP:
    spawn_daemon_tcp(client, daemon_path, NULL, LACE_DEFAULT_PORT);
    break;
  case LACE_SPAWN_UNIX:
  default:
    spawn_and_attach(client, daemon_path, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
    break;
  }
  /* Error or success already set by the spawn function */
  return client;
}

/* Create client from existing socket fd */
static lace_client_t *create_from_socket(LaceOsHandle fd, LaceConnMode mode) {
  if (fd == LACE_OS_INVALID) {
    return NULL;
  }
  lace_client_t *client = client_alloc(mode);
  if (!lace_client_attach_socket(client, fd, mode == LACE_CONN_TCP)) {
    lace_client_destroy(client);
    return NULL;
  }
  return client;
}

lace_client_t *lace_client_create(const char *daemon_path) {
  LaceClientConfig config = {0};
  config.daemon_path = daemon_path;
  return lace_client_create_with_config(&config);
}

lace_client_t *lace_client_create_with_config(const LaceClientConfig *config) {
  static const LaceClientConfig defaults = {0};
  if (!config) {
    config = &defaults;
  }

  int timeout = config->connect_timeout_ms > 0 ? config->connect_timeout_ms
                                               : LACE_DEFAULT_CONNECT_TIMEOUT_MS;

  switch (config->mode) {
  case LACE_CONN_AUTO:
    return create_auto(config->daemon_path, config->spawn_mode,
                       config->idle_timeout);

  case LACE_CONN_UNIX: {
    char *default_path = NULL;
    const char *path = config->socket_path;
    if (!path) {
      default_path = lace_get_default_socket_path();
      path = default_path;
    }
    LaceOsHandle fd = path ? lace_connect_unix(path, timeout) : LACE_OS_INVALID;

    lace_client_t *client = client_alloc(LACE_CONN_UNIX);
    client->daemon_path = config->daemon_path ? str_dup(config->daemon_path) : NULL;
    client->idle_timeout = config->idle_timeout;
    client->no_respawn = !config->spawn_if_missing;
    if (fd != LACE_OS_INVALID && lace_client_attach_socket(client, fd, false)) {
      lace_client_set_socket_path(client, path);
      free(default_path);
      return client;
    }
    /* A spawned daemon always listens on the default path, so only spawn
     * when that is the path asked for */
    free(default_path);
    char *spawn_path = lace_get_default_socket_path();
    bool is_default = !config->socket_path ||
                      (spawn_path && strcmp(config->socket_path, spawn_path) == 0);
    free(spawn_path);
    if (config->spawn_if_missing && is_default) {
      client->conn_mode = LACE_CONN_AUTO;
      spawn_and_attach(client, config->daemon_path, timeout);
    } else {
      set_error(client, "Failed to connect to Unix socket");
    }
    return client;
  }

  case LACE_CONN_TCP: {
    LaceOsHandle fd = lace_connect_tcp(config->host, config->port, timeout);
    lace_client_t *client = create_from_socket(fd, LACE_CONN_TCP);
    if (!client) {
      client = client_alloc(LACE_CONN_TCP);
      set_error(client, "Failed to connect to TCP socket");
    } else {
      client->tcp_host = str_dup(config->host ? config->host : "127.0.0.1");
      client->tcp_port = config->port > 0 ? config->port : LACE_DEFAULT_PORT;
    }
    return client;
  }
  }

  lace_client_t *client = client_alloc(LACE_CONN_AUTO);
  set_error(client, "Unknown connection mode");
  return client;
}

void lace_client_destroy(lace_client_t *client) {
  if (!client) {
    return;
  }

  /* Never shut a socket daemon down here, even one this client spawned:
   * other frontends may have attached to it through the shared socket.
   * Socket daemons manage their own lifetime (--idle-timeout or a signal).
   * Only a stdio daemon is owned, since nothing else can reach it. */
  lace_client_close_streams(client);
  if (client->owns_daemon && client->daemon_pid) {
    /* A stdio daemon belongs to this client alone; closing its stdin makes
     * it exit. Give it a moment, then make sure. */
    bool exited = false;
    for (int waited_ms = 0; !(exited = lace_os_process_exited(client->daemon_pid)) &&
                            waited_ms < 1000;
         waited_ms += 50) {
      lace_os_sleep_ms(50);
    }
    /* Only a process not yet reaped: a reaped pid may belong to another */
    if (!exited) {
      lace_os_process_stop(client->daemon_pid, false);
    }
  } else if (client->daemon_pid) {
    lace_os_process_exited(client->daemon_pid); /* Reap if it already exited */
  }

  pthread_cond_destroy(&client->link_released);
  pthread_mutex_destroy(&client->lock);
  pthread_mutex_destroy(&client->write_lock);
  pthread_mutex_destroy(&client->spawn_lock);
  free(client->daemon_path);
  free(client->socket_path);
  free(client->tcp_host);
  str_secure_free(client->token);
  free(client);
}

bool lace_client_connected(const lace_client_t *client) {
  /* `connected` means the client was set up successfully. A socket dropped
   * later is re-established transparently by the next RPC call. */
  return client && client->connected;
}

const char *lace_client_error(const lace_client_t *client) {
  if (!client) {
    return "Invalid client";
  }
  pthread_once(&g_error_once, error_key_init);
  const char *msg = pthread_getspecific(g_error_key);
  return msg ? msg : "Unknown error";
}

uint64_t lace_client_generation(const lace_client_t *client) {
  return client ? client->generation : 0;
}

void lace_client_set_socket_path(lace_client_t *client, const char *path) {
  if (!client || !path || client->socket_path == path) {
    return;
  }
  char *copy = str_dup(path);
  pthread_mutex_lock(&client->lock);
  free(client->socket_path);
  client->socket_path = copy;
  pthread_mutex_unlock(&client->lock);
}

/* Count a link just opened (num_links) and stamp it with the current
 * epoch; with bump, links of every older epoch are retired first */
static void count_new_link(lace_client_t *client, LaceLink *l, bool bump) {
  pthread_mutex_lock(&client->lock);
  if (bump) {
    client->link_epoch++;
  }
  l->epoch = client->link_epoch;
  client->num_links++;
  pthread_mutex_unlock(&client->lock);
}

/* Detect mode: a TCP endpoint is usable only once the handshake passed */
static LaceLink *detect_tcp_link(lace_client_t *client, const char *host, int port) {
  LaceOsHandle fd = lace_connect_tcp(host, port, 1000);
  if (fd == LACE_OS_INVALID) {
    return NULL;
  }
  LaceLink *l = lace_link_new(fd, fd, true);
  l->tcp = true;
  if (lace_link_initialize(client, l, false) != LACE_OK) {
    lace_link_free(l);
    return NULL;
  }
  return l;
}

/* Detect mode, after the daemon was lost (or its links are all gone):
 * detect again, as at startup. The daemon in use while it answers, else a
 * shared daemon on the default Unix socket, else one on TCP
 * LACE_DEFAULT_PORT, else a new private stdio daemon; the client's
 * transport follows. Everything happens under spawn_lock, and the new link
 * is counted before it is released, so a thread that waited there sees the
 * switch another one made. Returns as lace_client_open_link(). */
static int detect_open_link(lace_client_t *client, LaceLink **out) {
  pthread_mutex_lock(&client->spawn_lock);
  /* Another thread switched to a stdio daemon meanwhile: wait for its link */
  pthread_mutex_lock(&client->lock);
  bool stdio_live = client->single_link && client->num_links > 0;
  LaceConnMode mode = client->conn_mode;
  char *cur_path = client->socket_path ? str_dup(client->socket_path) : NULL;
  char *cur_host = client->tcp_host ? str_dup(client->tcp_host) : NULL;
  int cur_port = client->tcp_port;
  pthread_mutex_unlock(&client->lock);
  LaceLink *l = NULL;
  int how = 1;
  if (stdio_live) {
    how = LACE_OPEN_LINK_WAIT;
    goto done;
  }

  /* The daemon in use, while it lives: a pool that merely ran out of links
   * must not hop to another daemon (that would orphan every conn_id) */
  if (mode == LACE_CONN_UNIX && cur_path) {
    LaceOsHandle fd = lace_connect_unix(cur_path, 1000);
    if (fd != LACE_OS_INVALID) {
      l = lace_link_new(fd, fd, true);
      count_new_link(client, l, false);
      goto done;
    }
  } else if (mode == LACE_CONN_TCP) {
    l = detect_tcp_link(client, cur_host, cur_port);
    if (l) {
      count_new_link(client, l, false);
      goto done;
    }
  }

  /* Lost: retire a private daemon that died, and leave stdio mode */
  if (client->owns_daemon) {
    if (client->daemon_pid) {
      lace_os_process_stop(client->daemon_pid, false);
    }
    client->daemon_pid = 0;
    client->owns_daemon = false;
    pthread_mutex_lock(&client->lock);
    client->single_link = false;
    pthread_mutex_unlock(&client->lock);
  }

  char *path = lace_get_default_socket_path();
  LaceOsHandle fd = path ? lace_connect_unix(path, 1000) : LACE_OS_INVALID;
  if (fd != LACE_OS_INVALID) {
    lace_client_set_socket_path(client, path);
    free(path);
    pthread_mutex_lock(&client->lock);
    client->conn_mode = LACE_CONN_UNIX;
    pthread_mutex_unlock(&client->lock);
    l = lace_link_new(fd, fd, true);
    count_new_link(client, l, true); /* A different daemon moves the
                                        generation on in link_initialize */
    goto done;
  }
  free(path);

  l = detect_tcp_link(client, NULL, LACE_DEFAULT_PORT);
  if (l) {
    pthread_mutex_lock(&client->lock);
    free(client->tcp_host);
    client->tcp_host = str_dup("127.0.0.1");
    client->tcp_port = LACE_DEFAULT_PORT;
    client->conn_mode = LACE_CONN_TCP;
    pthread_mutex_unlock(&client->lock);
    count_new_link(client, l, true);
    goto done;
  }

  l = start_stdio_daemon(client, client->daemon_path);
  if (l) {
    count_new_link(client, l, true);
    how = 2;
  } else {
    how = 0;
  }

done:
  pthread_mutex_unlock(&client->spawn_lock);
  free(cur_path);
  free(cur_host);
  *out = l;
  return l || how == LACE_OPEN_LINK_WAIT ? how : 0;
}

/* The non-detect transports */
static int open_link_fixed(lace_client_t *client, LaceLink **out);

int lace_client_open_link(lace_client_t *client, LaceLink **out) {
  *out = NULL;
  if (client->detect) {
    return detect_open_link(client, out);
  }
  int how = open_link_fixed(client, out);
  if (*out) {
    (*out)->tcp = client->conn_mode == LACE_CONN_TCP; /* Fixed for its life */
    count_new_link(client, *out, false);
  }
  return how;
}

static int open_link_fixed(lace_client_t *client, LaceLink **out) {
  *out = NULL;
  /* A stdio daemon died with its pipes: nothing to reopen */
  if (client->owns_daemon) {
    return 0;
  }

  if (client->conn_mode == LACE_CONN_TCP) {
    pthread_mutex_lock(&client->lock);
    char *host = client->tcp_host ? str_dup(client->tcp_host) : NULL;
    int port = client->tcp_port;
    pthread_mutex_unlock(&client->lock);
    LaceOsHandle fd = lace_connect_tcp(host, port, 1000);
    free(host);
    if (fd == LACE_OS_INVALID) {
      return 0;
    }
    *out = lace_link_new(fd, fd, true);
    return 1;
  }

  pthread_mutex_lock(&client->lock);
  char *own_path = client->socket_path ? str_dup(client->socket_path) : NULL;
  pthread_mutex_unlock(&client->lock);
  char *default_path = lace_get_default_socket_path();
  const char *path = own_path ? own_path : default_path;
  LaceOsHandle fd = path ? lace_connect_unix(path, 1000) : LACE_OS_INVALID;
  if (fd != LACE_OS_INVALID) {
    free(own_path);
    free(default_path);
    *out = lace_link_new(fd, fd, true);
    return 1;
  }

  /* Nothing listening. Only respawn on the default path: that is where
   * spawn_daemon_unix puts a daemon, and a custom path is someone else's. */
  bool is_default = !own_path ||
                    (default_path && strcmp(own_path, default_path) == 0);
  free(own_path);
  free(default_path);
  if (!is_default || client->no_respawn) {
    return 0;
  }
  /* One respawn at a time. A thread that waited here usually finds the
   * daemon another one just started: connect to it instead of forking a
   * second (link_initialize then sees its new instance). */
  pthread_mutex_lock(&client->spawn_lock);
  char *retry_path = lace_get_default_socket_path();
  fd = retry_path ? lace_connect_unix(retry_path, 1000) : LACE_OS_INVALID;
  free(retry_path);
  if (fd != LACE_OS_INVALID) {
    pthread_mutex_unlock(&client->spawn_lock);
    *out = lace_link_new(fd, fd, true);
    return 1;
  }
  if (client->daemon_pid) {
    lace_os_process_exited(client->daemon_pid); /* Reap the dead one */
    client->daemon_pid = 0;
  }
  char *spawned = NULL;
  if (!spawn_daemon_unix(client, client->daemon_path, &spawned)) {
    pthread_mutex_unlock(&client->spawn_lock);
    return 0;
  }
  fd = lace_connect_unix(spawned, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
  if (fd == LACE_OS_INVALID) {
    free(spawned);
    kill_spawned_daemon(client);
    pthread_mutex_unlock(&client->spawn_lock);
    return 0;
  }
  pthread_mutex_unlock(&client->spawn_lock);
  lace_client_set_socket_path(client, spawned);
  free(spawned);
  *out = lace_link_new(fd, fd, true);
  return 2;
}

/* ==========================================================================
 * Configuration
 * ========================================================================== */

void lace_set_timeout(lace_client_t *client, int timeout_ms) {
  if (client) {
    client->timeout_ms = timeout_ms;
  }
}

void lace_client_set_token(lace_client_t *client, const char *token) {
  if (!client) {
    return;
  }
  char *copy = token ? str_dup(token) : NULL;
  pthread_mutex_lock(&client->lock);
  str_secure_free(client->token);
  client->token = copy;
  pthread_mutex_unlock(&client->lock);
}

void lace_set_query_timeout(lace_client_t *client, int timeout_ms) {
  if (client) {
    client->query_timeout_ms = timeout_ms > 0 ? timeout_ms : 0;
  }
}

int lace_get_timeout(const lace_client_t *client) {
  return client ? client->timeout_ms : 0;
}

/* ==========================================================================
 * Database Connection
 * ========================================================================== */

LaceValueType lace_type_from_name(const char *type_name, LaceDriver driver) {
  if (!type_name || !*type_name) {
    /* SQLite: no declared type (BLOB affinity, anything goes). Edits of
     * such a column are text, not hex */
    return LACE_TYPE_TEXT;
  }
  char lower[64];
  size_t n = 0;
  for (; type_name[n] && n < sizeof(lower) - 1; n++)
    lower[n] = (char)tolower((unsigned char)type_name[n]);
  lower[n] = '\0';

  if (driver == LACE_DRIVER_SQLITE) {
    /* The affinity rules, in SQLite's order */
    if (strstr(lower, "int"))
      return LACE_TYPE_INT;
    if (strstr(lower, "char") || strstr(lower, "clob") || strstr(lower, "text"))
      return LACE_TYPE_TEXT;
    if (strstr(lower, "blob"))
      return LACE_TYPE_BLOB;
    if (strstr(lower, "real") || strstr(lower, "floa") || strstr(lower, "doub"))
      return LACE_TYPE_FLOAT;
    return LACE_TYPE_FLOAT; /* NUMERIC affinity: integers or reals */
  }

  /* The leading word: "double precision", "varbinary(16)", "int unsigned" */
  char word[32];
  size_t w = 0;
  while (lower[w] && (isalpha((unsigned char)lower[w]) || lower[w] == '_') &&
         w < sizeof(word) - 1) {
    word[w] = lower[w];
    w++;
  }
  word[w] = '\0';
  static const struct {
    const char *name;
    LaceValueType type;
  } names[] = {
      {"tinyint", LACE_TYPE_INT},     {"smallint", LACE_TYPE_INT},
      {"mediumint", LACE_TYPE_INT},   {"int", LACE_TYPE_INT},
      {"integer", LACE_TYPE_INT},     {"bigint", LACE_TYPE_INT},
      {"serial", LACE_TYPE_INT},      {"bigserial", LACE_TYPE_INT},
      {"smallserial", LACE_TYPE_INT}, {"year", LACE_TYPE_INT},
      {"real", LACE_TYPE_FLOAT},      {"float", LACE_TYPE_FLOAT},
      {"double", LACE_TYPE_FLOAT},    {"numeric", LACE_TYPE_DECIMAL},
      {"decimal", LACE_TYPE_DECIMAL}, {"bytea", LACE_TYPE_BLOB},
      {"blob", LACE_TYPE_BLOB},       {"tinyblob", LACE_TYPE_BLOB},
      {"mediumblob", LACE_TYPE_BLOB}, {"longblob", LACE_TYPE_BLOB},
      {"binary", LACE_TYPE_BLOB},     {"varbinary", LACE_TYPE_BLOB},
      {"bit", LACE_TYPE_BLOB},        {"geometry", LACE_TYPE_BLOB},
      {"point", LACE_TYPE_TEXT},      {"boolean", LACE_TYPE_BOOL},
      {"bool", LACE_TYPE_BOOL},       {"date", LACE_TYPE_DATE},
      {"timestamp", LACE_TYPE_TIMESTAMP}, {"datetime", LACE_TYPE_TIMESTAMP},
      {"time", LACE_TYPE_TIMESTAMP},
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strcmp(word, names[i].name) == 0) {
      /* PostgreSQL "bit" is a bit string shown as text; MySQL's is bytes */
      if (names[i].type == LACE_TYPE_BLOB && strcmp(word, "bit") == 0 &&
          driver == LACE_DRIVER_POSTGRES)
        return LACE_TYPE_TEXT;
      /* MySQL BOOL is TINYINT(1) */
      if (names[i].type == LACE_TYPE_BOOL && driver != LACE_DRIVER_POSTGRES)
        return LACE_TYPE_INT;
      return names[i].type;
    }
  }
  return LACE_TYPE_TEXT;
}

/* "sqlite://relative.db" would be opened relative to the daemon's working
 * directory, which a shared or long-running daemon does not share with this
 * process: make the path absolute here. Returns NULL if nothing to change. */
static char *absolute_sqlite_connstr(const char *connstr) {
  if (strncmp(connstr, "sqlite://", 9) != 0) {
    return NULL;
  }
  const char *rest = connstr + 9;
  if (lace_os_path_is_absolute(rest) || rest[0] == '\0' || rest[0] == ':' ||
      rest[0] == '?') {
    return NULL; /* Absolute already, in-memory, or empty */
  }
  char *cwd = lace_os_current_dir();
  if (!cwd) {
    return NULL;
  }
  /* The rest is already in URL form; the working directory is not */
  char *enc_cwd = connstr_encode_path(strcmp(cwd, "/") == 0 ? "" : cwd);
  free(cwd);
  char *abs = enc_cwd ? str_printf("sqlite://%s/%s", enc_cwd, rest) : NULL;
  free(enc_cwd);
  return abs;
}

int lace_connect(lace_client_t *client, const char *connstr,
                 const char *password, int *conn_id) {
  if (!client || !connstr || !conn_id) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  char *abs_connstr = absolute_sqlite_connstr(connstr);
  if (abs_connstr) {
    connstr = abs_connstr;
  }
  cJSON_AddStringToObject(params, "connstr", connstr);
  if (password) {
    cJSON_AddStringToObject(params, "password", password);
  }

  /* The id belongs to the daemon that answered: if the daemon behind the
   * socket was replaced during the call, it may be another one's, and a
   * caller tagging it with the new generation would reach another
   * database. Fail instead; the caller reconnects. */
  uint64_t generation = client->generation;
  cJSON *result = NULL;
  int err = lace_rpc_call(client, "connect", params, &result);
  cJSON_Delete(params);

  if (err != LACE_OK) {
    str_secure_free(abs_connstr);
    return err;
  }
  if (client->generation != generation) {
    cJSON_Delete(result);
    str_secure_free(abs_connstr);
    return lace_fail(LACE_ERR_CONNECTION_LOST,
                     "Lace daemon was restarted during connect; try again");
  }

  cJSON *id_json = cJSON_GetObjectItem(result, "conn_id");
  if (!id_json || !cJSON_IsNumber(id_json)) {
    cJSON_Delete(result);
    str_secure_free(abs_connstr);
    return lace_fail(LACE_ERR_INTERNAL_ERROR, NULL);
  }

  *conn_id = id_json->valueint;
  cJSON_Delete(result);

  /* Track driver type for this connection */
  LaceDriver driver = parse_driver(connstr);
  track_connection(client, *conn_id, driver);

  str_secure_free(abs_connstr); /* connstr may point at it: free it last */
  return LACE_OK;
}

int lace_disconnect(lace_client_t *client, int conn_id) {
  if (!client) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "disconnect", params, &result);
  cJSON_Delete(params);
  cJSON_Delete(result);

  /* Untrack connection */
  untrack_connection(client, conn_id);

  return err;
}

int lace_list_connections(lace_client_t *client, LaceConnInfo **info,
                          size_t *count) {
  if (!client || !info || !count) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "connections", NULL, &result);
  if (err != LACE_OK) {
    return err;
  }

  if (!cJSON_IsArray(result)) {
    cJSON_Delete(result);
    return lace_fail(LACE_ERR_INTERNAL_ERROR, NULL);
  }

  int num = cJSON_GetArraySize(result);
  if (num == 0) {
    *info = NULL;
    *count = 0;
    cJSON_Delete(result);
    return LACE_OK;
  }

  LaceConnInfo *arr = safe_calloc((size_t)num, sizeof(LaceConnInfo));

  for (int i = 0; i < num; i++) {
    cJSON *item = cJSON_GetArrayItem(result, i);
    if (!item) continue;

    cJSON *id = cJSON_GetObjectItem(item, "id");
    cJSON *driver = cJSON_GetObjectItem(item, "driver");
    cJSON *database = cJSON_GetObjectItem(item, "database");
    cJSON *host = cJSON_GetObjectItem(item, "host");
    cJSON *port = cJSON_GetObjectItem(item, "port");
    cJSON *user = cJSON_GetObjectItem(item, "user");

    if (id && cJSON_IsNumber(id)) arr[i].id = id->valueint;
    if (driver && cJSON_IsString(driver)) {
      driver_from_name(driver->valuestring, &arr[i].driver);
    }
    if (database && cJSON_IsString(database)) arr[i].database = str_dup(database->valuestring);
    if (host && cJSON_IsString(host)) arr[i].host = str_dup(host->valuestring);
    if (port && cJSON_IsNumber(port)) arr[i].port = port->valueint;
    if (user && cJSON_IsString(user)) arr[i].user = str_dup(user->valuestring);
  }

  *info = arr;
  *count = (size_t)num;
  cJSON_Delete(result);
  return LACE_OK;
}

void lace_conn_info_array_free(LaceConnInfo *info, size_t count) {
  if (!info) return;

  for (size_t i = 0; i < count; i++) {
    free(info[i].database);
    free(info[i].host);
    free(info[i].user);
  }
  free(info);
}

/* ==========================================================================
 * Schema Discovery
 * ========================================================================== */

int lace_list_tables(lace_client_t *client, int conn_id, char ***tables,
                     size_t *count) {
  if (!client || !tables || !count) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  LaceDriver driver;
  int drv_err = get_connection_driver(client, conn_id, &driver);
  if (drv_err != LACE_OK) {
    return drv_err;
  }

  /* Build driver-specific SQL */
  const char *sql;
  switch (driver) {
  case LACE_DRIVER_SQLITE:
    sql = "SELECT name FROM sqlite_master WHERE type='table' "
          "AND substr(name, 1, 7) <> 'sqlite_' ORDER BY name";
    break;
  case LACE_DRIVER_POSTGRES:
    /* Every table in every usable non-system schema, partitions excluded.
     * Column 3 says whether the table is in public and reached by an
     * unqualified name (listed bare); those sort first. */
    sql = "SELECT n.nspname, c.relname, "
          "(n.nspname = 'public' AND pg_catalog.pg_table_is_visible(c.oid))::int "
          "AS bare "
          "FROM pg_catalog.pg_class c "
          "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
          "WHERE c.relkind IN ('r', 'p') AND NOT c.relispartition "
          "AND n.nspname NOT IN ('pg_catalog', 'information_schema') "
          "AND n.nspname NOT LIKE 'pg\\_toast%' "
          "AND n.nspname NOT LIKE 'pg\\_temp\\_%' "
          "AND pg_catalog.has_schema_privilege(n.oid, 'USAGE') "
          "ORDER BY bare DESC, n.nspname, c.relname";
    break;
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB:
    sql = "SHOW TABLES";
    break;
  default:
    return lace_fail(LACE_ERR_INTERNAL_ERROR, NULL);
  }

  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);
  if (err != LACE_OK) {
    return err;
  }

  if (!result || result->num_rows == 0) {
    *tables = NULL;
    *count = 0;
    lace_result_free(result);
    return LACE_OK;
  }

  char **arr = safe_calloc(result->num_rows, sizeof(char *));
  if (!arr) {
    lace_result_free(result);
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  for (size_t i = 0; i < result->num_rows; i++) {
    LaceRow *row = &result->rows[i];
    if (driver == LACE_DRIVER_POSTGRES) {
      if (row->cells && row->num_cells >= 3 &&
          row->cells[0].type == LACE_TYPE_TEXT &&
          row->cells[1].type == LACE_TYPE_TEXT) {
        bool bare = row->cells[2].type == LACE_TYPE_INT && row->cells[2].int_val;
        arr[i] = lace_table_ref(bare ? NULL : row->cells[0].text.data,
                                row->cells[1].text.data);
      }
    } else if (row->cells && row->num_cells > 0 && row->cells[0].type == LACE_TYPE_TEXT) {
      arr[i] = str_dup(row->cells[0].text.data);
    }
  }

  *tables = arr;
  *count = result->num_rows;
  lace_result_free(result);
  return LACE_OK;
}

void lace_tables_free(char **tables, size_t count) {
  if (!tables) return;

  for (size_t i = 0; i < count; i++) {
    free(tables[i]);
  }
  free(tables);
}

/* Copy a TEXT cell's string, or NULL for any other type (including NULL) */
static char *cell_text_dup(const LaceValue *v) {
  return v->type == LACE_TYPE_TEXT && v->text.data ? str_dup(v->text.data) : NULL;
}

int lace_get_schema(lace_client_t *client, int conn_id, const char *table,
                    LaceSchema **schema) {
  if (!client || !table || !schema) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  LaceDriver driver;
  int drv_err = get_connection_driver(client, conn_id, &driver);
  if (drv_err != LACE_OK) {
    return drv_err;
  }

  /* Build driver-specific schema query */
  char *sql = NULL;
  switch (driver) {
  case LACE_DRIVER_SQLITE: {
    char *lit = sql_string_literal(table, driver);
    if (lit)
      /* table_xinfo also lists generated columns (in SELECT * results), so
       * positions and names match the data; table_info omits them */
      sql = str_printf("PRAGMA table_xinfo(%s)", lit);
    free(lit);
    break;
  }
  case LACE_DRIVER_POSTGRES: {
    /* to_regclass() resolves the name through search_path, exactly like the
     * unqualified SQL the client sends for this table. Booleans are cast to
     * int because they reach the client as 't'/'f' text. */
    char *ident = lace_quote_table(driver, table);
    char *lit = ident ? sql_string_literal(ident, driver) : NULL;
    if (lit) {
      sql = str_printf(
          "SELECT a.attname, "
          "pg_catalog.format_type(a.atttypid, a.atttypmod), "
          "(NOT a.attnotnull)::int, "
          "pg_catalog.pg_get_expr(d.adbin, d.adrelid), "
          "(EXISTS (SELECT 1 FROM pg_catalog.pg_index i "
          "WHERE i.indrelid = a.attrelid AND i.indisprimary "
          "AND a.attnum = ANY (i.indkey)))::int, "
          "(a.attidentity <> '' OR COALESCE(pg_catalog.pg_get_expr("
          "d.adbin, d.adrelid) LIKE 'nextval(%%', false))::int, "
          /* attgenerated exists from PostgreSQL 12: read it via JSON so
           * older servers just get NULL */
          "(COALESCE(row_to_json(a)->>'attgenerated', '') <> '')::int "
          "FROM pg_catalog.pg_attribute a "
          "LEFT JOIN pg_catalog.pg_attrdef d "
          "ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
          "WHERE a.attrelid = to_regclass(%s) "
          "AND a.attnum > 0 AND NOT a.attisdropped "
          "ORDER BY a.attnum",
          lit);
    }
    free(ident);
    free(lit);
    break;
  }
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB: {
    char *ident = lace_quote_table(driver, table);
    if (ident)
      sql = str_printf("DESCRIBE %s", ident);
    free(ident);
    break;
  }
  default:
    return lace_fail(LACE_ERR_INTERNAL_ERROR, NULL);
  }
  if (!sql)
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);

  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);
  if (err != LACE_OK && driver == LACE_DRIVER_SQLITE) {
    /* SQLite before 3.26 has no table_xinfo */
    char *lit = sql_string_literal(table, driver);
    char *old_sql = lit ? str_printf("PRAGMA table_info(%s)", lit) : NULL;
    free(lit);
    if (old_sql) {
      err = lace_exec(client, conn_id, old_sql, &result);
      free(old_sql);
    }
  }
  free(sql);
  if (err != LACE_OK) {
    return err;
  }

  if (!result || result->num_rows == 0) {
    lace_result_free(result);
    return lace_fail(LACE_ERR_TABLE_NOT_FOUND, NULL);
  }

  /* Allocate schema */
  LaceSchema *sch = safe_calloc(1, sizeof(LaceSchema));
  if (!sch) {
    lace_result_free(result);
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  sch->name = str_dup(table);
  sch->num_columns = result->num_rows;
  sch->columns = safe_calloc(result->num_rows, sizeof(LaceColumn));
  if (!sch->columns) {
    free(sch->name);
    free(sch);
    lace_result_free(result);
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  /* Parse columns based on driver */
  for (size_t i = 0; i < result->num_rows; i++) {
    LaceRow *row = &result->rows[i];
    LaceColumn *col = &sch->columns[i];

    switch (driver) {
    case LACE_DRIVER_SQLITE:
      /* PRAGMA table_xinfo: cid, name, type, notnull, dflt_value, pk,
       * hidden (0 normal, 1 hidden in a virtual table, 2/3 generated) */
      if (result->num_columns >= 6 && row->num_cells >= 6) {
        int hidden = (row->num_cells >= 7 && row->cells[6].type == LACE_TYPE_INT)
                         ? (int)row->cells[6].int_val
                         : 0;
        if (hidden == 1) {
          break; /* Not part of SELECT *: dropped below */
        }
        col->name = row->cells[1].type == LACE_TYPE_TEXT ? str_dup(row->cells[1].text.data) : NULL;
        col->type_name = row->cells[2].type == LACE_TYPE_TEXT ? str_dup(row->cells[2].text.data) : NULL;
        col->nullable = row->cells[3].type == LACE_TYPE_INT ? !row->cells[3].int_val : true;
        col->primary_key = row->cells[5].type == LACE_TYPE_INT ? row->cells[5].int_val > 0 : false;
        col->default_val = cell_text_dup(&row->cells[4]);
        col->generated = hidden == 2 || hidden == 3;
      }
      break;

    case LACE_DRIVER_POSTGRES:
      /* name, type, nullable, default, primary_key, auto_increment */
      if (result->num_columns >= 6 && row->num_cells >= 6) {
        col->name = cell_text_dup(&row->cells[0]);
        col->type_name = cell_text_dup(&row->cells[1]);
        col->nullable = row->cells[2].type == LACE_TYPE_INT && row->cells[2].int_val != 0;
        col->default_val = cell_text_dup(&row->cells[3]);
        col->primary_key = row->cells[4].type == LACE_TYPE_INT && row->cells[4].int_val != 0;
        col->auto_increment = row->cells[5].type == LACE_TYPE_INT && row->cells[5].int_val != 0;
        col->generated = row->num_cells >= 7 && row->cells[6].type == LACE_TYPE_INT &&
                         row->cells[6].int_val != 0;
      }
      break;

    case LACE_DRIVER_MYSQL:
    case LACE_DRIVER_MARIADB:
      /* DESCRIBE: Field, Type, Null, Key, Default, Extra */
      if (result->num_columns >= 4 && row->num_cells >= 4) {
        col->name = row->cells[0].type == LACE_TYPE_TEXT ? str_dup(row->cells[0].text.data) : NULL;
        col->type_name = row->cells[1].type == LACE_TYPE_TEXT ? str_dup(row->cells[1].text.data) : NULL;
        col->nullable = row->cells[2].type == LACE_TYPE_TEXT && row->cells[2].text.data &&
                        strcmp(row->cells[2].text.data, "YES") == 0;
        col->primary_key = row->cells[3].type == LACE_TYPE_TEXT && row->cells[3].text.data &&
                           strcmp(row->cells[3].text.data, "PRI") == 0;
        if (row->num_cells >= 6) {
          col->default_val = cell_text_dup(&row->cells[4]);
          col->auto_increment = row->cells[5].type == LACE_TYPE_TEXT && row->cells[5].text.data &&
                                strstr(row->cells[5].text.data, "auto_increment") != NULL;
          col->generated = row->cells[5].type == LACE_TYPE_TEXT && row->cells[5].text.data &&
                           strstr(row->cells[5].text.data, "GENERATED") != NULL &&
                           strstr(row->cells[5].text.data, "DEFAULT_GENERATED") == NULL;
        }
      }
      break;

    default:
      break;
    }
    col->type = lace_type_from_name(col->type_name, driver);
  }

  if (driver == LACE_DRIVER_SQLITE) {
    /* Drop hidden columns (left nameless above) */
    size_t kept = 0;
    for (size_t i = 0; i < sch->num_columns; i++) {
      if (sch->columns[i].name) {
        sch->columns[kept++] = sch->columns[i];
      }
    }
    sch->num_columns = kept;
  }

  if (driver == LACE_DRIVER_SQLITE) {
    /* A sole INTEGER PRIMARY KEY column aliases the rowid and is assigned
     * automatically when omitted from an INSERT */
    size_t num_pk = 0, pk_idx = 0;
    for (size_t i = 0; i < sch->num_columns; i++) {
      if (sch->columns[i].primary_key) {
        num_pk++;
        pk_idx = i;
      }
    }
    const char *type = num_pk == 1 ? sch->columns[pk_idx].type_name : NULL;
    if (type && str_eq_nocase(type, "INTEGER"))
      sch->columns[pk_idx].auto_increment = true;
  }

  lace_result_free(result);
  *schema = sch;
  return LACE_OK;
}

/* ==========================================================================
 * Data Queries
 * ========================================================================== */

int lace_query(lace_client_t *client, int conn_id, const char *table,
               size_t offset, size_t limit, LaceResult **result) {
  if (!client || !table || !result) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  LaceDriver driver;
  int drv_err = get_connection_driver(client, conn_id, &driver);
  if (drv_err != LACE_OK) {
    return drv_err;
  }
  char *escaped_table = lace_quote_table(driver, table);
  if (!escaped_table) {
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  char *sql = str_printf("SELECT * FROM %s LIMIT %zu OFFSET %zu",
                         escaped_table, limit > 0 ? limit : 500, offset);
  free(escaped_table);
  if (!sql) {
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  int err = lace_exec(client, conn_id, sql, result);
  free(sql);
  return err;
}

int lace_count(lace_client_t *client, int conn_id, const char *table,
               size_t *count, bool *approximate) {
  if (!client || !table || !count) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  LaceDriver driver;
  int drv_err = get_connection_driver(client, conn_id, &driver);
  if (drv_err != LACE_OK) {
    return drv_err;
  }

  /* Exact COUNT(*) is a full scan on PostgreSQL and InnoDB: minutes on big
   * tables. When the caller allows it, use the catalog estimate for large
   * tables; callers refine to an exact count only when they need one. */
  bool allow_estimate = approximate && *approximate;
  if (approximate) {
    *approximate = false;
  }
  if (allow_estimate && driver != LACE_DRIVER_SQLITE) {
    char *est_sql = NULL;
    if (driver == LACE_DRIVER_POSTGRES) {
      char *ident = lace_quote_table(driver, table);
      char *lit = ident ? sql_string_literal(ident, driver) : NULL;
      if (lit) {
        est_sql = str_printf("SELECT reltuples::bigint FROM pg_class "
                             "WHERE oid = to_regclass(%s)", lit);
      }
      free(ident);
      free(lit);
    } else {
      char *lit = sql_string_literal(table, driver);
      if (lit) {
        est_sql = str_printf("SELECT TABLE_ROWS FROM information_schema.TABLES "
                             "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = %s",
                             lit);
      }
      free(lit);
    }
    LaceResult *est = NULL;
    if (est_sql && lace_exec(client, conn_id, est_sql, &est) == LACE_OK && est &&
        est->num_rows > 0 && est->rows && est->rows[0].num_cells > 0 &&
        est->rows[0].cells) {
      LaceValue *v = &est->rows[0].cells[0];
      int64_t n = v->type == LACE_TYPE_INT     ? v->int_val
                  : v->type == LACE_TYPE_FLOAT ? (int64_t)v->float_val
                  : (v->type == LACE_TYPE_TEXT && v->text.data)
                      ? strtoll(v->text.data, NULL, 10)
                      : -1;
      if (n >= 100000) {
        *count = (size_t)n;
        *approximate = true;
        lace_result_free(est);
        free(est_sql);
        return LACE_OK;
      }
    }
    lace_result_free(est);
    free(est_sql);
  }
  char *escaped_table = lace_quote_table(driver, table);
  if (!escaped_table) {
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  char *sql = str_printf("SELECT COUNT(*) FROM %s", escaped_table);
  free(escaped_table);
  if (!sql) {
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);
  free(sql);
  if (err != LACE_OK) {
    return err;
  }

  if (!result || result->num_rows == 0) {
    lace_result_free(result);
    return lace_fail(LACE_ERR_INTERNAL_ERROR, NULL);
  }

  LaceRow *row = &result->rows[0];
  if (!row->cells || row->num_cells == 0) {
    lace_result_free(result);
    return lace_fail(LACE_ERR_INTERNAL_ERROR, NULL);
  }

  /* Extract count from first column of first row */
  LaceValue *cell = &row->cells[0];
  if (cell->type == LACE_TYPE_INT) {
    *count = (size_t)cell->int_val;
  } else if (cell->type == LACE_TYPE_FLOAT) {
    *count = (size_t)cell->float_val;
  } else if (cell->type == LACE_TYPE_TEXT && cell->text.data) {
    *count = (size_t)strtoll(cell->text.data, NULL, 10);
  } else {
    *count = 0;
  }

  lace_result_free(result);
  return LACE_OK;
}

/* The ON DELETE rule as the catalogs spell it, in words */
static const char *delete_rule_word(const char *rule) {
  if (!rule || !*rule) return "NO ACTION";
  if (!rule[1]) { /* PostgreSQL's confdeltype */
    switch (rule[0]) {
    case 'c': return "CASCADE";
    case 'n': return "SET NULL";
    case 'd': return "SET DEFAULT";
    case 'r': return "RESTRICT";
    default: return "NO ACTION";
    }
  }
  return rule;
}

int lace_table_dependents(lace_client_t *client, int conn_id, const char *table,
                          LaceDependent **deps, size_t *count) {
  if (deps) *deps = NULL;
  if (count) *count = 0;
  if (!client || !table || !deps || !count) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }
  LaceDriver driver;
  int drv_err = get_connection_driver(client, conn_id, &driver);
  if (drv_err != LACE_OK) {
    return drv_err;
  }
  char *sql = NULL;
  switch (driver) {
  case LACE_DRIVER_SQLITE: {
    /* pragma_foreign_key_list as a table-valued function (SQLite 3.16+):
     * every table's foreign keys, kept where they point at ours */
    char *lit = sql_string_literal(table, driver);
    if (lit)
      sql = str_printf("SELECT m.name, f.on_delete FROM sqlite_master m "
                       "JOIN pragma_foreign_key_list(m.name) f "
                       "WHERE m.type = 'table' AND f.\"table\" = %s COLLATE NOCASE",
                       lit);
    free(lit);
    break;
  }
  case LACE_DRIVER_POSTGRES: {
    /* to_regclass() as in lace_get_schema; the referencing table's name
     * comes back the way the search_path spells it */
    char *ident = lace_quote_table(driver, table);
    char *lit = ident ? sql_string_literal(ident, driver) : NULL;
    if (lit)
      sql = str_printf("SELECT c.conrelid::regclass::text, c.confdeltype::text "
                       "FROM pg_catalog.pg_constraint c "
                       "WHERE c.contype = 'f' AND c.confrelid = to_regclass(%s)",
                       lit);
    free(ident);
    free(lit);
    break;
  }
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB: {
    char *lit = sql_string_literal(table, driver);
    if (lit)
      sql = str_printf("SELECT TABLE_NAME, DELETE_RULE "
                       "FROM information_schema.REFERENTIAL_CONSTRAINTS "
                       "WHERE CONSTRAINT_SCHEMA = DATABASE() "
                       "AND UNIQUE_CONSTRAINT_SCHEMA = DATABASE() "
                       "AND REFERENCED_TABLE_NAME = %s",
                       lit);
    free(lit);
    break;
  }
  default:
    return lace_fail(LACE_ERR_INTERNAL_ERROR, NULL);
  }
  if (!sql)
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);
  free(sql);
  if (err != LACE_OK)
    return err;
  size_t n = result ? result->num_rows : 0;
  LaceDependent *out = safe_calloc(n + 1, sizeof *out);
  size_t k = 0;
  for (size_t i = 0; result && result->rows && i < n; i++) {
    const LaceRow *r = &result->rows[i];
    if (r->num_cells < 2 || r->cells[0].is_null) continue;
    char *name = lace_value_to_edit_text(&r->cells[0]);
    char *rule = r->cells[1].is_null ? NULL : lace_value_to_edit_text(&r->cells[1]);
    if (!name) {
      free(rule);
      continue;
    }
    out[k].table = name;
    out[k].on_delete = str_dup(delete_rule_word(rule));
    free(rule);
    k++;
  }
  lace_result_free(result);
  if (!k) {
    free(out);
    out = NULL;
  }
  *deps = out;
  *count = k;
  return LACE_OK;
}

int lace_exec(lace_client_t *client, int conn_id, const char *sql,
              LaceResult **result) {
  if (result) {
    *result = NULL;
  }
  if (!client || !sql) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);
  cJSON_AddStringToObject(params, "sql", sql);
  set_thread_last_sql(sql);
  lace_set_thread_tx_state(-1); /* Unknown unless the reply says */

  cJSON *resp = NULL;
  int err = lace_rpc_call(client, "query", params, &resp);
  cJSON_Delete(params);

  if (err != LACE_OK) {
    return err;
  }

  /* The transaction state, also for callers that want no result */
  bool in_tx = cJSON_IsTrue(cJSON_GetObjectItem(resp, "in_transaction"));
  if (cJSON_GetObjectItem(resp, "in_transaction")) {
    lace_set_thread_tx_state(
        !in_tx ? 0
               : cJSON_IsTrue(cJSON_GetObjectItem(resp, "transaction_failed"))
                     ? 2
                     : 1);
  }

  if (result) {
    /* Check if it's a select result */
    cJSON *type = cJSON_GetObjectItem(resp, "type");
    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "select") == 0) {
      cJSON *data = cJSON_GetObjectItem(resp, "data");
      *result = lace_rpc_parse_result(data);
      if (!*result) {
        /* Success always comes with a result; never LACE_OK and NULL */
        cJSON_Delete(resp);
        return lace_fail(LACE_ERR_PARSE_ERROR, "Malformed query reply");
      }
    } else {
      /* Non-select: create minimal result with affected count */
      *result = safe_calloc(1, sizeof(LaceResult));
      cJSON *affected = cJSON_GetObjectItem(resp, "affected");
      if (affected && cJSON_IsNumber(affected)) {
        (*result)->total_rows = (size_t)affected->valuedouble;
        (*result)->rows_affected = (int64_t)affected->valuedouble;
      }
    }
    if (*result) {
      (*result)->in_transaction = in_tx;
    }
  }

  cJSON_Delete(resp);
  return LACE_OK;
}

int lace_cancel_query(lace_client_t *client, int conn_id) {
  if (!client) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);

  /* The query being cancelled is waiting on its own link; the cancel takes
   * another one from the pool, so it never waits behind the query. A stdio
   * daemon has a single link: if it is busy, try_call fails at once rather
   * than queueing the cancel after the query it should stop. */
  cJSON *resp = NULL;
  int err = lace_rpc_try_call(client, "cancel", params, &resp);
  pthread_mutex_lock(&client->lock);
  bool single = client->single_link;
  pthread_mutex_unlock(&client->lock);
  if (err == LACE_ERR_TIMEOUT && single) {
    /* The stdio link is busy, most likely with the very statement to
     * cancel: send the cancel down the same pipe as a notification */
    err = lace_rpc_notify(client, "cancel", params);
  }
  cJSON_Delete(params);
  cJSON_Delete(resp);

  return err;
}

/* ==========================================================================
 * Data Mutations
 * ========================================================================== */

/* A value too large to load holds only a placeholder label (omitted > 0).
 * Writing or matching it would store or compare the label, not the data. */
static bool values_loaded(const LaceValue *values, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (values[i].omitted) {
      lace_set_thread_error("A value too large to load cannot be written or "
                            "used to find a row");
      return false;
    }
  }
  return true;
}

static bool pk_loaded(const LacePkValue *pk, size_t num_pk) {
  for (size_t i = 0; i < num_pk; i++) {
    if (!values_loaded(&pk[i].value, 1)) {
      return false;
    }
  }
  return true;
}

int lace_update(lace_client_t *client, int conn_id, const char *table,
                const LacePkValue *pk, size_t num_pk,
                const char *column, const LaceValue *value) {
  if (!client || !table || !pk || num_pk == 0 || !column || !value) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }
  if (!values_loaded(value, 1) || !pk_loaded(pk, num_pk)) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver;
  int drv_err = get_connection_driver(client, conn_id, &driver);
  if (drv_err != LACE_OK) {
    return drv_err;
  }
  char *esc_table = lace_quote_table(driver, table);
  char *esc_col = lace_quote_identifier(driver, column);
  char *val_sql = value_to_sql(value, driver);
  StringBuilder *sb = sb_new(256);
  if (!esc_table || !esc_col || !val_sql || !sb) {
    free(esc_table);
    free(esc_col);
    str_secure_free(val_sql);
    sb_free(sb);
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  sb_printf(sb, "UPDATE %s SET %s = ", esc_table, esc_col);
  sb_append(sb, val_sql);
  sb_append(sb, " WHERE ");
  free(esc_table);
  free(esc_col);
  str_secure_free(val_sql);

  if (!append_pk_where(sb, pk, num_pk, driver)) {
    sb_free(sb);
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }
  return exec_mutation(client, conn_id, sb_finish(sb), EXPECT_ONE);
}

int lace_delete(lace_client_t *client, int conn_id, const char *table,
                const LacePkValue *pk, size_t num_pk) {
  if (!client || !table || !pk || num_pk == 0) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }
  if (!pk_loaded(pk, num_pk)) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver;
  int drv_err = get_connection_driver(client, conn_id, &driver);
  if (drv_err != LACE_OK) {
    return drv_err;
  }
  char *esc_table = lace_quote_table(driver, table);
  StringBuilder *sb = sb_new(128);
  if (!esc_table || !sb) {
    free(esc_table);
    sb_free(sb);
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  sb_printf(sb, "DELETE FROM %s WHERE ", esc_table);
  free(esc_table);

  if (!append_pk_where(sb, pk, num_pk, driver)) {
    sb_free(sb);
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }
  return exec_mutation(client, conn_id, sb_finish(sb), EXPECT_ONE);
}

int lace_insert(lace_client_t *client, int conn_id, const char *table,
                const char **columns, const LaceValue *values, size_t num_columns) {
  if (!client || !table ||
      (num_columns > 0 && (!columns || !values))) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }
  if (num_columns > 0 && !values_loaded(values, num_columns)) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver;
  int drv_err = get_connection_driver(client, conn_id, &driver);
  if (drv_err != LACE_OK) {
    return drv_err;
  }
  if (num_columns == 0) {
    /* Every column takes its default */
    char *esc = lace_quote_table(driver, table);
    if (!esc) {
      return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
    }
    bool mysql = driver == LACE_DRIVER_MYSQL || driver == LACE_DRIVER_MARIADB;
    char *sql = str_printf(mysql ? "INSERT INTO %s () VALUES ()"
                                 : "INSERT INTO %s DEFAULT VALUES",
                           esc);
    free(esc);
    if (!sql) {
      return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
    }
    return exec_mutation(client, conn_id, sql, EXPECT_SOME);
  }

  char *esc_table = lace_quote_table(driver, table);
  StringBuilder *sb = sb_new(256);
  if (!esc_table || !sb) {
    free(esc_table);
    sb_free(sb);
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }

  sb_printf(sb, "INSERT INTO %s (", esc_table);
  free(esc_table);

  for (size_t i = 0; i < num_columns; i++) {
    char *esc_col = lace_quote_identifier(driver, columns[i]);
    if (!esc_col) {
      sb_free(sb);
      return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
    }
    sb_printf(sb, "%s%s", i > 0 ? ", " : "", esc_col);
    free(esc_col);
  }

  sb_append(sb, ") VALUES (");
  for (size_t i = 0; i < num_columns; i++) {
    char *val_sql = value_to_sql(&values[i], driver);
    if (!val_sql) {
      sb_free(sb);
      return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
    }
    if (i > 0) {
      sb_append(sb, ", ");
    }
    sb_append(sb, val_sql);
    str_secure_free(val_sql);
  }
  sb_append(sb, ")");

  /* Auto-generated keys are not returned (would need RETURNING/LAST_INSERT_ID) */
  return exec_mutation(client, conn_id, sb_finish(sb), EXPECT_SOME);
}

int lace_select_by_pk(lace_client_t *client, int conn_id, const char *table,
                       const LacePkValue *pk, size_t num_pk,
                       const char **columns, size_t num_columns,
                       LaceResult **result) {
  if (result) *result = NULL;
  if (!client || !table || !pk || num_pk == 0 ||
      !columns || num_columns == 0 || !result) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }
  if (!pk_loaded(pk, num_pk)) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver;
  int drv_err = get_connection_driver(client, conn_id, &driver);
  if (drv_err != LACE_OK) {
    return drv_err;
  }
  StringBuilder *sb = sb_new(256);
  if (!sb) {
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }
  sb_append(sb, "SELECT ");
  for (size_t i = 0; i < num_columns; i++) {
    char *col = lace_quote_identifier(driver, columns[i]);
    if (!col) {
      sb_free(sb);
      return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
    }
    sb_printf(sb, "%s%s", i > 0 ? ", " : "", col);
    free(col);
  }
  char *esc_table = lace_quote_table(driver, table);
  if (!esc_table) {
    sb_free(sb);
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }
  sb_printf(sb, " FROM %s WHERE ", esc_table);
  free(esc_table);
  /* Same WHERE builder as lace_update/lace_delete, so this sees exactly the
   * rows a following mutation would touch. LIMIT 2 detects "more than one". */
  if (!append_pk_where(sb, pk, num_pk, driver)) {
    sb_free(sb);
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }
  sb_append(sb, " LIMIT 2");
  char *sql = sb_finish(sb);
  if (!sql) {
    return lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
  }
  int err = lace_exec(client, conn_id, sql, result);
  free(sql);
  return err;
}

void lace_pk_free(LacePkValue *pk, size_t num_pk) {
  if (!pk) return;

  for (size_t i = 0; i < num_pk; i++) {
    free(pk[i].column);
    lace_value_free(&pk[i].value);
  }
  free(pk);
}

/* ==========================================================================
 * Utility Functions
 * ========================================================================== */

int lace_ping(lace_client_t *client) {
  if (!client) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "ping", NULL, &result);
  cJSON_Delete(result);
  return err;
}

int lace_version(lace_client_t *client, char **version) {
  if (!client || !version) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "version", NULL, &result);
  if (err != LACE_OK) {
    return err;
  }

  cJSON *ver = cJSON_GetObjectItem(result, "daemon_version");
  if (ver && cJSON_IsString(ver)) {
    *version = str_dup(ver->valuestring);
  } else {
    *version = str_dup("unknown");
  }

  cJSON_Delete(result);
  return LACE_OK;
}

int lace_shutdown(lace_client_t *client) {
  if (!client) {
    return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  }
  if (!client->connected) {
    return lace_fail(LACE_ERR_CONNECTION_CLOSED, NOT_CONNECTED_MSG);
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "shutdown", NULL, &result);
  cJSON_Delete(result);
  return err;
}
