/* The Information sub-view: the table's facts (lace_table_info and its
 * counter, loaded when first shown), the exact row count on demand, and the
 * triggers on the table (from the connection's object list; Enter opens
 * one). A database view, routine or trigger shows what the catalog listed. */
#include "ui/subviews.h"
#include "model/workspace.h"
#include "util/str.h"
#include <stdlib.h>

static bool info_build(NcApp *app, View *v, CatalogRows *t, char **message) {
  (void)message;
  bool table_like = v->object == LACE_OBJ_TABLE || v->object == LACE_OBJ_VIEW;
  if (table_like && !v->info && !v->info_loading && !v->info_error) app_view_load_info(app, v);
  ConnTab *tab = app_view_tab(app, v);
  size_t n = 0;
  const LaceObject *objs = tab && tab->conn ? nc_connection_objects(tab->conn, &n) : NULL;
  /* the exact count: asked for here, or known from Data (an unfiltered total) */
  bool exact_known = page_cache_total_known(v->cache) && !page_cache_total_approximate(v->cache) && !v->num_filters;
  int64_t exact = v->info_count >= 0 ? v->info_count : exact_known ? (int64_t)page_cache_extent(v->cache) : -1;
  catalog_rows_info(t, v->object, v->table, v->object_table, v->object_detail, v->info, v->autoinc, v->info_error,
                    exact, v->info_count_pending, objs, n);
  return true;
}

static void info_activate(NcApp *app, View *v, int tag) {
  if (tag == CATALOG_TAG_ROWS) {
    if (!v->info_count_pending) app_view_count_exact(app, v);
    return;
  }
  if (tag < CATALOG_TAG_TRIGGER) return;
  ConnTab *tab = app_view_tab(app, v);
  if (!tab || !tab->conn) return;
  size_t n = 0;
  const LaceObject *objs = nc_connection_objects(tab->conn, &n);
  size_t i = (size_t)(tag - CATALOG_TAG_TRIGGER);
  if (i >= n || objs[i].kind != LACE_OBJ_TRIGGER) return;
  char *name = str_dup(objs[i].name), *table = objs[i].table ? str_dup(objs[i].table) : NULL;
  app_open_object(app, LACE_OBJ_TRIGGER, name, table, NULL);
  free(name);
  free(table);
}

const SvOps sv_info_ops = {info_build, info_activate, "Enter count/open"};
