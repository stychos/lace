/* The Foreign keys sub-view: the table's foreign keys from the schema, then
 * "Referenced by", the tables whose keys point here (lace_table_dependents,
 * loaded when first shown); Enter on one of those opens that table's
 * Foreign keys sub-view */
#include "ui/subviews.h"
#include "util/str.h"
#include <stdlib.h>

static bool fks_build(NcApp *app, View *v, CatalogRows *t, char **message) {
  if (v->schema_error) {
    *message = str_dup(v->schema_error);
    return false;
  }
  if (!v->schema) {
    *message = str_dup("Loading\xe2\x80\xa6");
    return false;
  }
  if (!v->deps_loaded && !v->deps_loading && !v->deps_error) app_view_load_deps(app, v);
  catalog_rows_fks(t, v->schema, v->table, v->deps, v->num_deps, v->deps_loaded, v->deps_error);
  return true;
}

static void fks_activate(NcApp *app, View *v, int tag) {
  if (tag < CATALOG_TAG_DEP || (size_t)(tag - CATALOG_TAG_DEP) >= v->num_deps) return;
  const char *table = v->deps[tag - CATALOG_TAG_DEP].table;
  if (!table) return;
  char *name = str_dup(table); /* opening may reload the dependents */
  View *nv = app_open_table(app, name);
  if (nv) subview_set(app, nv, SV_FOREIGN_KEYS);
  free(name);
}

const SvOps sv_fks_ops = {fks_build, fks_activate, "Enter open"};
