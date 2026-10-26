/*
 * Lace
 * Application implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "app.h"
#include "../config/config.h"
#include "../config/session.h"
#include "../core/app_state.h"
#include "util/connstr.h"
#include "../db_compat.h"
#include "../tui.h"
#include "../../liblace/include/util/str.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * Create a lace client with the specified spawn mode. LACE_CONN_AUTO
 * attaches to an already-running daemon first and only spawns when none
 * answers; it also records idle_timeout, so a daemon respawned later (after a
 * crash) gets the configured timeout too.
 */
static lace_client_t *create_client_with_mode(LaceSpawnMode spawn_mode,
                                              int idle_timeout) {
  LaceClientConfig config = {0};
  config.mode = LACE_CONN_AUTO;
  config.spawn_mode = spawn_mode;
  config.idle_timeout = idle_timeout;
  return lace_client_create_with_config(&config);
}

/* volatile: the writes must not be optimized away */
static void blank(char *from, const char *to) {
  for (volatile char *v = from; v < (volatile const char *)to; v++)
    *v = 'x';
}

/* Blank the password of the "user:password@" starting at seg, where at is
 * its '@': everything after the first ':' (a password may hold a raw '/'
 * or '@' even though the URL then does not parse) */
static void scrub_userinfo(char *seg, char *at) {
  char *colon = memchr(seg, ':', (size_t)(at - seg));
  if (colon)
    blank(colon + 1, at);
}

/* Blank the values of password-like query parameters (?password=...) */
static void scrub_query(char *q) {
  static const char *const keys[] = {"password", "passwd", "pwd",
                                     "ssh_password", "sshpassword", NULL};
  for (char *p = q; p && *p; p = strchr(p, '&')) {
    p++; /* Past '?' or '&' */
    char *eq = strchr(p, '=');
    char *end = strchr(p, '&');
    if (!eq || (end && eq > end))
      continue;
    for (int k = 0; keys[k]; k++) {
      if ((size_t)(eq - p) == strlen(keys[k]) && strncasecmp(p, keys[k], eq - p) == 0)
        blank(eq + 1, end ? end : eq + 1 + strlen(eq + 1));
    }
  }
}

/* Blank every password in a connection URL in place, as far as the text
 * allows (also in URLs the parser would reject): scheme://user:PASS@host/db,
 * the SSH and database passwords of a +ssh URL, and ?password= */
static void scrub_url_passwords(char *url) {
  char *scheme_end = strstr(url, "://");
  if (!scheme_end)
    return;
  bool ssh = false;
  for (const char *c = url; c + 4 <= scheme_end; c++) {
    if (strncmp(c, "+ssh", 4) == 0)
      ssh = true;
  }
  char *start = scheme_end + 3;
  char *query = strchr(start, '?');
  char *limit = query ? query : start + strlen(start);
  /* The last '@' before the query ends the database's user part */
  char *last_at = NULL;
  for (char *c = start; c < limit; c++) {
    if (*c == '@')
      last_at = c;
  }
  if (ssh) {
    /* sshuser:sshpass@sshhost:port/dbuser:dbpass@dbhost/db: the SSH part
     * ends at the first '@', the database part at the last */
    char *first_at = memchr(start, '@', (size_t)(limit - start));
    if (first_at && first_at != last_at) {
      scrub_userinfo(start, first_at);
      char *db_seg = memchr(first_at, '/', (size_t)(limit - first_at));
      if (db_seg && db_seg < last_at)
        scrub_userinfo(db_seg + 1, last_at);
    } else if (first_at) {
      scrub_userinfo(start, first_at);
    }
  } else if (last_at) {
    scrub_userinfo(start, last_at);
  }
  if (query)
    scrub_query(query);
}

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

    /* Blank the passwords in argv so /proc/<pid>/cmdline and ps do not
     * show them (config->connstr keeps its own copy) */
    scrub_url_passwords(argv[optind]);
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

/* Convert TUI spawn mode to liblace spawn mode */
static LaceSpawnMode convert_spawn_mode(int tui_mode) {
  switch (tui_mode) {
  case SPAWN_MODE_STDIO:
    return LACE_SPAWN_STDIO;
  case SPAWN_MODE_TCP:
    return LACE_SPAWN_TCP;
  case SPAWN_MODE_DETECT:
    return LACE_SPAWN_DETECT;
  case SPAWN_MODE_UNIX:
  default:
    return LACE_SPAWN_UNIX;
  }
}

