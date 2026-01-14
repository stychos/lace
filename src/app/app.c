/*
 * Lace
 * Application implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "app.h"
#include "../config/session.h"
#include "../core/app_state.h"
#include "../db/connstr.h"
#include "../db/db.h"
#include "../tui/ncurses/tui.h"
#include "../util/str.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct option long_options[] = {{"help", no_argument, NULL, 'h'},
                                       {"query", required_argument, NULL, 'q'},
                                       {"no-session", no_argument, NULL, 's'},
                                       {NULL, 0, NULL, 0}};

bool app_parse_args(int argc, char **argv, AppConfig *config) {
  if (!config)
    return false;

  memset(config, 0, sizeof(AppConfig));

  int opt;
  while ((opt = getopt_long(argc, argv, "hq:s", long_options, NULL)) != -1) {
    switch (opt) {
    case 'h':
      config->help = true;
      break;
    case 'q':
      config->query = str_dup(optarg);
      break;
    case 's':
      config->skip_session = true;
      break;
    default:
      return false;
    }
  }

  /* Get connection string from remaining argument */
  if (optind < argc) {
    const char *connstr = argv[optind];

    /* Enforce maximum connection string length to prevent issues */
    size_t connstr_len = strlen(connstr);
    if (connstr_len > 4096) {
      fprintf(stderr, "Connection string too long (max 4096 characters)\n");
      str_secure_free(config->query);
      return false;
    }

    /* Check if it's a connection string (has ://) or a file path */
    if (!strstr(connstr, "://")) {
      /* No scheme - try to detect if it's a SQLite file */
      char *path_err = NULL;
      char *sqlite_connstr = connstr_from_path(connstr, &path_err);
      if (sqlite_connstr) {
        config->connstr = sqlite_connstr;
      } else {
        fprintf(stderr, "%s\n", path_err ? path_err : "Invalid file path");
        free(path_err);
        str_secure_free(config->query);
        return false;
      }
    } else {
      config->connstr = str_dup(connstr);
    }
    if (!config->connstr) {
      fprintf(stderr, "Memory allocation failed\n");
      str_secure_free(config->query);
      return false;
    }

    /* Securely clear password in argv to prevent leakage via /proc/cmdline.
     * Password is between first ':' after '://' and '@'.
     * Format: scheme://user:password@host/db */
    char *scheme_end = strstr(argv[optind], "://");
    if (scheme_end) {
      char *user_start = scheme_end + 3;
      char *pass_start = strchr(user_start, ':');
      if (pass_start) {
        pass_start++; /* Move past ':' */
        char *pass_end = strchr(pass_start, '@');
        if (pass_end && pass_end > pass_start) {
          /* Use volatile to prevent compiler from optimizing away the write */
          volatile char *vpass = pass_start;
          while (vpass < (volatile char *)pass_end) {
            *vpass++ = 'x';
          }
        }
      }
    }
  }

  return true;
}

void app_config_free(AppConfig *config) {
  if (!config)
    return;
  /* Use secure free for connection string and query as they may contain
   * passwords */
  str_secure_free(config->connstr);
  str_secure_free(config->query);
}

void app_print_usage(const char *prog) {
  printf("Usage: %s [OPTIONS] <connection-string | file.db>\n", prog);
  printf("\n");
  printf("%s - %s\n", LACE_NAME, LACE_DESCRIPTION);
  printf("\n");
  printf("Connection string format:\n");
  printf("  sqlite:///path/to/database.db\n");
  printf("  postgres://user:pass@host:5432/database\n");
  printf("  mysql://user:pass@host:3306/database\n");
  printf("\n");
  printf("For SQLite, you can also pass a plain file path:\n");
  printf("  ./database.db, /path/to/file.sqlite, etc.\n");
  printf("\n");
  printf("Options:\n");
  printf("  -h, --help        Show this help message\n");
  printf("  -q, --query SQL   Execute query and exit\n");
  printf("  -s, --no-session  Don't restore previous session\n");
  printf("\n");
  printf("Examples:\n");
  printf("  %s ./data.db\n", prog);
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

  /* Validate result set structure */
  if (!rs->columns || (rs->num_rows > 0 && !rs->rows)) {
    fprintf(stderr, "Invalid result set structure\n");
    db_result_free(rs);
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

/* Password callback for session restore - wraps TUI password dialog */
static char *tui_password_callback(void *user_data, const char *title,
                                   const char *label, const char *error_msg) {
  TuiState *state = (TuiState *)user_data;
  return tui_show_password_dialog(state, title, label, error_msg);
}

static int run_tui_mode(AppConfig *config) {
  AppState app;
  TuiState state = {
      0}; /* Zero-initialize to prevent use of uninitialized fields */

  app_state_init(&app);

  if (!tui_init(&state, &app)) {
    fprintf(stderr, "Failed to initialize TUI\n");
    return 1;
  }

  bool session_restored = false;

  if (config->connstr) {
    /* Explicit connection string provided - use it */
    if (!tui_connect(&state, config->connstr)) {
      /* Error message already shown in TUI */
    }
  } else if (!config->skip_session && app.config &&
             app.config->general.restore_session) {
    /* No connection string - try to restore previous session */
    char *session_err = NULL;
    Session *session = session_load(&session_err);

    if (session) {
      /* Register password callback for session restore */
      session_set_password_callback(tui_password_callback, &state);

      char *restore_err = NULL;
      if (session_restore(&state, session, &restore_err)) {
        session_restored = true;
        /* Initialize widgets and sync state for restored tab */
        tab_restore(&state);
        tui_refresh(&state);
      } else {
        /* Restoration failed - show error and fall through to connect dialog */
        if (restore_err) {
          tui_set_error(&state, "Session restore failed: %s", restore_err);
          free(restore_err);
        }
      }
      session_free(session);
    }
    free(session_err);
  }

  if (!config->connstr && !session_restored) {
    /* No connection and no session - show connect dialog */
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
  } else {
    result = run_tui_mode(config);
  }

  db_cleanup();
  return result;
}
