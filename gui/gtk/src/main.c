/*
 * Lace GTK frontend
 * Entry point
 *
 *   lace-gtk [-s|--no-session] [-n|--new-window] [URL or SQLite file ...]
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "app.h"
#include <locale.h>
#include <stdio.h>
#include <string.h>

static void usage(const char *prog) {
  printf("Usage: %s [options] [connection ...]\n\n"
         "  connection       sqlite:///path.db, a SQLite file path,\n"
         "                   postgres://user:pass@host/db, mysql://..., mariadb://...,\n"
         "                   or driver+ssh://sshuser@sshhost/user:pass@dbhost/db\n"
         "  -s, --no-session Do not restore the previous windows\n"
         "  -n, --new-window Open the connections in a new window\n"
         "  -h, --help       This help\n"
         "  -v, --version    Print the version\n",
         prog);
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "");
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    }
    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
      printf("lace-gtk 0.1.0\n");
      return 0;
    }
  }
  LaceApp *app = lace_app_new();
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
