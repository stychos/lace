/*
 * Lace
 * Application state and lifecycle
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_APP_H
#define LACE_APP_H

#include <stdbool.h>

/* Application configuration */
typedef struct {
  char *connstr;     /* Connection string */
  bool help;         /* Show help */
  char *query;       /* Direct query mode */
  bool skip_session; /* --no-session flag */
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
