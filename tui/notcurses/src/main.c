#include "app.h"
#include "config/config.h"
#include "core/conn_fields.h"
#include "core/query_mode.h"
#include "util/str.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
  printf("Usage: %s [OPTIONS] [connection-string | file.db ...]\n\n", prog);
  printf("Options:\n");
  printf("  -h, --help         Show this help message\n");
  printf("  -q, --query SQL    Run SQL against the one connection given, print the result and exit\n");
  printf("  -f, --format FMT   How -q prints it: %s\n", query_out_format_names());
  printf("                     (default table; SQL ending in \\G prints vertical)\n");
  printf("  -s, --no-session   Don't restore the previous session\n");
  printf("      --version      Print the name and exit\n\n");
  printf("Examples:\n");
  printf("  %s ./data.db postgres://localhost/mydb\n", prog);
  printf("  %s -q 'SELECT * FROM users' -f json ./data.db\n", prog);
}

/* An option's value: "--opt VALUE", "--opt=VALUE", "-o VALUE" or "-oVALUE";
 * NULL when argv[*i] is not that option (*missing when it lacks its value) */
static const char *option_value(int argc, char **argv, int *i, const char *shortopt, const char *longopt,
                                bool *missing) {
  const char *a = argv[*i];
  size_t ll = strlen(longopt);
  if (!strcmp(a, shortopt) || !strcmp(a, longopt)) {
    if (*i + 1 >= argc) {
      *missing = true;
      return NULL;
    }
    return argv[++*i];
  }
  if (!strncmp(a, longopt, ll) && a[ll] == '=')
    return a + ll + 1;
  if (!strncmp(a, shortopt, 2) && a[2])
    return a + 2;
  return NULL;
}

/* -q and -f: run the query without starting the screen. Returns -1 when
 * the arguments ask for no query (the app starts), else the exit status. */
static int query_mode(int argc, char **argv) {
  const char *sql = NULL, *format = NULL, *target = NULL;
  int targets = 0;
  for (int i = 1; i < argc; i++) {
    bool missing = false;
    const char *v;
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage(argv[0]);
      return 0;
    } else if ((v = option_value(argc, argv, &i, "-q", "--query", &missing))) {
      sql = v;
    } else if ((v = option_value(argc, argv, &i, "-f", "--format", &missing))) {
      format = v;
    } else if (missing) {
      fprintf(stderr, "lace-nc: %s needs a value\n", argv[i]);
      return 2;
    } else if (argv[i][0] != '-' || !argv[i][1]) {
      target = argv[i];
      targets++;
    }
  }
  if (!sql && !format)
    return -1;
  if (!sql) {
    fprintf(stderr, "lace-nc: -f/--format goes with -q\n");
    return 2;
  }
  QueryOutFormat fmt = QUERY_OUT_TABLE;
  if (format && !query_out_format_from_name(format, &fmt)) {
    fprintf(stderr, "lace-nc: unknown format '%s' (%s)\n", format, query_out_format_names());
    return 2;
  }
  if (targets != 1) {
    fprintf(stderr, "lace-nc: -q needs exactly one connection string or SQLite file\n");
    return 2;
  }
  char *err = NULL;
  char *connstr = conn_target_to_connstr(target, &err);
  if (!connstr) {
    fprintf(stderr, "lace-nc: %s\n", err ? err : "not a connection string or SQLite file");
    free(err);
    return 2;
  }
  char *query = str_dup(sql);
  if (query_out_strip_vertical(query) && !format)
    fmt = QUERY_OUT_VERTICAL;
  Config *cfg = config_load(NULL);
  int spawn = cfg ? cfg->general.daemon_spawn_mode : 0;
  int idle = cfg ? cfg->general.daemon_idle_timeout : 0;
  config_free(cfg);
  int rc = query_mode_run(spawn, idle, connstr, query, fmt);
  str_secure_free(query);
  str_secure_free(connstr);
  return rc;
}

int main(int argc, char **argv) {
  int q = query_mode(argc, argv);
  if (q >= 0)
    return q;
  char *err = NULL;
  NcApp *a = app_new(argc, argv, &err);
  if (!a) {
    fprintf(stderr, "lace-nc: %s\n", err ? err : "cannot start");
    free(err);
    return 2;
  }
  int rc = app_run(a);
  app_free(a);
  return rc;
}
