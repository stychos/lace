/*
 * laced - Lace Database Daemon
 * Entry point and initialization
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util/mem.h>
#include <util/str.h>

#include "server.h"

/* Global state */
static volatile sig_atomic_t g_shutdown_requested = 0;

/* Transport mode */
typedef enum {
  MODE_STDIO,
  MODE_UNIX,
  MODE_TCP
} TransportMode;

/* Configuration */
typedef struct {
  TransportMode mode;
  char *socket_path;     /* Unix socket path (NULL for default) */
  char *bind_addr;       /* TCP bind address (NULL for localhost) */
  int port;              /* TCP port (0 for default 7433) */
  size_t max_clients;    /* Max concurrent clients (0 for default 64) */
  bool daemonize;        /* Fork to background */
  char *pidfile;         /* PID file path */
} DaemonConfig;

/* Signal handler for graceful shutdown */
static void signal_handler(int sig) {
  (void)sig;
  g_shutdown_requested = 1;
}

/* Print usage information */
static void print_usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [OPTIONS]\n"
          "\n"
          "Lace database daemon - JSON-RPC server for database operations.\n"
          "\n"
          "Transport modes (mutually exclusive):\n"
          "  --unix [PATH]        Listen on Unix socket (default)\n"
          "                       Default: $XDG_RUNTIME_DIR/laced.sock or /tmp/laced-{uid}.sock\n"
          "  --tcp [HOST:]PORT    Listen on TCP socket\n"
          "                       Default: localhost:7433\n"
          "  --stdio              Use stdin/stdout (for embedded/spawn mode)\n"
          "\n"
          "Options:\n"
          "  --daemonize, -d      Fork to background (socket modes only)\n"
          "  --pidfile PATH       Write PID to file\n"
          "  --max-clients N      Maximum concurrent clients (default: 64)\n"
          "  -h, --help           Show this help message\n"
          "  -v, --version        Show version information\n"
          "\n"
          "Examples:\n"
          "  %s                      # Unix socket with default path (default)\n"
          "  %s --unix /tmp/my.sock  # Unix socket with custom path\n"
          "  %s --tcp 7433           # TCP on localhost:7433\n"
          "  %s --tcp 0.0.0.0:7433   # TCP on all interfaces\n"
          "  %s -d                   # Background daemon with Unix socket\n"
          "  %s --stdio              # Use with liblace spawn mode\n",
          prog, prog, prog, prog, prog, prog, prog);
}

/* Print version information */
static void print_version(void) {
  fprintf(stderr, "laced version 0.1.0\n");
  fprintf(stderr, "Protocol version: 1.0\n");
}

/* Parse TCP address in format [HOST:]PORT */
static bool parse_tcp_addr(const char *arg, char **host, int *port) {
  const char *colon = strrchr(arg, ':');
  if (colon) {
    /* HOST:PORT format */
    size_t host_len = (size_t)(colon - arg);
    *host = safe_malloc(host_len + 1);
    if (!*host) {
      return false; /* Allocation failed */
    }
    memcpy(*host, arg, host_len);
    (*host)[host_len] = '\0';
    *port = atoi(colon + 1);
  } else {
    /* PORT only */
    *host = NULL;
    *port = atoi(arg);
  }
  if (*port <= 0 || *port > 65535) {
    free(*host);
    *host = NULL;
    return false;
  }
  return true;
}

