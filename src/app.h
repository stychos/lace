/*
 * lace - Database Viewer and Manager
 * Application state and lifecycle
 */

#ifndef LACE_APP_H
#define LACE_APP_H

#include <stdbool.h>

/* Application configuration */
typedef struct {
    char *connstr;          /* Connection string */
    bool  tui_mode;         /* Use TUI (default: true) */
    bool  help;             /* Show help */
    char *query;            /* Direct query mode */
} AppConfig;

/* Parse command line arguments */
bool app_parse_args(int argc, char **argv, AppConfig *config);

/* Free config resources */
void app_config_free(AppConfig *config);

/* Print usage */
void app_print_usage(const char *prog);

/* Run the application */
int app_run(AppConfig *config);

/* Version info */
#define LACE_VERSION "0.1.0"
#define LACE_NAME "lace"
#define LACE_DESCRIPTION "Database Viewer and Manager"

#endif /* LACE_APP_H */
