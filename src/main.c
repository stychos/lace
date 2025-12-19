/*
 * lace - Database Viewer and Manager
 *
 * A TUI database viewer/manager for Linux
 * Supports: SQLite, PostgreSQL, MySQL
 */

#include "app.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  AppConfig config;

  if (!app_parse_args(argc, argv, &config)) {
    app_print_usage(argv[0]);
    return 1;
  }

  int result = app_run(&config);

  app_config_free(&config);
  return result;
}