/* Parse command line arguments */
static int parse_args(int argc, char **argv, DaemonConfig *config) {
  /* Defaults - Unix socket is the default mode */
  config->mode = MODE_UNIX;
  config->socket_path = NULL;
  config->bind_addr = NULL;
  config->port = 0;
  config->max_clients = 0;
  config->daemonize = false;
  config->pidfile = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return -1;
    }
    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
      print_version();
      return -1;
    }
    if (strcmp(argv[i], "--stdio") == 0) {
      config->mode = MODE_STDIO;
      continue;
    }
    if (strcmp(argv[i], "--unix") == 0) {
      config->mode = MODE_UNIX;
      /* Check for optional path argument */
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        i++;
        config->socket_path = str_dup(argv[i]);
      }
      continue;
    }
    if (strcmp(argv[i], "--tcp") == 0) {
      config->mode = MODE_TCP;
      /* Require address argument */
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        i++;
        if (!parse_tcp_addr(argv[i], &config->bind_addr, &config->port)) {
          fprintf(stderr, "Invalid TCP address: %s\n", argv[i]);
          return -1;
        }
      } else {
        /* Default to localhost:7433 */
        config->port = 7433;
      }
      continue;
    }
    if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemonize") == 0) {
      config->daemonize = true;
      continue;
    }
    if (strcmp(argv[i], "--pidfile") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--pidfile requires an argument\n");
        return -1;
      }
      i++;
      config->pidfile = str_dup(argv[i]);
      continue;
    }
    if (strcmp(argv[i], "--max-clients") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--max-clients requires an argument\n");
        return -1;
      }
      i++;
      config->max_clients = (size_t)atoi(argv[i]);
      if (config->max_clients == 0 || config->max_clients > 10000) {
        fprintf(stderr, "Invalid --max-clients value: %s (must be 1-10000)\n", argv[i]);
        return -1;
      }
      continue;
    }

    fprintf(stderr, "Unknown option: %s\n", argv[i]);
    print_usage(argv[0]);
    return -1;
  }

  /* Validate options */
  if (config->daemonize && config->mode == MODE_STDIO) {
    fprintf(stderr, "Cannot daemonize in stdio mode\n");
    return -1;
  }

  return 0;
}

/* Free config resources */
static void free_config(DaemonConfig *config) {
  free(config->socket_path);
  free(config->bind_addr);
  free(config->pidfile);
}

/* Write PID file */
static bool write_pidfile(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) {
    return false;
  }
  fprintf(f, "%d\n", (int)getpid());
  fclose(f);
  return true;
}

/* Remove PID file */
static void remove_pidfile(const char *path) {
  if (path) {
    unlink(path);
  }
}

/* Daemonize the process */
static bool do_daemonize(void) {
  pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid > 0) {
    /* Parent exits */
    _exit(0);
  }

  /* Child continues */
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

  return true;
}

int main(int argc, char **argv) {
  DaemonConfig config = {0};

  /* Parse arguments */
  if (parse_args(argc, argv, &config) < 0) {
    free_config(&config);
    return 1;
  }

  /* Daemonize if requested */
  if (config.daemonize) {
    if (!do_daemonize()) {
      fprintf(stderr, "Failed to daemonize\n");
      free_config(&config);
      return 1;
    }
  }

  /* Set up signal handlers */
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);

  /* Ignore SIGPIPE - we handle write errors explicitly */
  signal(SIGPIPE, SIG_IGN);

  /* Write PID file */
  if (config.pidfile) {
    if (!write_pidfile(config.pidfile)) {
      fprintf(stderr, "Failed to write PID file: %s\n", config.pidfile);
      free_config(&config);
      return 1;
    }
  }

  /* Initialize server */
  LacedServer *server = laced_server_create();
  if (!server) {
    fprintf(stderr, "Failed to create server\n");
    remove_pidfile(config.pidfile);
    free_config(&config);
    return 1;
  }

  /* Run server loop based on transport mode */
  int result = 0;
  switch (config.mode) {
  case MODE_STDIO:
    result = laced_server_run_stdio(server, &g_shutdown_requested);
    break;

  case MODE_UNIX:
    result = laced_server_run_unix(server, config.socket_path,
                                   config.max_clients, &g_shutdown_requested);
    break;

  case MODE_TCP:
    result = laced_server_run_tcp(server, config.bind_addr, config.port,
                                  config.max_clients, &g_shutdown_requested);
    break;
  }

  /* Cleanup */
  laced_server_destroy(server);
  remove_pidfile(config.pidfile);
  free_config(&config);

  return result;
}
