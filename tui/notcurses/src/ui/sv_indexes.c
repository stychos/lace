/* The Indexes sub-view: the table's indexes and its other constraints (the
 * primary key, unique, check and exclusion), from the schema */
#include "ui/subviews.h"
#include "util/str.h"
#include <stdlib.h>

static bool indexes_build(NcApp *app, View *v, CatalogRows *t, char **message) {
  (void)app;
  if (v->schema_error) {
    *message = str_dup(v->schema_error);
    return false;
  }
  if (!v->schema) {
    *message = str_dup("Loading\xe2\x80\xa6");
    return false;
  }
  if (!catalog_rows_indexes(t, v->schema)) {
    *message = str_dup("No indexes or constraints");
    return false;
  }
  return true;
}

const SvOps sv_indexes_ops = {indexes_build, NULL, NULL};
