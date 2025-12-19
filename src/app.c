/*
 * lace - Database Viewer and Manager
 * Application implementation
 */

#include "app.h"
#include "db/db.h"
#include "tui/tui.h"
#include "util/str.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct option long_options[] = {{"help", no_argument, NULL, 'h'},
                                       {"query", required_argument, NULL, 'q'},
                                       {"no-tui", no_argument, NULL, 'n'},
                                       {NULL, 0, NULL, 0}};

bool app_parse_args(int argc, char **argv, AppConfig *config) {
  if (!config)
    return false;

  memset(config, 0, sizeof(AppConfig));
  config->tui_mode = true;

  int opt;
  while ((opt = getopt_long(argc, argv, "hq:n", long_options, NULL)) != -1) {
    switch (opt) {
    case 'h':
      config->help = true;
      break;
    case 'q':
      config->query = str_dup(optarg);
      config->tui_mode = false;
      break;
    case 'n':
      config->tui_mode = false;
      break;
    default:
      return false;
    }
  }

  /* Get connection string from remaining argument */
  if (optind < argc) {
    const char *connstr = argv[optind];
    /* Basic validation: must contain :// scheme separator */
    if (!strstr(connstr, "://")) {
      fprintf(stderr,
              "Invalid connection string format. Expected: driver://...\n");
      fprintf(stderr, "Examples: sqlite:///path.db, postgres://localhost/db\n");
      return false;
    }
    config->connstr = str_dup(connstr);
    if (!config->connstr) {
      fprintf(stderr, "Memory allocation failed\n");
      return false;
    }
  }

  return true;
}

void app_config_free(AppConfig *config) {
  if (!config)
    return;
  free(config->connstr);
  free(config->query);
}

void app_print_usage(const char *prog) {
  printf("Usage: %s [OPTIONS] <connection-string>\n", prog);
  printf("\n");
  printf("%s - %s\n", LACE_NAME, LACE_DESCRIPTION);
  printf("\n");
  printf("Connection string format:\n");
  printf("  sqlite:///path/to/database.db\n");
  printf("  postgres://user:pass@host:5432/database\n");
  printf("  mysql://user:pass@host:3306/database\n");
  printf("\n");
  printf("Options:\n");
  printf("  -h, --help       Show this help message\n");
  printf("  -q, --query SQL  Execute query and exit\n");
  printf("  -n, --no-tui     Disable TUI mode\n");
  printf("\n");
  printf("Examples:\n");
  printf("  %s sqlite:///data.db\n", prog);
  printf("  %s postgres://localhost/mydb\n", prog);
  printf("  %s -q 'SELECT * FROM users' sqlite:///data.db\n", prog);
  printf("\n");
  printf("Press ? or F1 in TUI for keyboard shortcuts.\n");
}

static int run_query_mode(AppConfig *config) {
  char *err = NULL;
  DbConnection *conn = db_connect(config->connstr, &err);

  if (!conn) {
    fprintf(stderr, "Connection failed: %s\n", err ? err : "Unknown error");
    free(err);
    return 1;
  }

  ResultSet *rs = db_query(conn, config->query, &err);
  if (!rs) {
    fprintf(stderr, "Query failed: %s\n", err ? err : "Unknown error");
    free(err);
    db_disconnect(conn);
    return 1;
  }

  /* Print column headers */
  for (size_t i = 0; i < rs->num_columns; i++) {
    if (i > 0)
      printf("\t");
    printf("%s", rs->columns[i].name ? rs->columns[i].name : "");
  }
  printf("\n");

  /* Print separator */
  for (size_t i = 0; i < rs->num_columns; i++) {
    if (i > 0)
      printf("\t");
    printf("---");
  }
  printf("\n");

  /* Print rows */
  for (size_t row = 0; row < rs->num_rows; row++) {
    Row *r = &rs->rows[row];
    for (size_t col = 0; col < rs->num_columns; col++) {
      if (col > 0)
        printf("\t");
      if (r->cells && col < r->num_cells) {
        char *str = db_value_to_string(&r->cells[col]);
        printf("%s", str ? str : "");
        free(str);
      }
    }
    printf("\n");
  }

  printf("\n%zu rows\n", rs->num_rows);

  db_result_free(rs);
  db_disconnect(conn);
  return 0;
}

static int run_list_tables(AppConfig *config) {
  char *err = NULL;
  DbConnection *conn = db_connect(config->connstr, &err);

  if (!conn) {
    fprintf(stderr, "Connection failed: %s\n", err ? err : "Unknown error");
    free(err);
    return 1;
  }

  size_t count;
  char **tables = db_list_tables(conn, &count, &err);

  if (!tables) {
    fprintf(stderr, "Failed to list tables: %s\n", err ? err : "Unknown error");
    free(err);
    db_disconnect(conn);
    return 1;
  }

  printf("Tables in %s:\n", conn->database);
  for (size_t i = 0; i < count; i++) {
    printf("  %s\n", tables[i]);
    free(tables[i]);
  }
  free(tables);

  db_disconnect(conn);
  return 0;
}

static int run_tui_mode(AppConfig *config) {
  TuiState state;

  if (!tui_init(&state)) {
    fprintf(stderr, "Failed to initialize TUI\n");
    return 1;
  }

  if (config->connstr) {
    if (!tui_connect(&state, config->connstr)) {
      /* Error message already shown in TUI */
    }
  } else {
    /* No connection string - show connect dialog */
    tui_refresh(&state);
    tui_show_connect_dialog(&state);
  }

  tui_run(&state);
  tui_cleanup(&state);

  return 0;
}

int app_run(AppConfig *config) {
  if (!config)
    return 1;

  if (config->help) {
    app_print_usage("lace");
    return 0;
  }

  /* Initialize database subsystem */
  db_init();

  int result;

  if (config->query && config->connstr) {
    result = run_query_mode(config);
  } else if (!config->tui_mode && config->connstr) {
    result = run_list_tables(config);
  } else {
    result = run_tui_mode(config);
  }

  db_cleanup();
  return result;
}
