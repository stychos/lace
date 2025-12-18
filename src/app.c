/*
 * lace - Database Viewer and Manager
 * Application implementation
 */

#include "app.h"
#include "db/db.h"
#include "tui/tui.h"
#include "util/str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static struct option long_options[] = {
    {"help",    no_argument,       NULL, 'h'},
    {"version", no_argument,       NULL, 'V'},
    {"verbose", no_argument,       NULL, 'v'},
    {"query",   required_argument, NULL, 'q'},
    {"no-tui",  no_argument,       NULL, 'n'},
    {NULL,      0,                 NULL,  0 }
};

bool app_parse_args(int argc, char **argv, AppConfig *config) {
    if (!config) return false;

    memset(config, 0, sizeof(AppConfig));
    config->tui_mode = true;

    int opt;
    while ((opt = getopt_long(argc, argv, "hVvq:n", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                config->help = true;
                break;
            case 'V':
                config->version = true;
                break;
            case 'v':
                config->verbose = true;
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
        config->connstr = str_dup(argv[optind]);
    }

    return true;
}

void app_config_free(AppConfig *config) {
    if (!config) return;
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
    printf("  -V, --version    Show version information\n");
    printf("  -v, --verbose    Verbose output\n");
    printf("  -q, --query SQL  Execute query and exit\n");
    printf("  -n, --no-tui     Disable TUI mode\n");
    printf("\n");
    printf("TUI Navigation:\n");
    printf("  Arrow keys, hjkl  Move cursor\n");
    printf("  Page Up/Down      Scroll pages\n");
    printf("  Home/End          Go to first/last column\n");
    printf("  a, g, Ctrl+Home   Go to first row\n");
    printf("  z, G, Ctrl+End    Go to last row\n");
    printf("  /, F5, Ctrl+G     Go to row number\n");
    printf("  t, F9             Toggle tables sidebar\n");
    printf("  s, F3             Show table schema\n");
    printf("  c, F2             Connect to database\n");
    printf("  q, F10, Ctrl+Q    Quit\n");
    printf("  F1, ?             Show help\n");
    printf("\n");
    printf("Tabs:\n");
    printf("  ]/F6, [/F7        Next/previous tab\n");
    printf("  +/= (sidebar)     Open table in new tab\n");
    printf("  -/_               Close current tab\n");
    printf("\n");
    printf("Editing:\n");
    printf("  Enter             Edit cell (inline)\n");
    printf("  e, F4             Edit cell (modal)\n");
    printf("  n, Ctrl+N         Set cell to NULL\n");
    printf("  d, Ctrl+D         Set cell to empty\n");
    printf("  x, Delete         Delete row\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s sqlite:///data.db\n", prog);
    printf("  %s postgres://localhost/mydb\n", prog);
    printf("  %s -q 'SELECT * FROM users' sqlite:///data.db\n", prog);
}

void app_print_version(void) {
    printf("%s %s\n", LACE_NAME, LACE_VERSION);
    printf("Compiled with Fil-C\n");
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
        if (i > 0) printf("\t");
        printf("%s", rs->columns[i].name ? rs->columns[i].name : "");
    }
    printf("\n");

    /* Print separator */
    for (size_t i = 0; i < rs->num_columns; i++) {
        if (i > 0) printf("\t");
        printf("---");
    }
    printf("\n");

    /* Print rows */
    for (size_t row = 0; row < rs->num_rows; row++) {
        for (size_t col = 0; col < rs->num_columns; col++) {
            if (col > 0) printf("\t");
            char *str = db_value_to_string(&rs->rows[row].cells[col]);
            printf("%s", str ? str : "");
            free(str);
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
    if (!config) return 1;

    if (config->help) {
        app_print_usage("lace");
        return 0;
    }

    if (config->version) {
        app_print_version();
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
