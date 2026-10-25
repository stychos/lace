/* The Structure sub-view of a table (its columns) and the Definition
 * sub-view of a database view (its SQL, then its columns), a routine or a
 * trigger (the SQL alone); the rows come from core/catalog_rows.c */
#include "ui/subviews.h"
#include "util/str.h"
#include <stdlib.h>

static bool structure_build(NcApp *app, View *v, CatalogRows *t, char **message) {
  (void)app;
  if (v->schema_error) {
    *message = str_dup(v->schema_error);
    return false;
  }
  if (!v->schema) {
    *message = str_dup("Loading\xe2\x80\xa6");
    return false;
  }
  if (!v->schema->num_columns) {
    *message = str_dup("No columns");
    return false;
  }
  catalog_rows_structure(t, v->schema);
  return true;
}

const SvOps sv_structure_ops = {structure_build, NULL, NULL};

static bool definition_build(NcApp *app, View *v, CatalogRows *t, char **message) {
  if (!v->definition && !v->definition_error && !v->definition_loading) app_view_load_definition(app, v);
  if (v->definition_error) {
    *message = str_dup(v->definition_error);
    return false;
  }
  if (!v->definition) {
    *message = str_dup("Loading\xe2\x80\xa6");
    return false;
  }
  catalog_rows_definition(t, v->definition, grid_content_rect(app).cols - 2,
                          v->object == LACE_OBJ_VIEW ? v->schema : NULL);
  return true;
}

const SvOps sv_definition_ops = {definition_build, NULL, NULL};
