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
  if (str_eq(cs->driver, "postgres") || str_eq(cs->driver, "postgresql")) {
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
static void track_connection(lace_client_t *client, int conn_id, LaceDriver driver) {
  if (client->num_connections >= LACE_CLIENT_MAX_CONNECTIONS) {
    return; /* Silently ignore if full */
  }
  for (size_t i = 0; i < client->num_connections; i++) {
    if (client->connections[i].conn_id == conn_id) {
      client->connections[i].driver = driver; /* Daemon reused the id */
      return;
    }
  }
  client->connections[client->num_connections].conn_id = conn_id;
  client->connections[client->num_connections].driver = driver;
  client->num_connections++;
}

/* Remove a tracked connection */
static void untrack_connection(lace_client_t *client, int conn_id) {
  for (size_t i = 0; i < client->num_connections; i++) {
    if (client->connections[i].conn_id == conn_id) {
      /* Shift remaining entries */
      for (size_t j = i; j < client->num_connections - 1; j++) {
        client->connections[j] = client->connections[j + 1];
      }
      client->num_connections--;
      return;
    }
  }
}

/* Get driver type for a connection */
static LaceDriver get_connection_driver(lace_client_t *client, int conn_id) {
  for (size_t i = 0; i < client->num_connections; i++) {
    if (client->connections[i].conn_id == conn_id) {
      return client->connections[i].driver;
    }
  }
  return LACE_DRIVER_SQLITE; /* Default */
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
  snprintf(buf, sizeof(buf), "%.15g", v);
  /* Normalize the locale's decimal separator to '.' before parsing back */
  const char *dp = localeconv()->decimal_point;
  char sep = (dp && dp[0]) ? dp[0] : '.';
  if (strtod(buf, NULL) != v) {
    snprintf(buf, sizeof(buf), "%.17g", v);
  }
  if (sep != '.') {
    for (char *p = buf; *p; p++) {
      if (*p == sep) {
        *p = '.';
      }
    }
  }
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
static int exec_mutation(lace_client_t *client, int conn_id, char *sql) {
  if (!sql) {
    return LACE_ERR_OUT_OF_MEMORY;
  }
  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);
  str_secure_free(sql); /* May contain sensitive cell data */
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

/* Spawn daemon process in Unix socket mode */
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

    /* Execute daemon in default Unix socket mode */
    /* A daemon started on demand goes away once its clients are gone
     * (including clients that crashed without shutting it down). */
    execl(daemon_exe, daemon_exe, "--unix", socket_path, "--exit-when-idle",
          "600", (char *)NULL);

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

/* ==========================================================================
 * Client Lifecycle
 * ========================================================================== */

void lace_client_init_common(lace_client_t *client) {
  client->timeout_ms = DEFAULT_TIMEOUT_MS;
  client->next_id = 1;
  client->socket_fd = -1;
  client->daemon_pid = 0;
  pthread_mutex_init(&client->lock, NULL);
}

static lace_client_t *client_alloc(LaceConnMode mode) {
  lace_client_t *client = safe_calloc(1, sizeof(lace_client_t));
  lace_client_init_common(client);
  client->conn_mode = mode;
  return client;
}

void lace_client_close_streams(lace_client_t *client) {
  /* to_daemon owns socket_fd, from_daemon owns a dup() of it */
  if (client->to_daemon) {
    fclose(client->to_daemon);
  } else if (client->socket_fd >= 0) {
    close(client->socket_fd);
  }
  if (client->from_daemon) {
    fclose(client->from_daemon);
  }
  client->to_daemon = NULL;
  client->from_daemon = NULL;
  client->socket_fd = -1;
}

bool lace_client_attach_socket(lace_client_t *client, int fd) {
  if (fd < 0) {
    return false;
  }
  int fd_read = dup(fd);
  if (fd_read < 0) {
    close(fd);
    return false;
  }
  FILE *w = fdopen(fd, "w");
  FILE *r = fdopen(fd_read, "r");
  if (!w || !r) {
    if (w) fclose(w); else close(fd);
    if (r) fclose(r); else close(fd_read);
    return false;
  }
  setvbuf(w, NULL, _IONBF, 0);
  setvbuf(r, NULL, _IONBF, 0);
  client->socket_fd = fd;
  client->to_daemon = w;
  client->from_daemon = r;
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
  }
  free(spawned_path);
}

lace_client_t *lace_client_create(const char *daemon_path) {
  lace_client_t *client = client_alloc(LACE_CONN_SPAWN);

  /* Prefer an already-running daemon */
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

  spawn_and_attach(client, daemon_path, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
  return client;
}

static lace_client_t *create_from_socket(int fd, LaceConnMode mode) {
  if (fd < 0) {
    return NULL;
  }
  lace_client_t *client = client_alloc(mode);
  if (!lace_client_attach_socket(client, fd)) {
    pthread_mutex_destroy(&client->lock);
    free(client);
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
    return lace_client_create(config->daemon_path);

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
    free(default_path);
    if (config->spawn_if_missing) {
      client->conn_mode = LACE_CONN_SPAWN;
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
  return create_from_socket(fd, LACE_CONN_TCP);
}

void lace_client_destroy(lace_client_t *client) {
  if (!client) {
    return;
  }

  /* Never shut the daemon down here, even one this client spawned: other
   * frontends may have attached to it through the shared default socket.
   * Daemons started by liblace run with --exit-when-idle and go away by
   * themselves once their last client disconnects. */
  lace_client_close_streams(client);
  if (client->daemon_pid > 0) {
    waitpid(client->daemon_pid, NULL, WNOHANG); /* Reap if it already exited */
  }

  pthread_mutex_destroy(&client->lock);
  free(client->socket_path);
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
  free(client->socket_path);
  client->socket_path = copy;
}

int lace_client_reattach(lace_client_t *client) {
  if (client->conn_mode == LACE_CONN_TCP) {
    return 0;
  }
  lace_client_close_streams(client);

  char *default_path = lace_get_default_socket_path();
  const char *path = client->socket_path ? client->socket_path : default_path;
  int fd = path ? lace_connect_unix(path, 1000) : -1;
  if (fd >= 0) {
    bool ok = lace_client_attach_socket(client, fd);
    free(default_path);
    return ok ? 1 : 0;
  }

  /* Nothing listening. Only respawn on the default path: that is where
   * spawn_daemon_unix puts a daemon, and a custom path is someone else's. */
  bool is_default = !client->socket_path ||
                    (default_path && strcmp(client->socket_path, default_path) == 0);
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
  if (fd < 0 || !lace_client_attach_socket(client, fd)) {
    free(spawned);
    kill_spawned_daemon(client);
    return 0;
  }
  lace_client_set_socket_path(client, spawned);
  free(spawned);
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

int lace_get_timeout(const lace_client_t *client) {
  return client ? client->timeout_ms : 0;
}

/* ==========================================================================
 * Database Connection
 * ========================================================================== */

int lace_connect(lace_client_t *client, const char *connstr,
                 const char *password, int *conn_id) {
  if (!client || !client->connected || !connstr || !conn_id) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddStringToObject(params, "connstr", connstr);
  if (password) {
    cJSON_AddStringToObject(params, "password", password);
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "connect", params, &result);
  cJSON_Delete(params);

  if (err != LACE_OK) {
    return err;
  }

  cJSON *id_json = cJSON_GetObjectItem(result, "conn_id");
  if (!id_json || !cJSON_IsNumber(id_json)) {
    cJSON_Delete(result);
    return LACE_ERR_INTERNAL_ERROR;
  }

  *conn_id = id_json->valueint;
  cJSON_Delete(result);

  /* Track driver type for this connection */
  LaceDriver driver = parse_driver(connstr);
  track_connection(client, *conn_id, driver);

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
      sql = str_printf("PRAGMA table_info(%s)", lit);
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
          "d.adbin, d.adrelid) LIKE 'nextval(%%', false))::int "
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
      /* PRAGMA table_info: cid, name, type, notnull, dflt_value, pk */
      if (result->num_columns >= 6 && row->num_cells >= 6) {
        col->name = row->cells[1].type == LACE_TYPE_TEXT ? str_dup(row->cells[1].text.data) : NULL;
        col->type_name = row->cells[2].type == LACE_TYPE_TEXT ? str_dup(row->cells[2].text.data) : NULL;
        col->nullable = row->cells[3].type == LACE_TYPE_INT ? !row->cells[3].int_val : true;
        col->primary_key = row->cells[5].type == LACE_TYPE_INT ? row->cells[5].int_val > 0 : false;
        col->default_val = cell_text_dup(&row->cells[4]);
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
        }
      }
      break;

    default:
      break;
    }
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

  cJSON *resp = NULL;
  int err = lace_rpc_call(client, "query", params, &resp);
  cJSON_Delete(params);

  if (err != LACE_OK) {
    return err;
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

  /* The query being cancelled usually holds client->lock while it waits for
   * its reply, so send the cancel over a separate short-lived connection.
   * The daemon's connection pool is shared across sockets. */
  cJSON *resp = NULL;
  int err = lace_rpc_try_call(client, "cancel", params, &resp);
  if (err == LACE_ERR_TIMEOUT && client->conn_mode != LACE_CONN_TCP) {
    lace_client_t *side = lace_client_connect(client->socket_path);
    if (side) {
      err = lace_rpc_call(side, "cancel", params, &resp);
      lace_client_destroy(side);
    }
  }
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
  return exec_mutation(client, conn_id, sb_finish(sb));
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
  return exec_mutation(client, conn_id, sb_finish(sb));
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
    return exec_mutation(client, conn_id, sql);
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
  return exec_mutation(client, conn_id, sb_finish(sb));
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
