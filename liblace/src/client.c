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
#include <errno.h>
#include <math.h>
#include <locale.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* Default timeout in milliseconds */
#define DEFAULT_TIMEOUT_MS 30000

/* Socket utilities (implemented in socket.c) */
extern int lace_connect_unix(const char *path, int timeout_ms);
extern int lace_connect_tcp(const char *host, int port, int timeout_ms);
extern int lace_find_daemon_socket(void);
extern char *lace_get_default_socket_path(void);
extern bool lace_daemon_is_running(const char *socket_path);

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

static void sql_key_init(void) {
  pthread_key_create(&g_sql_key, free);
}

static void set_thread_last_sql(const char *sql) {
  pthread_once(&g_sql_once, sql_key_init);
  free(pthread_getspecific(g_sql_key));
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
static void set_error(lace_client_t *client, const char *msg) {
  (void)client;
  lace_set_thread_error(msg);
}

/* Parse driver type from connection string */
static LaceDriver parse_driver(const char *connstr) {
  ConnString *cs = connstr_parse(connstr, NULL);
  if (!cs) {
    return LACE_DRIVER_SQLITE; /* Default */
  }

  LaceDriver driver = LACE_DRIVER_SQLITE;
  /* Same scheme names connstr.c and the daemon accept ("pg" fell through
   * to SQLite: sqlite_master queries and SQLite literals on PostgreSQL) */
  if (str_eq(cs->driver, "postgres") || str_eq(cs->driver, "postgresql") ||
      str_eq(cs->driver, "pg")) {
    driver = LACE_DRIVER_POSTGRES;
  } else if (str_eq(cs->driver, "mysql")) {
    driver = LACE_DRIVER_MYSQL;
  } else if (str_eq(cs->driver, "mariadb")) {
    driver = LACE_DRIVER_MARIADB;
  }

  connstr_free(cs);
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
  if (client->num_connections < LACE_CLIENT_MAX_CONNECTIONS) {
    client->connections[client->num_connections].conn_id = conn_id;
    client->connections[client->num_connections].driver = driver;
    client->num_connections++;
  }
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

/* Get driver type for a connection */
static LaceDriver get_connection_driver(lace_client_t *client, int conn_id) {
  LaceDriver driver = LACE_DRIVER_SQLITE; /* Default */
  pthread_mutex_lock(&client->lock);
  for (size_t i = 0; i < client->num_connections; i++) {
    if (client->connections[i].conn_id == conn_id) {
      driver = client->connections[i].driver;
      break;
    }
  }
  pthread_mutex_unlock(&client->lock);
  return driver;
}

/* Get identifier quote character for driver */
static char get_quote_char(LaceDriver driver) {
  return (driver == LACE_DRIVER_MYSQL || driver == LACE_DRIVER_MARIADB)
      ? '`' : '"';
}

/* Escape identifier (caller must free) */
static char *escape_identifier(const char *name, LaceDriver driver) {
  if (!name) return NULL;
  char q = get_quote_char(driver);

  /* Count occurrences of quote char that need escaping */
  size_t len = strlen(name);
  size_t extra = 0;
  for (size_t i = 0; i < len; i++) {
    if (name[i] == q) extra++;
  }

  /* Allocate: quotes + original + doubled quotes + null */
  char *escaped = malloc(len + extra + 3);
  if (!escaped) return NULL;

  char *p = escaped;
  *p++ = q;
  for (size_t i = 0; i < len; i++) {
    if (name[i] == q) *p++ = q; /* Double the quote */
    *p++ = name[i];
  }
  *p++ = q;
  *p = '\0';

  return escaped;
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
    return escape_identifier(ref, driver);

  char *schema = NULL, *name = NULL;
  if (!ref_split(ref, &schema, &name))
    return NULL;
  char *qname = escape_identifier(name, driver);
  char *result = NULL;
  if (schema) {
    char *qschema = escape_identifier(schema, driver);
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
 * context. Embedded NUL bytes cannot be represented and end the value. */
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
  StringBuilder *sb = sb_new(strlen(value) + 3);
  if (!sb) {
    return NULL;
  }
  sb_append_char(sb, '\'');
  for (const char *p = value; *p; p++) {
    if (*p == '\'') {
      sb_append_char(sb, '\'');
    }
    sb_append_char(sb, *p);
  }
  sb_append_char(sb, '\'');
  return sb_finish(sb);
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
    sb_append(sb, "'\\x");
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
    char *col = escape_identifier(pk[i].column, driver);
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
    return LACE_ERR_OUT_OF_MEMORY;
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

/* Get directory of the current executable */
static char *get_exe_dir(void) {
  char exe_path[4096];
#ifdef __APPLE__
  uint32_t size = sizeof(exe_path);
  if (_NSGetExecutablePath(exe_path, &size) != 0) {
    return NULL;
  }
  char *resolved = realpath(exe_path, NULL);
  if (resolved) {
    snprintf(exe_path, sizeof(exe_path), "%s", resolved);
    free(resolved);
  }
#else
  ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (len <= 0) {
    return NULL;
  }
  exe_path[len] = '\0';
#endif

  /* Find last slash to get directory */
  char *last_slash = strrchr(exe_path, '/');
  if (last_slash) {
    *last_slash = '\0';
    return str_dup(exe_path);
  }
  return NULL;
}

/* Find daemon executable */
static char *find_daemon(const char *daemon_path) {
  if (daemon_path) {
    /* Check if the provided path exists and is executable */
    if (access(daemon_path, X_OK) == 0) {
      return str_dup(daemon_path);
    }
    return NULL;
  }

  /* First, try paths relative to the executable */
  char *exe_dir = get_exe_dir();
  if (exe_dir) {
    /* Search paths relative to executable directory */
    const char *exe_relative_paths[] = {
        "../../../../laced/build/laced",  /* gui/gtk/build/ -> laced/build/ */
        "../../../laced/build/laced",  /* tui/ncurses/build/ -> laced/build/ */
        "../../laced/build/laced",     /* tui/build/ -> laced/build/ */
        "../laced/build/laced",        /* Same level as laced/ */
        "./laced",                     /* Same directory as executable */
        NULL
    };

    for (int i = 0; exe_relative_paths[i]; i++) {
      char full_path[4096];
      snprintf(full_path, sizeof(full_path), "%s/%s", exe_dir, exe_relative_paths[i]);
      if (access(full_path, X_OK) == 0) {
        free(exe_dir);
        /* Resolve to absolute path */
        char *real = realpath(full_path, NULL);
        return real ? real : str_dup(full_path);
      }
    }
    free(exe_dir);
  }

  /* Search in common locations relative to CWD */
  const char *search_paths[] = {
      "./laced/build/laced",      /* Development build */
      "./build/laced",            /* Local build */
      "../laced/build/laced",     /* Sibling directory */
      "../../laced/build/laced",  /* Frontend in frontends/ subdir */
      "/usr/local/bin/laced",     /* Standard install */
      "/usr/bin/laced",           /* System install */
      NULL
  };

  for (int i = 0; search_paths[i]; i++) {
    if (access(search_paths[i], X_OK) == 0) {
      return str_dup(search_paths[i]);
    }
  }

  /* Try PATH */
  const char *path_env = getenv("PATH");
  if (path_env) {
    char *path_copy = str_dup(path_env);
    if (path_copy) {
      char *saveptr;
      char *dir = strtok_r(path_copy, ":", &saveptr);
      while (dir) {
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/laced", dir);
        if (access(full_path, X_OK) == 0) {
          free(path_copy);
          return str_dup(full_path);
        }
        dir = strtok_r(NULL, ":", &saveptr);
      }
      free(path_copy);
    }
  }

  return NULL;
}

/* Spawn daemon process in stdio mode (pipes).
 * The daemon automatically exits when stdin closes (client terminates). */
static bool spawn_daemon_stdio(lace_client_t *client, const char *daemon_path) {
  char *daemon_exe = find_daemon(daemon_path);
  if (!daemon_exe) {
    set_error(client, "Daemon executable not found");
    return false;
  }

  /* Create pipes for communication */
  int to_daemon[2];   /* Parent writes, daemon reads (daemon's stdin) */
  int from_daemon[2]; /* Daemon writes, parent reads (daemon's stdout) */

  if (pipe(to_daemon) < 0) {
    free(daemon_exe);
    set_error(client, "Failed to create pipe");
    return false;
  }

  if (pipe(from_daemon) < 0) {
    close(to_daemon[0]);
    close(to_daemon[1]);
    free(daemon_exe);
    set_error(client, "Failed to create pipe");
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(to_daemon[0]);
    close(to_daemon[1]);
    close(from_daemon[0]);
    close(from_daemon[1]);
    free(daemon_exe);
    set_error(client, "Fork failed");
    return false;
  }

  if (pid == 0) {
    /* Child process - become the daemon */

    /* Set up stdin from pipe */
    close(to_daemon[1]); /* Close write end */
    dup2(to_daemon[0], STDIN_FILENO);
    close(to_daemon[0]);

    /* Set up stdout to pipe */
    close(from_daemon[0]); /* Close read end */
    dup2(from_daemon[1], STDOUT_FILENO);
    close(from_daemon[1]);

    /* Redirect stderr to /dev/null (or keep for debugging) */
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    /* Execute daemon in stdio mode */
    execl(daemon_exe, daemon_exe, "--stdio", (char *)NULL);

    /* If exec fails, exit */
    _exit(127);
  }

  /* Parent process */
  free(daemon_exe);

  /* Close unused pipe ends */
  close(to_daemon[0]);   /* Close read end of to_daemon */
  close(from_daemon[1]); /* Close write end of from_daemon */

  /* The pipe pair is this client's one and only link: nothing else can
   * reach a stdio daemon, so calls take turns on it */
  LaceLink *link = lace_link_new(from_daemon[0], to_daemon[1], false);
  pthread_mutex_lock(&client->lock);
  link->next = client->idle_links;
  client->idle_links = link;
  client->num_idle++;
  client->num_links++;
  client->single_link = true;
  pthread_mutex_unlock(&client->lock);

  client->daemon_pid = pid;
  client->connected = true;
  client->owns_daemon = true;
  client->conn_mode = LACE_CONN_SPAWN;

  /* Verify daemon is responsive */
  cJSON *result = NULL;
  int err = lace_rpc_call(client, "ping", NULL, &result);
  cJSON_Delete(result);

  if (err != LACE_OK) {
    lace_client_close_streams(client);
    client->connected = false;
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    client->daemon_pid = 0;
    set_error(client, "Daemon not responding");
    return false;
  }

  return true;
}

/* Spawn daemon process in Unix socket mode.
 * We spawn the daemon, then connect to it via Unix socket.
 * This is preferred over stdio mode for better multi-client support. */
static bool spawn_daemon_unix(lace_client_t *client, const char *daemon_path,
                              char **socket_path_out) {
  char *daemon_exe = find_daemon(daemon_path);
  if (!daemon_exe) {
    set_error(client, "Daemon executable not found");
    return false;
  }

  /* Get the socket path the daemon will use */
  char *socket_path = lace_get_default_socket_path();
  if (!socket_path) {
    free(daemon_exe);
    set_error(client, "Failed to determine socket path");
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    free(daemon_exe);
    free(socket_path);
    set_error(client, "Fork failed");
    return false;
  }

  if (pid == 0) {
    /* Child process - become the daemon */

    /* Create new session to detach from terminal */
    setsid();

    /* Redirect stdin/stdout/stderr to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) {
        close(devnull);
      }
    }

    /* Execute daemon in Unix socket mode. It manages its own lifetime:
     * --idle-timeout (user setting, 0 = run until signalled). */
    if (client->idle_timeout > 0) {
      char timeout_str[16];
      snprintf(timeout_str, sizeof(timeout_str), "%d", client->idle_timeout);
      execl(daemon_exe, daemon_exe, "--unix", socket_path, "--idle-timeout",
            timeout_str, (char *)NULL);
    } else {
      execl(daemon_exe, daemon_exe, "--unix", socket_path, (char *)NULL);
    }

    /* If exec fails, exit */
    _exit(127);
  }

  /* Parent process */
  free(daemon_exe);

  /* Wait for the daemon to create the socket (up to 5 seconds) */
  int max_wait_ms = 5000;
  int wait_interval_ms = 50;
  int waited_ms = 0;

  while (waited_ms < max_wait_ms) {
    if (lace_daemon_is_running(socket_path)) {
      break;
    }

    /* Check if child process died */
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result > 0) {
      /* Child exited */
      free(socket_path);
      set_error(client, "Daemon process exited unexpectedly");
      return false;
    }

    usleep((useconds_t)(wait_interval_ms * 1000));
    waited_ms += wait_interval_ms;
  }

  if (waited_ms >= max_wait_ms) {
    /* Timeout - kill the daemon */
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    free(socket_path);
    set_error(client, "Timeout waiting for daemon to start");
    return false;
  }

  *socket_path_out = socket_path;
  client->daemon_pid = pid;
  return true;
}

/* Spawn daemon process in TCP socket mode.
 * We spawn the daemon, then connect to it via TCP.
 * This allows network connections from other machines. */
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

  /* Build port string for exec */
  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", port);

  /* Build host:port string if host specified */
  char addr_str[128];
  if (host && host[0]) {
    snprintf(addr_str, sizeof(addr_str), "%s:%d", host, port);
  } else {
    snprintf(addr_str, sizeof(addr_str), "%d", port);
  }

  pid_t pid = fork();
  if (pid < 0) {
    free(daemon_exe);
    set_error(client, "Fork failed");
    return false;
  }

  if (pid == 0) {
    /* Child process - become the daemon */

    /* Detach from controlling terminal */
    setsid();

    /* Redirect stdin/stdout/stderr to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) {
        close(devnull);
      }
    }

    /* Execute daemon in TCP socket mode */
    if (client->idle_timeout > 0) {
      char timeout_str[16];
      snprintf(timeout_str, sizeof(timeout_str), "%d", client->idle_timeout);
      execl(daemon_exe, daemon_exe, "--tcp", addr_str,
            "--idle-timeout", timeout_str, (char *)NULL);
    } else {
      execl(daemon_exe, daemon_exe, "--tcp", addr_str, (char *)NULL);
    }

    /* If exec fails, exit */
    _exit(127);
  }

  /* Parent process */
  free(daemon_exe);

  /* Wait for daemon to start listening (with timeout) */
  int wait_attempts = 50; /* 50 * 20ms = 1 second max */
  const char *connect_host = (host && host[0]) ? host : "127.0.0.1";
  while (wait_attempts > 0) {
    int fd = lace_connect_tcp(connect_host, port, 100);
    if (fd >= 0) {
      close(fd);
      break;
    }
    usleep(20000); /* 20ms */
    wait_attempts--;

    /* Check if daemon died */
    int status;
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w != 0) {
      if (w < 0) {
        kill(pid, SIGKILL); /* waitpid failed: make sure it is gone */
        waitpid(pid, NULL, 0);
      }
      set_error(client, "Daemon failed to start");
      return false;
    }
  }

  if (wait_attempts == 0) {
    /* Timeout - daemon didn't start */
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    set_error(client, "Daemon startup timeout");
    return false;
  }

  /* Connect to daemon via TCP */
  int fd = lace_connect_tcp(connect_host, port, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
  if (fd < 0) {
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    set_error(client, "Failed to connect to spawned daemon");
    return false;
  }

  if (!lace_client_attach_socket(client, fd)) {
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
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
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
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
  bool got = false;
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    got = read(fd, raw, sizeof(raw)) == (ssize_t)sizeof(raw);
    close(fd);
  }
  if (!got) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t seed = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 20) ^
                    ((uint64_t)getpid() << 40) ^ (uint64_t)(uintptr_t)out;
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
  pthread_mutex_init(&client->lock, NULL);
  pthread_cond_init(&client->link_released, NULL);
  make_client_id(client->client_id, sizeof(client->client_id));
}

static lace_client_t *client_alloc(LaceConnMode mode) {
  lace_client_t *client = safe_calloc(1, sizeof(lace_client_t));
  lace_client_init_common(client);
  client->conn_mode = mode;
  return client;
}

LaceLink *lace_link_new(int rfd, int wfd, bool is_socket) {
  LaceLink *l = safe_calloc(1, sizeof(LaceLink));
  l->rfd = rfd;
  l->wfd = wfd;
  l->is_socket = is_socket;
  l->next_id = 1;
  return l;
}

void lace_link_free(LaceLink *link) {
  if (!link) {
    return;
  }
  if (link->rfd >= 0) {
    close(link->rfd);
  }
  if (link->wfd >= 0 && link->wfd != link->rfd) {
    close(link->wfd);
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
    lace_link_free(l);
    l = next;
  }
}

bool lace_client_attach_socket(lace_client_t *client, int fd) {
  if (fd < 0) {
    return false;
  }
  LaceLink *l = lace_link_new(fd, fd, true);
  pthread_mutex_lock(&client->lock);
  l->next = client->idle_links;
  client->idle_links = l;
  client->num_idle++;
  client->num_links++;
  pthread_mutex_unlock(&client->lock);
  client->connected = true;
  return true;
}

static void kill_spawned_daemon(lace_client_t *client) {
  if (client->daemon_pid > 0) {
    kill(client->daemon_pid, SIGTERM);
    waitpid(client->daemon_pid, NULL, 0);
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
  int fd = lace_connect_unix(spawned_path, timeout_ms);
  if (fd < 0 || !lace_client_attach_socket(client, fd)) {
    kill_spawned_daemon(client);
    set_error(client, "Failed to connect to spawned daemon");
  } else {
    lace_client_set_socket_path(client, spawned_path);
    client->conn_mode = LACE_CONN_UNIX;
  }
  free(spawned_path);
}

lace_client_t *lace_client_create_ex(const char *daemon_path, LaceSpawnMode spawn_mode) {
  return lace_client_create_ex2(daemon_path, spawn_mode, 0);
}

lace_client_t *lace_client_create_ex2(const char *daemon_path,
                                      LaceSpawnMode spawn_mode,
                                      int idle_timeout) {
  lace_client_t *client = client_alloc(LACE_CONN_SPAWN);
  client->idle_timeout = idle_timeout;

  /* Prefer an already-running daemon */
  if (spawn_mode == LACE_SPAWN_UNIX || spawn_mode == LACE_SPAWN_NONE) {
    char *socket_path = lace_get_default_socket_path();
    if (socket_path && lace_daemon_is_running(socket_path)) {
      int fd = lace_connect_unix(socket_path, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
      if (fd >= 0 && lace_client_attach_socket(client, fd)) {
        lace_client_set_socket_path(client, socket_path);
        free(socket_path);
        client->conn_mode = LACE_CONN_UNIX;
        return client;
      }
    }
    free(socket_path);
  }
  if (spawn_mode == LACE_SPAWN_TCP || spawn_mode == LACE_SPAWN_NONE) {
    int fd = lace_connect_tcp(NULL, LACE_DEFAULT_PORT,
                              LACE_DEFAULT_CONNECT_TIMEOUT_MS);
    if (fd >= 0 && lace_client_attach_socket(client, fd)) {
      client->conn_mode = LACE_CONN_TCP;
      client->tcp_host = str_dup("127.0.0.1");
      client->tcp_port = LACE_DEFAULT_PORT;
      return client;
    }
  }

  /* NONE mode: don't spawn, just fail if no daemon found */
  if (spawn_mode == LACE_SPAWN_NONE) {
    set_error(client, "No daemon found (spawn disabled)");
    return client;
  }

  /* No running daemon - spawn one with the preferred mode */
  switch (spawn_mode) {
  case LACE_SPAWN_STDIO:
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

lace_client_t *lace_client_create(const char *daemon_path) {
  return lace_client_create_ex(daemon_path, LACE_SPAWN_UNIX);
}

/* Create client from existing socket fd */
static lace_client_t *create_from_socket(int fd, LaceConnMode mode) {
  if (fd < 0) {
    return NULL;
  }
  lace_client_t *client = client_alloc(mode);
  if (!lace_client_attach_socket(client, fd)) {
    lace_client_destroy(client);
    return NULL;
  }
  return client;
}

lace_client_t *lace_client_create_with_config(const LaceClientConfig *config) {
  if (!config) {
    return lace_client_create(NULL);
  }

  int timeout = config->connect_timeout_ms > 0 ? config->connect_timeout_ms
                                               : LACE_DEFAULT_CONNECT_TIMEOUT_MS;

  switch (config->mode) {
  case LACE_CONN_SPAWN:
    return lace_client_create_ex2(config->daemon_path, config->spawn_mode,
                                  config->idle_timeout);

  case LACE_CONN_UNIX: {
    char *default_path = NULL;
    const char *path = config->socket_path;
    if (!path) {
      default_path = lace_get_default_socket_path();
      path = default_path;
    }
    int fd = path ? lace_connect_unix(path, timeout) : -1;

    lace_client_t *client = client_alloc(LACE_CONN_UNIX);
    if (fd >= 0 && lace_client_attach_socket(client, fd)) {
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
      client->conn_mode = LACE_CONN_SPAWN;
      client->idle_timeout = config->idle_timeout;
      spawn_and_attach(client, config->daemon_path, timeout);
    } else {
      set_error(client, "Failed to connect to Unix socket");
    }
    return client;
  }

  case LACE_CONN_TCP: {
    int fd = lace_connect_tcp(config->host, config->port, timeout);
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

  return NULL;
}

lace_client_t *lace_client_connect(const char *socket_path) {
  char *default_path = NULL;
  const char *path = socket_path;
  if (!path) {
    default_path = lace_get_default_socket_path();
    path = default_path;
  }
  int fd = path ? lace_connect_unix(path, LACE_DEFAULT_CONNECT_TIMEOUT_MS) : -1;
  lace_client_t *client = create_from_socket(fd, LACE_CONN_UNIX);
  if (client) {
    lace_client_set_socket_path(client, path);
  }
  free(default_path);
  return client;
}

lace_client_t *lace_client_connect_tcp(const char *host, int port) {
  int fd = lace_connect_tcp(host, port, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
  lace_client_t *client = create_from_socket(fd, LACE_CONN_TCP);
  if (client) {
    /* New links (one per concurrent call) go to the same endpoint */
    client->tcp_host = str_dup(host ? host : "127.0.0.1");
    client->tcp_port = port > 0 ? port : LACE_DEFAULT_PORT;
  }
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
  if (client->owns_daemon && client->daemon_pid > 0) {
    /* A stdio daemon belongs to this client alone; closing its stdin makes
     * it exit. Give it a moment, then make sure. */
    int waited_ms = 0;
    while (waitpid(client->daemon_pid, NULL, WNOHANG) == 0 && waited_ms < 1000) {
      usleep(50000);
      waited_ms += 50;
    }
    if (waited_ms >= 1000) {
      kill(client->daemon_pid, SIGTERM);
      waitpid(client->daemon_pid, NULL, 0);
    }
  } else if (client->daemon_pid > 0) {
    waitpid(client->daemon_pid, NULL, WNOHANG); /* Reap if it already exited */
  }

  pthread_cond_destroy(&client->link_released);
  pthread_mutex_destroy(&client->lock);
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

int lace_client_open_link(lace_client_t *client, LaceLink **out) {
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
    int fd = lace_connect_tcp(host, port, 1000);
    free(host);
    if (fd < 0) {
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
  int fd = path ? lace_connect_unix(path, 1000) : -1;
  if (fd >= 0) {
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
  if (!is_default) {
    return 0;
  }
  if (client->daemon_pid > 0) {
    waitpid(client->daemon_pid, NULL, WNOHANG); /* Reap the dead one */
    client->daemon_pid = 0;
  }
  char *spawned = NULL;
  if (!spawn_daemon_unix(client, NULL, &spawned)) {
    return 0;
  }
  fd = lace_connect_unix(spawned, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
  if (fd < 0) {
    free(spawned);
    kill_spawned_daemon(client);
    return 0;
  }
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

/* "sqlite://relative.db" would be opened relative to the daemon's working
 * directory, which a shared or long-running daemon does not share with this
 * process: make the path absolute here. Returns NULL if nothing to change. */
static char *absolute_sqlite_connstr(const char *connstr) {
  if (strncmp(connstr, "sqlite://", 9) != 0) {
    return NULL;
  }
  const char *rest = connstr + 9;
  if (rest[0] == '/' || rest[0] == '\0' || rest[0] == ':' || rest[0] == '?') {
    return NULL; /* Absolute already, in-memory, or empty */
  }
  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd))) {
    return NULL;
  }
  return str_printf("sqlite://%s/%s", strcmp(cwd, "/") == 0 ? "" : cwd, rest);
}

int lace_connect(lace_client_t *client, const char *connstr,
                 const char *password, int *conn_id) {
  if (!client || !client->connected || !connstr || !conn_id) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  char *abs_connstr = absolute_sqlite_connstr(connstr);
  if (abs_connstr) {
    connstr = abs_connstr;
  }
  cJSON_AddStringToObject(params, "connstr", connstr);
  if (password) {
    cJSON_AddStringToObject(params, "password", password);
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "connect", params, &result);
  cJSON_Delete(params);

  if (err != LACE_OK) {
    str_secure_free(abs_connstr);
    return err;
  }

  cJSON *id_json = cJSON_GetObjectItem(result, "conn_id");
  if (!id_json || !cJSON_IsNumber(id_json)) {
    cJSON_Delete(result);
    str_secure_free(abs_connstr);
    return LACE_ERR_INTERNAL_ERROR;
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
  if (!client || !client->connected) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
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

int lace_reconnect(lace_client_t *client, int conn_id, const char *password) {
  /* Reconnect is currently implemented as disconnect + connect
   * This would need the original connection string stored */
  (void)client;
  (void)conn_id;
  (void)password;
  return LACE_ERR_INTERNAL_ERROR; /* TODO: Implement */
}

int lace_list_connections(lace_client_t *client, LaceConnInfo **info,
                          size_t *count) {
  if (!client || !client->connected || !info || !count) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "connections", NULL, &result);
  if (err != LACE_OK) {
    return err;
  }

  if (!cJSON_IsArray(result)) {
    cJSON_Delete(result);
    return LACE_ERR_INTERNAL_ERROR;
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
      /* Map driver string to enum */
      if (strcmp(driver->valuestring, "sqlite") == 0) {
        arr[i].driver = LACE_DRIVER_SQLITE;
      } else if (strcmp(driver->valuestring, "postgres") == 0) {
        arr[i].driver = LACE_DRIVER_POSTGRES;
      } else if (strcmp(driver->valuestring, "mysql") == 0) {
        arr[i].driver = LACE_DRIVER_MYSQL;
      } else if (strcmp(driver->valuestring, "mariadb") == 0) {
        arr[i].driver = LACE_DRIVER_MARIADB;
      }
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
  if (!client || !client->connected || !tables || !count) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver = get_connection_driver(client, conn_id);

  /* Build driver-specific SQL */
  const char *sql;
  switch (driver) {
  case LACE_DRIVER_SQLITE:
    sql = "SELECT name FROM sqlite_master WHERE type='table' "
          "AND name NOT LIKE 'sqlite_%' ORDER BY name";
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
    return LACE_ERR_INTERNAL_ERROR;
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
    return LACE_ERR_OUT_OF_MEMORY;
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
  if (!client || !client->connected || !table || !schema) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver = get_connection_driver(client, conn_id);

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
    return LACE_ERR_INTERNAL_ERROR;
  }
  if (!sql)
    return LACE_ERR_OUT_OF_MEMORY;

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
    return LACE_ERR_TABLE_NOT_FOUND;
  }

  /* Allocate schema */
  LaceSchema *sch = safe_calloc(1, sizeof(LaceSchema));
  if (!sch) {
    lace_result_free(result);
    return LACE_ERR_OUT_OF_MEMORY;
  }

  sch->name = str_dup(table);
  sch->num_columns = result->num_rows;
  sch->columns = safe_calloc(result->num_rows, sizeof(LaceColumn));
  if (!sch->columns) {
    free(sch->name);
    free(sch);
    lace_result_free(result);
    return LACE_ERR_OUT_OF_MEMORY;
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
               const LaceFilter *filters, size_t num_filters,
               const LaceSort *sorts, size_t num_sorts,
               size_t offset, size_t limit, LaceResult **result) {
  if (!client || !client->connected || !table || !result) {
    return LACE_ERR_INVALID_PARAMS;
  }

  /* Note: filters and sorts require schema to build SQL - not supported here.
   * Use lace_exec() with pre-built SQL for filtered/sorted queries. */
  (void)filters;
  (void)num_filters;
  (void)sorts;
  (void)num_sorts;

  LaceDriver driver = get_connection_driver(client, conn_id);
  char *escaped_table = lace_quote_table(driver, table);
  if (!escaped_table) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  char *sql = str_printf("SELECT * FROM %s LIMIT %zu OFFSET %zu",
                         escaped_table, limit > 0 ? limit : 500, offset);
  free(escaped_table);
  if (!sql) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  int err = lace_exec(client, conn_id, sql, result);
  free(sql);
  return err;
}

int lace_count(lace_client_t *client, int conn_id, const char *table,
               const LaceFilter *filters, size_t num_filters,
               size_t *count, bool *approximate) {
  if (!client || !client->connected || !table || !count) {
    return LACE_ERR_INVALID_PARAMS;
  }

  /* Note: filters require schema to build SQL - not supported here.
   * Use lace_exec() with COUNT(*) WHERE ... for filtered counts. */
  (void)filters;
  (void)num_filters;

  LaceDriver driver = get_connection_driver(client, conn_id);

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
    return LACE_ERR_OUT_OF_MEMORY;
  }

  char *sql = str_printf("SELECT COUNT(*) FROM %s", escaped_table);
  free(escaped_table);
  if (!sql) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);
  free(sql);
  if (err != LACE_OK) {
    return err;
  }

  if (!result || result->num_rows == 0) {
    lace_result_free(result);
    return LACE_ERR_INTERNAL_ERROR;
  }

  LaceRow *row = &result->rows[0];
  if (!row->cells || row->num_cells == 0) {
    lace_result_free(result);
    return LACE_ERR_INTERNAL_ERROR;
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

int lace_exec(lace_client_t *client, int conn_id, const char *sql,
              LaceResult **result) {
  if (!client || !client->connected || !sql) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
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
  if (!client || !client->connected) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);

  /* The query being cancelled is waiting on its own link; the cancel takes
   * another one from the pool, so it never waits behind the query. A stdio
   * daemon has a single link: if it is busy, try_call fails at once rather
   * than queueing the cancel after the query it should stop. */
  cJSON *resp = NULL;
  int err = lace_rpc_try_call(client, "cancel", params, &resp);
  cJSON_Delete(params);
  cJSON_Delete(resp);

  return err;
}

/* ==========================================================================
 * Data Mutations
 * ========================================================================== */

int lace_update(lace_client_t *client, int conn_id, const char *table,
                const LacePkValue *pk, size_t num_pk,
                const char *column, const LaceValue *value) {
  if (!client || !client->connected || !table || !pk || num_pk == 0 || !column || !value) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver = get_connection_driver(client, conn_id);
  char *esc_table = lace_quote_table(driver, table);
  char *esc_col = escape_identifier(column, driver);
  char *val_sql = value_to_sql(value, driver);
  StringBuilder *sb = sb_new(256);
  if (!esc_table || !esc_col || !val_sql || !sb) {
    free(esc_table);
    free(esc_col);
    str_secure_free(val_sql);
    sb_free(sb);
    return LACE_ERR_OUT_OF_MEMORY;
  }

  sb_printf(sb, "UPDATE %s SET %s = ", esc_table, esc_col);
  sb_append(sb, val_sql);
  sb_append(sb, " WHERE ");
  free(esc_table);
  free(esc_col);
  str_secure_free(val_sql);

  if (!append_pk_where(sb, pk, num_pk, driver)) {
    sb_free(sb);
    return LACE_ERR_OUT_OF_MEMORY;
  }
  return exec_mutation(client, conn_id, sb_finish(sb), EXPECT_ONE);
}

int lace_delete(lace_client_t *client, int conn_id, const char *table,
                const LacePkValue *pk, size_t num_pk) {
  if (!client || !client->connected || !table || !pk || num_pk == 0) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver = get_connection_driver(client, conn_id);
  char *esc_table = lace_quote_table(driver, table);
  StringBuilder *sb = sb_new(128);
  if (!esc_table || !sb) {
    free(esc_table);
    sb_free(sb);
    return LACE_ERR_OUT_OF_MEMORY;
  }

  sb_printf(sb, "DELETE FROM %s WHERE ", esc_table);
  free(esc_table);

  if (!append_pk_where(sb, pk, num_pk, driver)) {
    sb_free(sb);
    return LACE_ERR_OUT_OF_MEMORY;
  }
  return exec_mutation(client, conn_id, sb_finish(sb), EXPECT_ONE);
}

int lace_insert(lace_client_t *client, int conn_id, const char *table,
                const char **columns, const LaceValue *values, size_t num_columns,
                LacePkValue **out_pk, size_t *out_num_pk) {
  if (out_pk) *out_pk = NULL;
  if (out_num_pk) *out_num_pk = 0;
  if (!client || !client->connected || !table ||
      (num_columns > 0 && (!columns || !values))) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver = get_connection_driver(client, conn_id);
  if (num_columns == 0) {
    /* Every column takes its default */
    char *esc = lace_quote_table(driver, table);
    if (!esc) {
      return LACE_ERR_OUT_OF_MEMORY;
    }
    bool mysql = driver == LACE_DRIVER_MYSQL || driver == LACE_DRIVER_MARIADB;
    char *sql = str_printf(mysql ? "INSERT INTO %s () VALUES ()"
                                 : "INSERT INTO %s DEFAULT VALUES",
                           esc);
    free(esc);
    if (!sql) {
      return LACE_ERR_OUT_OF_MEMORY;
    }
    return exec_mutation(client, conn_id, sql, EXPECT_SOME);
  }

  char *esc_table = lace_quote_table(driver, table);
  StringBuilder *sb = sb_new(256);
  if (!esc_table || !sb) {
    free(esc_table);
    sb_free(sb);
    return LACE_ERR_OUT_OF_MEMORY;
  }

  sb_printf(sb, "INSERT INTO %s (", esc_table);
  free(esc_table);

  for (size_t i = 0; i < num_columns; i++) {
    char *esc_col = escape_identifier(columns[i], driver);
    if (!esc_col) {
      sb_free(sb);
      return LACE_ERR_OUT_OF_MEMORY;
    }
    sb_printf(sb, "%s%s", i > 0 ? ", " : "", esc_col);
    free(esc_col);
  }

  sb_append(sb, ") VALUES (");
  for (size_t i = 0; i < num_columns; i++) {
    char *val_sql = value_to_sql(&values[i], driver);
    if (!val_sql) {
      sb_free(sb);
      return LACE_ERR_OUT_OF_MEMORY;
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
  if (!client || !client->connected || !table || !pk || num_pk == 0 ||
      !columns || num_columns == 0 || !result) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver = get_connection_driver(client, conn_id);
  StringBuilder *sb = sb_new(256);
  if (!sb) {
    return LACE_ERR_OUT_OF_MEMORY;
  }
  sb_append(sb, "SELECT ");
  for (size_t i = 0; i < num_columns; i++) {
    char *col = escape_identifier(columns[i], driver);
    if (!col) {
      sb_free(sb);
      return LACE_ERR_OUT_OF_MEMORY;
    }
    sb_printf(sb, "%s%s", i > 0 ? ", " : "", col);
    free(col);
  }
  char *esc_table = lace_quote_table(driver, table);
  if (!esc_table) {
    sb_free(sb);
    return LACE_ERR_OUT_OF_MEMORY;
  }
  sb_printf(sb, " FROM %s WHERE ", esc_table);
  free(esc_table);
  /* Same WHERE builder as lace_update/lace_delete, so this sees exactly the
   * rows a following mutation would touch. LIMIT 2 detects "more than one". */
  if (!append_pk_where(sb, pk, num_pk, driver)) {
    sb_free(sb);
    return LACE_ERR_OUT_OF_MEMORY;
  }
  sb_append(sb, " LIMIT 2");
  char *sql = sb_finish(sb);
  if (!sql) {
    return LACE_ERR_OUT_OF_MEMORY;
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
  if (!client || !client->connected) {
    return LACE_ERR_CONNECTION_CLOSED;
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "ping", NULL, &result);
  cJSON_Delete(result);
  return err;
}

int lace_version(lace_client_t *client, char **version) {
  if (!client || !client->connected || !version) {
    return LACE_ERR_INVALID_PARAMS;
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
  if (!client || !client->connected) {
    return LACE_ERR_CONNECTION_CLOSED;
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "shutdown", NULL, &result);
  cJSON_Delete(result);
  return err;
}
