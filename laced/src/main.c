/*
 * laced - Lace Database Daemon
 * Entry point and initialization
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

/* Global state */
static volatile sig_atomic_t g_shutdown_requested = 0;

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
          "Options:\n"
          "  -h, --help     Show this help message\n"
          "  -v, --version  Show version information\n"
          "  --stdio        Use stdin/stdout for communication (default)\n"
          "\n"
          "The daemon communicates via JSON-RPC 2.0 over stdin/stdout.\n"
          "It is typically spawned by liblace client library.\n",
          prog);
}

/* Print version information */
static void print_version(void) {
  fprintf(stderr, "laced version 0.1.0\n");
  fprintf(stderr, "Protocol version: 1.0\n");
}

/* Parse command line arguments */
static int parse_args(int argc, char **argv, bool *use_stdio) {
  *use_stdio = true; /* Default to stdio mode */

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
      *use_stdio = true;
      continue;
    }
    fprintf(stderr, "Unknown option: %s\n", argv[i]);
    print_usage(argv[0]);
    return -1;
  }

  return 0;
}

int main(int argc, char **argv) {
  bool use_stdio = true;

  /* Parse arguments */
  if (parse_args(argc, argv, &use_stdio) < 0) {
    return 1;
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

  /* Initialize server */
  LacedServer *server = laced_server_create();
  if (!server) {
    fprintf(stderr, "Failed to create server\n");
    return 1;
  }

  /* Run server loop */
  int result = 0;
  if (use_stdio) {
    result = laced_server_run_stdio(server, &g_shutdown_requested);
  }

  /* Cleanup */
  laced_server_destroy(server);

  return result;
}