static int run_query_mode(AppConfig *config) {
  /* Load application config to get spawn mode preference */
  Config *app_config = config_load(NULL);
  LaceSpawnMode spawn_mode = LACE_SPAWN_UNIX; /* Default */
  int idle_timeout = CONFIG_IDLE_TIMEOUT_DEFAULT;
  if (app_config) {
    spawn_mode = convert_spawn_mode(app_config->general.daemon_spawn_mode);
    idle_timeout = app_config->general.daemon_idle_timeout;
  }

  /* Create liblace client with configured spawn mode */
  lace_client_t *client = create_client_with_mode(spawn_mode, idle_timeout);
  config_free(app_config);

  if (!client) {
    fprintf(stderr, "Failed to allocate client\n");
    return 1;
  }
  if (!lace_client_connected(client)) {
    fprintf(stderr, "Failed to connect to lace daemon: %s\n",
            lace_client_error(client));
    lace_client_destroy(client);
    return 1;
  }

  char *err = NULL;
  DbConnection *conn = db_connect(client, config->connstr, &err);

  if (!conn) {
    fprintf(stderr, "Connection failed: %s\n", err ? err : "Unknown error");
    free(err);
    lace_client_destroy(client);
    return 1;
  }

  ResultSet *rs = db_query(conn, config->query, &err);
  if (!rs) {
    fprintf(stderr, "Query failed: %s\n", err ? err : "Unknown error");
    free(err);
    db_disconnect(conn);
    lace_client_destroy(client);
    return 1;
  }

  /* Statements without a result set (INSERT/UPDATE/DDL) report a count */
  if (rs->num_columns == 0) {
    printf("%lld rows affected\n", (long long)rs->rows_affected);
    db_result_free(rs);
    db_disconnect(conn);
    lace_client_destroy(client);
    return 0;
  }

  /* Validate result set structure */
  if (!rs->columns || (rs->num_rows > 0 && !rs->rows)) {
    fprintf(stderr, "Invalid result set structure\n");
    db_result_free(rs);
    db_disconnect(conn);
    lace_client_destroy(client);
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

  printf("\n%zu rows%s\n", rs->num_rows,
         rs->truncated ? " (truncated: result exceeded the daemon's limit)" : "");

  db_result_free(rs);
  db_disconnect(conn);
  lace_client_destroy(client);
  return 0;
}

/* Password callback for session restore - wraps TUI password dialog */
static char *tui_password_callback(void *user_data, const char *title,
                                   const char *label, const char *error_msg) {
  TuiState *state = (TuiState *)user_data;
  return tui_show_password_dialog(state, title, label, error_msg);
}

static int run_tui_mode(AppConfig *config) {
  /* Load application config first to get spawn mode preference */
  Config *early_config = config_load(NULL);
  LaceSpawnMode spawn_mode = LACE_SPAWN_UNIX; /* Default */
  int idle_timeout = CONFIG_IDLE_TIMEOUT_DEFAULT;
  if (early_config) {
    spawn_mode = convert_spawn_mode(early_config->general.daemon_spawn_mode);
    idle_timeout = early_config->general.daemon_idle_timeout;
  }

  /* Create liblace client with configured spawn mode */
  lace_client_t *client = create_client_with_mode(spawn_mode, idle_timeout);
  if (!client) {
    config_free(early_config);
    fprintf(stderr, "Failed to allocate client\n");
    return 1;
  }
  if (!lace_client_connected(client)) {
    fprintf(stderr, "Failed to connect to lace daemon: %s\n",
            lace_client_error(client));
    lace_client_destroy(client);
    config_free(early_config);
    return 1;
  }

  AppState app;
  TuiState state = {
      0}; /* Zero-initialize to prevent use of uninitialized fields */

  app_state_init(&app);
  app.client = client;

  /* Use the already-loaded config instead of loading again */
  if (early_config) {
    config_free(app.config);
    app.config = early_config;
  }

  if (!tui_init(&state, &app)) {
    fprintf(stderr, "Failed to initialize TUI\n");
    lace_client_destroy(client);
    return 1;
  }

  bool session_restored = false;
  /* Session restore and saved workspaces ask for missing passwords */
  session_set_password_callback(tui_password_callback, &state);

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

  /* An older config.json may carry defaults that are now other actions' keys */
  char *conflicts = config_hotkey_conflicts_text(app.config);
  if (conflicts) {
    tui_set_status(&state, "Hotkey conflicts in config.json (%s lists them, R resets all): %s",
                   "the configuration dialog", conflicts);
    free(conflicts);
  }

  tui_run(&state);
  tui_cleanup(&state);
  lace_client_destroy(client);

  return 0;
}

int app_run(AppConfig *config) {
  if (!config)
    return 1;

  if (config->help) {
    app_print_usage("lace");
    return 0;
  }

  if (config->query && config->connstr) {
    return run_query_mode(config);
  } else {
    return run_tui_mode(config);
  }
}
