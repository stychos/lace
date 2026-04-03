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
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Default timeout in milliseconds */
#define DEFAULT_TIMEOUT_MS 30000

/* Maximum tracked connections */
#define MAX_CONNECTIONS 64

/* Connection tracking entry */
typedef struct {
  int conn_id;
  LaceDriver driver;
} ConnEntry;

/* Client structure */
struct lace_client {
  pid_t daemon_pid;       /* Daemon process ID (0 if connected to existing) */
  FILE *to_daemon;        /* Write to daemon stdin */
  FILE *from_daemon;      /* Read from daemon stdout */
  int socket_fd;          /* Socket fd (-1 if using pipes) */
  LaceConnMode conn_mode; /* Connection mode */
  int timeout_ms;         /* Request timeout */
  char *last_error;       /* Last error message */
  int64_t next_id;        /* Next request ID */
  bool connected;         /* Whether daemon is running */
  bool owns_daemon;       /* True if we spawned this daemon */
  char *socket_path;      /* Socket path for cleanup (if we spawned) */
  int idle_timeout;       /* Daemon idle timeout to pass on spawn */

  /* Connection tracking */
  ConnEntry connections[MAX_CONNECTIONS];
  size_t num_connections;
};

/* Socket utilities (implemented in socket.c) */
extern int lace_connect_unix(const char *path, int timeout_ms);
extern int lace_connect_tcp(const char *host, int port, int timeout_ms);
extern int lace_find_daemon_socket(void);
extern char *lace_get_default_socket_path(void);
extern bool lace_daemon_is_running(const char *socket_path);

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/* Set error message */
static void set_error(lace_client_t *client, const char *msg) {
  free(client->last_error);
  client->last_error = msg ? str_dup(msg) : NULL;
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
  if (client->num_connections >= MAX_CONNECTIONS) {
    return; /* Silently ignore if full */
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

/* Escape string value for SQL (caller must free) */
static char *escape_string_value(const char *value) {
  if (!value) return str_dup("NULL");

  size_t len = strlen(value);
  size_t extra = 0;
  for (size_t i = 0; i < len; i++) {
    if (value[i] == '\'') extra++;
  }

  char *escaped = malloc(len + extra + 3);
  if (!escaped) return NULL;

  char *p = escaped;
  *p++ = '\'';
  for (size_t i = 0; i < len; i++) {
    if (value[i] == '\'') *p++ = '\'';
    *p++ = value[i];
  }
  *p++ = '\'';
  *p = '\0';

  return escaped;
}

/* Convert LaceValue to SQL literal (caller must free) */
static char *value_to_sql(const LaceValue *val) {
  if (!val || val->is_null) return str_dup("NULL");

  char buf[64];
  switch (val->type) {
  case LACE_TYPE_NULL:
    return str_dup("NULL");
  case LACE_TYPE_INT:
    snprintf(buf, sizeof(buf), "%lld", (long long)val->int_val);
    return str_dup(buf);
  case LACE_TYPE_FLOAT:
    snprintf(buf, sizeof(buf), "%g", val->float_val);
    return str_dup(buf);
  case LACE_TYPE_TEXT:
    return escape_string_value(val->text.data);
  case LACE_TYPE_BOOL:
    return str_dup(val->bool_val ? "TRUE" : "FALSE");
  case LACE_TYPE_BLOB:
    /* For now, return NULL for blobs - proper handling would need hex encoding */
    return str_dup("NULL");
  default:
    return str_dup("NULL");
  }
}

/* Get directory of the current executable */
static char *get_exe_dir(void) {
  char exe_path[4096];
  ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (len <= 0) {
    return NULL;
  }
  exe_path[len] = '\0';

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

  /* Set up FILE streams */
  client->to_daemon = fdopen(to_daemon[1], "w");
  client->from_daemon = fdopen(from_daemon[0], "r");

  if (!client->to_daemon || !client->from_daemon) {
    if (client->to_daemon) {
      fclose(client->to_daemon);
    } else {
      close(to_daemon[1]);
    }
    if (client->from_daemon) {
      fclose(client->from_daemon);
    } else {
      close(from_daemon[0]);
    }
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    set_error(client, "Failed to create FILE streams");
    return false;
  }

  /* Disable buffering for immediate writes */
  setvbuf(client->to_daemon, NULL, _IONBF, 0);
  setvbuf(client->from_daemon, NULL, _IONBF, 0);

  client->daemon_pid = pid;
  client->socket_fd = -1; /* Not using socket */
  client->connected = true;
  client->owns_daemon = true;
  client->conn_mode = LACE_CONN_SPAWN;

  /* Verify daemon is responsive */
  cJSON *result = NULL;
  int err = lace_rpc_call(client, "ping", NULL, &result);
  cJSON_Delete(result);

  if (err != LACE_OK) {
    fclose(client->to_daemon);
    fclose(client->from_daemon);
    client->to_daemon = NULL;
    client->from_daemon = NULL;
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
static bool spawn_daemon_unix(lace_client_t *client, const char *daemon_path) {
  char *daemon_exe = find_daemon(daemon_path);
  if (!daemon_exe) {
    set_error(client, "Daemon executable not found");
    return false;
  }

  /* Get socket path for daemon */
  char *socket_path = lace_get_default_socket_path();
  if (!socket_path) {
    free(daemon_exe);
    set_error(client, "Failed to determine socket path");
    return false;
  }

  /* Remove stale socket file if it exists */
  unlink(socket_path);

  pid_t pid = fork();
  if (pid < 0) {
    free(daemon_exe);
    free(socket_path);
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

    /* Execute daemon in Unix socket mode */
    if (client->idle_timeout > 0) {
      char timeout_str[16];
      snprintf(timeout_str, sizeof(timeout_str), "%d", client->idle_timeout);
      execl(daemon_exe, daemon_exe, "--unix", socket_path,
            "--idle-timeout", timeout_str, (char *)NULL);
    } else {
      execl(daemon_exe, daemon_exe, "--unix", socket_path, (char *)NULL);
    }

    /* If exec fails, exit */
    _exit(127);
  }

  /* Parent process */
  free(daemon_exe);

  /* Wait for daemon to create socket (with timeout) */
  int wait_attempts = 50; /* 50 * 20ms = 1 second max */
  while (wait_attempts > 0) {
    if (lace_daemon_is_running(socket_path)) {
      break;
    }
    usleep(20000); /* 20ms */
    wait_attempts--;

    /* Check if daemon died */
    int status;
    if (waitpid(pid, &status, WNOHANG) != 0) {
      free(socket_path);
      set_error(client, "Daemon failed to start");
      return false;
    }
  }

  if (wait_attempts == 0) {
    /* Timeout - daemon didn't start */
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    free(socket_path);
    set_error(client, "Daemon startup timeout");
    return false;
  }

  /* Connect to daemon via Unix socket */
  int fd = lace_connect_unix(socket_path, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
  if (fd < 0) {
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    free(socket_path);
    set_error(client, "Failed to connect to spawned daemon");
    return false;
  }

  /* Set up FILE streams */
  int fd_read = dup(fd);
  if (fd_read < 0) {
    close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    free(socket_path);
    set_error(client, "Failed to duplicate socket fd");
    return false;
  }

  client->to_daemon = fdopen(fd, "w");
  client->from_daemon = fdopen(fd_read, "r");

  if (!client->to_daemon || !client->from_daemon) {
    if (client->to_daemon) {
      fclose(client->to_daemon);
    } else {
      close(fd);
    }
    if (client->from_daemon) {
      fclose(client->from_daemon);
    } else {
      close(fd_read);
    }
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    free(socket_path);
    set_error(client, "Failed to create FILE streams");
    return false;
  }

  /* Disable buffering for immediate writes */
  setvbuf(client->to_daemon, NULL, _IONBF, 0);
  setvbuf(client->from_daemon, NULL, _IONBF, 0);

  client->daemon_pid = pid;
  client->socket_fd = fd;
  client->socket_path = socket_path;
  client->connected = true;
  client->owns_daemon = false; /* Daemon manages its own lifecycle (idle timeout / signal) */
  client->conn_mode = LACE_CONN_UNIX;

  /* Verify daemon is responsive */
  cJSON *result = NULL;
  int err = lace_rpc_call(client, "ping", NULL, &result);
  cJSON_Delete(result);

  if (err != LACE_OK) {
    fclose(client->to_daemon);
    fclose(client->from_daemon);
    client->to_daemon = NULL;
    client->from_daemon = NULL;
    client->connected = false;
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    client->daemon_pid = 0;
    free(socket_path);
    client->socket_path = NULL;
    client->owns_daemon = false;
    set_error(client, "Daemon not responding");
    return false;
  }

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
    if (waitpid(pid, &status, WNOHANG) != 0) {
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

  /* Set up FILE streams */
  int fd_read = dup(fd);
  if (fd_read < 0) {
    close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    set_error(client, "Failed to duplicate socket fd");
    return false;
  }

  client->to_daemon = fdopen(fd, "w");
  client->from_daemon = fdopen(fd_read, "r");

  if (!client->to_daemon || !client->from_daemon) {
    if (client->to_daemon) {
      fclose(client->to_daemon);
    } else {
      close(fd);
    }
    if (client->from_daemon) {
      fclose(client->from_daemon);
    } else {
      close(fd_read);
    }
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    set_error(client, "Failed to create FILE streams");
    return false;
  }

  /* Disable buffering for immediate writes */
  setvbuf(client->to_daemon, NULL, _IONBF, 0);
  setvbuf(client->from_daemon, NULL, _IONBF, 0);

  client->daemon_pid = pid;
  client->socket_fd = fd;
  client->connected = true;
  client->owns_daemon = false; /* Daemon manages its own lifecycle (idle timeout / signal) */
  client->conn_mode = LACE_CONN_TCP;

  /* Verify daemon is responsive */
  cJSON *result = NULL;
  int err = lace_rpc_call(client, "ping", NULL, &result);
  cJSON_Delete(result);

  if (err != LACE_OK) {
    fclose(client->to_daemon);
    fclose(client->from_daemon);
    client->to_daemon = NULL;
    client->from_daemon = NULL;
    client->connected = false;
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    client->daemon_pid = 0;
    client->owns_daemon = false;
    set_error(client, "Daemon not responding");
    return false;
  }

  return true;
}

/* ==========================================================================
 * Client Lifecycle
 * ========================================================================== */

lace_client_t *lace_client_create_ex(const char *daemon_path, LaceSpawnMode spawn_mode) {
  return lace_client_create_ex2(daemon_path, spawn_mode, 0);
}

lace_client_t *lace_client_create_ex2(const char *daemon_path,
                                      LaceSpawnMode spawn_mode,
                                      int idle_timeout) {
  lace_client_t *client = safe_calloc(1, sizeof(lace_client_t));

  client->timeout_ms = DEFAULT_TIMEOUT_MS;
  client->next_id = 1;
  client->socket_fd = -1;
  client->conn_mode = LACE_CONN_SPAWN;
  client->idle_timeout = idle_timeout;

  /* Try to connect to existing daemon first */
  if (spawn_mode == LACE_SPAWN_UNIX || spawn_mode == LACE_SPAWN_NONE) {
    /* Try Unix socket */
    char *socket_path = lace_get_default_socket_path();
    if (socket_path && lace_daemon_is_running(socket_path)) {
      int fd = lace_connect_unix(socket_path, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
      free(socket_path);
      if (fd >= 0) {
        /* Connected to existing daemon */
        client->socket_fd = fd;
        int fd_read = dup(fd);
        if (fd_read >= 0) {
          client->to_daemon = fdopen(fd, "w");
          client->from_daemon = fdopen(fd_read, "r");
          if (client->to_daemon && client->from_daemon) {
            setvbuf(client->to_daemon, NULL, _IONBF, 0);
            setvbuf(client->from_daemon, NULL, _IONBF, 0);
            client->connected = true;
            client->conn_mode = LACE_CONN_UNIX;
            return client;
          }
          if (client->to_daemon) fclose(client->to_daemon);
          if (client->from_daemon) fclose(client->from_daemon);
          else close(fd_read);
          if (!client->to_daemon) close(fd);
        } else {
          close(fd);
        }
      }
    } else {
      free(socket_path);
    }
  }

  if (spawn_mode == LACE_SPAWN_TCP || spawn_mode == LACE_SPAWN_NONE) {
    /* Try TCP socket on default port */
    int fd = lace_connect_tcp(NULL, LACE_DEFAULT_PORT, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
    if (fd >= 0) {
      client->socket_fd = fd;
      int fd_read = dup(fd);
      if (fd_read >= 0) {
        client->to_daemon = fdopen(fd, "w");
        client->from_daemon = fdopen(fd_read, "r");
        if (client->to_daemon && client->from_daemon) {
          setvbuf(client->to_daemon, NULL, _IONBF, 0);
          setvbuf(client->from_daemon, NULL, _IONBF, 0);
          client->connected = true;
          client->conn_mode = LACE_CONN_TCP;
          return client;
        }
        if (client->to_daemon) fclose(client->to_daemon);
        if (client->from_daemon) fclose(client->from_daemon);
        else close(fd_read);
        if (!client->to_daemon) close(fd);
      } else {
        close(fd);
      }
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
    spawn_daemon_unix(client, daemon_path);
    break;
  }
  /* Error or success already set in spawn function */
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

  lace_client_t *client = safe_calloc(1, sizeof(lace_client_t));

  client->timeout_ms = DEFAULT_TIMEOUT_MS;
  client->next_id = 1;
  client->socket_fd = fd;
  client->conn_mode = mode;
  client->daemon_pid = 0;

  /* Create FILE streams for the socket */
  int fd_read = dup(fd);
  if (fd_read < 0) {
    close(fd);
    free(client);
    return NULL;
  }

  client->to_daemon = fdopen(fd, "w");
  client->from_daemon = fdopen(fd_read, "r");

  if (!client->to_daemon || !client->from_daemon) {
    if (client->to_daemon) fclose(client->to_daemon);
    if (client->from_daemon) fclose(client->from_daemon);
    else close(fd_read);
    if (!client->to_daemon) close(fd);
    free(client);
    return NULL;
  }

  /* Disable buffering for immediate communication */
  setvbuf(client->to_daemon, NULL, _IONBF, 0);
  setvbuf(client->from_daemon, NULL, _IONBF, 0);

  client->connected = true;
  return client;
}

lace_client_t *lace_client_create_with_config(const LaceClientConfig *config) {
  if (!config) {
    return lace_client_create(NULL);
  }

  switch (config->mode) {
  case LACE_CONN_SPAWN:
    return lace_client_create(config->daemon_path);

  case LACE_CONN_UNIX: {
    const char *path = config->socket_path;
    char *default_path = NULL;
    if (!path) {
      default_path = lace_get_default_socket_path();
      path = default_path;
    }
    int fd = lace_connect_unix(path, config->connect_timeout_ms);
    free(default_path);
    if (fd < 0 && config->spawn_if_missing) {
      /* No daemon running, spawn one in Unix socket mode.
       * We'll send shutdown RPC when this client is destroyed. */
      lace_client_t *client = safe_calloc(1, sizeof(lace_client_t));
      client->timeout_ms = DEFAULT_TIMEOUT_MS;
      client->next_id = 1;
      client->socket_fd = -1;
      spawn_daemon_unix(client, config->daemon_path);
      return client;
    }
    if (fd < 0) {
      lace_client_t *client = safe_calloc(1, sizeof(lace_client_t));
      set_error(client, "Failed to connect to Unix socket");
      return client;
    }
    return create_from_socket(fd, LACE_CONN_UNIX);
  }

  case LACE_CONN_TCP: {
    int fd = lace_connect_tcp(config->host, config->port, config->connect_timeout_ms);
    if (fd < 0) {
      lace_client_t *client = safe_calloc(1, sizeof(lace_client_t));
      set_error(client, "Failed to connect to TCP socket");
      return client;
    }
    return create_from_socket(fd, LACE_CONN_TCP);
  }
  }

  return NULL;
}

lace_client_t *lace_client_connect(const char *socket_path) {
  char *path = NULL;
  bool allocated = false;

  if (socket_path) {
    path = (char *)socket_path;
  } else {
    path = lace_get_default_socket_path();
    if (!path) {
      return NULL;
    }
    allocated = true;
  }

  int fd = lace_connect_unix(path, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
  if (allocated) {
    free(path);
  }

  if (fd < 0) {
    return NULL;
  }

  return create_from_socket(fd, LACE_CONN_UNIX);
}

lace_client_t *lace_client_connect_tcp(const char *host, int port) {
  int fd = lace_connect_tcp(host, port, LACE_DEFAULT_CONNECT_TIMEOUT_MS);
  if (fd < 0) {
    return NULL;
  }

  return create_from_socket(fd, LACE_CONN_TCP);
}

void lace_client_destroy(lace_client_t *client) {
  if (!client) {
    return;
  }

  if (client->connected) {
    if (client->owns_daemon && client->daemon_pid > 0) {
      /* We spawned this daemon: send shutdown RPC first */
      if (client->to_daemon) {
        lace_shutdown(client);
      }

      /* Close streams */
      if (client->to_daemon) {
        fclose(client->to_daemon);
      }
      if (client->from_daemon) {
        fclose(client->from_daemon);
      }

      /* Wait for daemon to exit, with timeout */
      int status;
      int wait_result = waitpid(client->daemon_pid, &status, WNOHANG);
      if (wait_result == 0) {
        /* Daemon still running, send SIGTERM */
        kill(client->daemon_pid, SIGTERM);
        usleep(100000); /* 100ms */
        wait_result = waitpid(client->daemon_pid, &status, WNOHANG);
        if (wait_result == 0) {
          /* Still running, force kill */
          kill(client->daemon_pid, SIGKILL);
          waitpid(client->daemon_pid, &status, 0);
        }
      }

      /* Clean up socket file if we spawned with Unix socket */
      if (client->socket_path) {
        unlink(client->socket_path);
      }
    } else {
      /* Connected to existing daemon: just close streams (don't shutdown) */
      if (client->to_daemon) {
        fclose(client->to_daemon);
      }
      if (client->from_daemon) {
        fclose(client->from_daemon);
      }
    }
  }

  free(client->socket_path);
  free(client->last_error);
  free(client);
}

bool lace_client_connected(const lace_client_t *client) {
  return client && client->connected;
}

const char *lace_client_error(const lace_client_t *client) {
  return client ? client->last_error : "Invalid client";
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
    sql = "SELECT tablename FROM pg_tables WHERE schemaname = 'public' "
          "ORDER BY tablename";
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
    if (row->cells && row->num_cells > 0 && row->cells[0].type == LACE_TYPE_TEXT) {
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

int lace_get_schema(lace_client_t *client, int conn_id, const char *table,
                    LaceSchema **schema) {
  if (!client || !client->connected || !table || !schema) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver = get_connection_driver(client, conn_id);

  /* Build driver-specific schema query */
  char sql[1024];
  switch (driver) {
  case LACE_DRIVER_SQLITE: {
    char *esc_table = escape_string_value(table);
    if (!esc_table) return LACE_ERR_OUT_OF_MEMORY;
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", esc_table);
    free(esc_table);
    break;
  }
  case LACE_DRIVER_POSTGRES: {
    char *esc_table = escape_string_value(table);
    if (!esc_table) return LACE_ERR_OUT_OF_MEMORY;
    snprintf(sql, sizeof(sql),
             "SELECT column_name, data_type, is_nullable, column_default "
             "FROM information_schema.columns "
             "WHERE table_name = %s AND table_schema = 'public' "
             "ORDER BY ordinal_position", esc_table);
    free(esc_table);
    break;
  }
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB: {
    char *esc_table = escape_identifier(table, driver);
    if (!esc_table) return LACE_ERR_OUT_OF_MEMORY;
    snprintf(sql, sizeof(sql), "DESCRIBE %s", esc_table);
    free(esc_table);
    break;
  }
  default:
    return LACE_ERR_INTERNAL_ERROR;
  }

  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);
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
      }
      break;

    case LACE_DRIVER_POSTGRES:
      /* column_name, data_type, is_nullable, column_default */
      if (result->num_columns >= 3 && row->num_cells >= 3) {
        col->name = row->cells[0].type == LACE_TYPE_TEXT ? str_dup(row->cells[0].text.data) : NULL;
        col->type_name = row->cells[1].type == LACE_TYPE_TEXT ? str_dup(row->cells[1].text.data) : NULL;
        col->nullable = row->cells[2].type == LACE_TYPE_TEXT && row->cells[2].text.data &&
                        strcmp(row->cells[2].text.data, "YES") == 0;
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
      }
      break;

    default:
      break;
    }
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
  char *escaped_table = escape_identifier(table, driver);
  if (!escaped_table) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  char sql[1024];
  snprintf(sql, sizeof(sql), "SELECT * FROM %s LIMIT %zu OFFSET %zu",
           escaped_table, limit > 0 ? limit : 500, offset);
  free(escaped_table);

  return lace_exec(client, conn_id, sql, result);
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
  char *escaped_table = escape_identifier(table, driver);
  if (!escaped_table) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  char sql[512];
  snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", escaped_table);
  free(escaped_table);

  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);
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

  if (approximate) {
    *approximate = false; /* Raw COUNT(*) is exact */
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

  cJSON *resp = NULL;
  int err = lace_rpc_call(client, "cancel", params, &resp);
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

  /* Build UPDATE table SET column = value WHERE pk1 = v1 AND pk2 = v2 ... */
  char sql[4096];
  char *p = sql;
  size_t remaining = sizeof(sql);

  /* UPDATE table SET column = value */
  char *esc_table = escape_identifier(table, driver);
  char *esc_col = escape_identifier(column, driver);
  char *val_sql = value_to_sql(value);
  if (!esc_table || !esc_col || !val_sql) {
    free(esc_table);
    free(esc_col);
    free(val_sql);
    return LACE_ERR_OUT_OF_MEMORY;
  }

  int n = snprintf(p, remaining, "UPDATE %s SET %s = %s WHERE ", esc_table, esc_col, val_sql);
  free(esc_table);
  free(esc_col);
  free(val_sql);

  if (n < 0 || (size_t)n >= remaining) {
    return LACE_ERR_INTERNAL_ERROR;
  }
  p += n;
  remaining -= (size_t)n;

  /* WHERE clause from primary keys */
  for (size_t i = 0; i < num_pk; i++) {
    char *esc_pk_col = escape_identifier(pk[i].column, driver);
    char *pk_val_sql = value_to_sql(&pk[i].value);
    if (!esc_pk_col || !pk_val_sql) {
      free(esc_pk_col);
      free(pk_val_sql);
      return LACE_ERR_OUT_OF_MEMORY;
    }

    n = snprintf(p, remaining, "%s%s = %s",
                 i > 0 ? " AND " : "", esc_pk_col, pk_val_sql);
    free(esc_pk_col);
    free(pk_val_sql);

    if (n < 0 || (size_t)n >= remaining) {
      return LACE_ERR_INTERNAL_ERROR;
    }
    p += n;
    remaining -= (size_t)n;
  }

  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);
  lace_result_free(result);
  return err;
}

int lace_delete(lace_client_t *client, int conn_id, const char *table,
                const LacePkValue *pk, size_t num_pk) {
  if (!client || !client->connected || !table || !pk || num_pk == 0) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver = get_connection_driver(client, conn_id);

  /* Build DELETE FROM table WHERE pk1 = v1 AND pk2 = v2 ... */
  char sql[4096];
  char *p = sql;
  size_t remaining = sizeof(sql);

  char *esc_table = escape_identifier(table, driver);
  if (!esc_table) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  int n = snprintf(p, remaining, "DELETE FROM %s WHERE ", esc_table);
  free(esc_table);

  if (n < 0 || (size_t)n >= remaining) {
    return LACE_ERR_INTERNAL_ERROR;
  }
  p += n;
  remaining -= (size_t)n;

  /* WHERE clause from primary keys */
  for (size_t i = 0; i < num_pk; i++) {
    char *esc_pk_col = escape_identifier(pk[i].column, driver);
    char *pk_val_sql = value_to_sql(&pk[i].value);
    if (!esc_pk_col || !pk_val_sql) {
      free(esc_pk_col);
      free(pk_val_sql);
      return LACE_ERR_OUT_OF_MEMORY;
    }

    n = snprintf(p, remaining, "%s%s = %s",
                 i > 0 ? " AND " : "", esc_pk_col, pk_val_sql);
    free(esc_pk_col);
    free(pk_val_sql);

    if (n < 0 || (size_t)n >= remaining) {
      return LACE_ERR_INTERNAL_ERROR;
    }
    p += n;
    remaining -= (size_t)n;
  }

  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);
  lace_result_free(result);
  return err;
}

int lace_insert(lace_client_t *client, int conn_id, const char *table,
                const char **columns, const LaceValue *values, size_t num_columns,
                LacePkValue **out_pk, size_t *out_num_pk) {
  if (!client || !client->connected || !table || !columns || !values || num_columns == 0) {
    return LACE_ERR_INVALID_PARAMS;
  }

  LaceDriver driver = get_connection_driver(client, conn_id);

  /* Build INSERT INTO table (col1, col2, ...) VALUES (v1, v2, ...) */
  char sql[8192];
  char *p = sql;
  size_t remaining = sizeof(sql);

  char *esc_table = escape_identifier(table, driver);
  if (!esc_table) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  int n = snprintf(p, remaining, "INSERT INTO %s (", esc_table);
  free(esc_table);

  if (n < 0 || (size_t)n >= remaining) {
    return LACE_ERR_INTERNAL_ERROR;
  }
  p += n;
  remaining -= (size_t)n;

  /* Column list */
  for (size_t i = 0; i < num_columns; i++) {
    char *esc_col = escape_identifier(columns[i], driver);
    if (!esc_col) {
      return LACE_ERR_OUT_OF_MEMORY;
    }

    n = snprintf(p, remaining, "%s%s", i > 0 ? ", " : "", esc_col);
    free(esc_col);

    if (n < 0 || (size_t)n >= remaining) {
      return LACE_ERR_INTERNAL_ERROR;
    }
    p += n;
    remaining -= (size_t)n;
  }

  n = snprintf(p, remaining, ") VALUES (");
  if (n < 0 || (size_t)n >= remaining) {
    return LACE_ERR_INTERNAL_ERROR;
  }
  p += n;
  remaining -= (size_t)n;

  /* Value list */
  for (size_t i = 0; i < num_columns; i++) {
    char *val_sql = value_to_sql(&values[i]);
    if (!val_sql) {
      return LACE_ERR_OUT_OF_MEMORY;
    }

    n = snprintf(p, remaining, "%s%s", i > 0 ? ", " : "", val_sql);
    free(val_sql);

    if (n < 0 || (size_t)n >= remaining) {
      return LACE_ERR_INTERNAL_ERROR;
    }
    p += n;
    remaining -= (size_t)n;
  }

  n = snprintf(p, remaining, ")");
  if (n < 0 || (size_t)n >= remaining) {
    return LACE_ERR_INTERNAL_ERROR;
  }

  LaceResult *result = NULL;
  int err = lace_exec(client, conn_id, sql, &result);

  /* Currently we don't return auto-generated PK - would need RETURNING clause */
  if (out_pk) *out_pk = NULL;
  if (out_num_pk) *out_num_pk = 0;

  lace_result_free(result);
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
