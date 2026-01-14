/*
 * Lace ncurses frontend
 * Entry point
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "app.h"
#include "tui.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* Global for signal handler */
static AppState *g_app = NULL;

static void signal_handler(int sig) {
  (void)sig;
  if (g_app) {
    g_app->running = false;
  }
}

static void print_usage(const char *prog) {
  fprintf(stderr, "Usage: %s [OPTIONS] [CONNECTION_STRING]\n", prog);
  fprintf(stderr, "\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -h, --help     Show this help\n");
  fprintf(stderr, "  -v, --version  Show version\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Connection string format:\n");
  fprintf(stderr, "  sqlite:///path/to/database.db\n");
  fprintf(stderr, "  postgres://user:pass@host:port/database\n");
  fprintf(stderr, "  mysql://user:pass@host:port/database\n");
  fprintf(stderr, "  mariadb://user:pass@host:port/database\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Examples:\n");
  fprintf(stderr, "  %s sqlite:///data.db\n", prog);
  fprintf(stderr, "  %s postgres://admin@localhost/mydb\n", prog);
  fprintf(stderr, "\n");
}

static void print_version(void) {
  printf("lace 0.1.0\n");
  printf("TUI database viewer/manager\n");
  printf("Using liblace client library\n");
}

int main(int argc, char **argv) {
  const char *connstr = NULL;
  int exit_code = 0;

  /* Parse arguments */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    }
    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
      print_version();
      return 0;
    }
    if (argv[i][0] == '-') {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
    /* Assume it's a connection string */
    if (!connstr) {
      connstr = argv[i];
    } else {
      fprintf(stderr, "Multiple connection strings not supported\n");
      return 1;
    }
  }

  /* Set locale for proper Unicode handling */
  setlocale(LC_ALL, "");

  /* Set up signal handlers */
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  /* Create application state */
  AppState *app = app_create();
  if (!app) {
    fprintf(stderr, "Failed to initialize application\n");
    return 1;
  }
  g_app = app;

  /* Connect if connection string provided */
  if (connstr) {
    int conn_idx = app_connect(app, connstr, NULL);
    if (conn_idx < 0) {
      fprintf(stderr, "Failed to connect: %s\n",
              app->status_message ? app->status_message : "Unknown error");
      app_destroy(app);
      return 1;
    }
  }

  /* Initialize TUI */
  TuiState *tui = tui_init(app);
  if (!tui) {
    fprintf(stderr, "Failed to initialize TUI\n");
    app_destroy(app);
    return 1;
  }

  /* Run main loop */
  tui_run(tui);

  /* Cleanup */
  tui_cleanup(tui);
  app_destroy(app);
  g_app = NULL;

  return exit_code;
}
