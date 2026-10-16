#include "app.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
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
