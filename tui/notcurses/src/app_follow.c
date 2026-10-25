/* Following foreign keys from the grid: the referenced table filtered by the
 * cursor row's key (follow_fk), and a table whose key points at the cursor
 * row (referenced_by, a pick list of the dependents). The target opens in a
 * new pinned view of the connection tab, or an open view of that table is
 * reused with its filters replaced. Works on a table view's Data and on
 * query results that are a single table's rows (the editing rule). */
#include "app.h"
#include "model/workspace.h"
#include "ui/dialogs/pick.h"
#include "ui/grid.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

/* The foreign keys whose first column is name */
static size_t fks_for_column(const View *v, const char *name, const LaceForeignKey **out, size_t max) {
  size_t n = 0;
  for (size_t i = 0; v->schema && name && i < v->schema->num_foreign_keys; i++) {
    const LaceForeignKey *fk = &v->schema->foreign_keys[i];
    if (fk->num_columns && fk->columns[0] && !strcmp(fk->columns[0], name) && n < max) out[n++] = fk;
  }
  return n;
}

bool view_column_is_fk(const View *v, const char *name) { return v && view_fk_first_column(v, name); }

/* The result column named name, or SIZE_MAX */
static size_t column_index(const View *v, const char *name) {
  size_t n = page_cache_num_columns(v->cache);
  for (size_t i = 0; i < n; i++) {
    const LaceColumn *c = page_cache_column(v->cache, i);
    if (c && c->name && !strcmp(c->name, name)) return i;
  }
  return SIZE_MAX;
}

/* The cursor row's values of the named columns as edit text (allocated,
 * freed by the caller); NULL with a status message when one is missing,
 * NULL or not loaded */
static char **row_values(NcApp *app, View *v, char *const *columns, size_t n) {
  char **vals = safe_calloc(n + 1, sizeof *vals);
  for (size_t k = 0; k < n; k++) {
    size_t ci = column_index(v, columns[k]);
    const LaceValue *cell = ci == SIZE_MAX ? NULL : page_cache_cell(v->cache, v->cursor_row, ci);
    const char *why = ci == SIZE_MAX ? "is not in the results" : !cell ? "is not loaded yet"
                      : cell->is_null ? "is NULL: nothing to follow" : cell->omitted ? "is too large to load" : NULL;
    if (why) {
      app_set_status(app, false, "%s %s", columns[k], why);
      for (size_t j = 0; j < k; j++) str_secure_free(vals[j]);
      free(vals);
      return NULL;
    }
    vals[k] = lace_value_to_edit_text(cell);
    if (!vals[k]) vals[k] = str_dup("");
  }
  return vals;
}

static void free_values(char **vals, size_t n) {
  for (size_t k = 0; vals && k < n; k++) str_secure_free(vals[k]);
  free(vals);
}

/* Open table filtered by columns = values in the current connection tab: an
 * open view of it gets the filters, else a new pinned view */
static void open_filtered(NcApp *app, const char *table, char *const *columns, char *const *values, size_t n) {
  ConnTab *t = app_current_tab(app);
  if (!t || !t->conn) return;
  char *name = str_dup(table);
  size_t i = conntab_find_table_view(t, name);
  View *v;
  if (i != SIZE_MAX) {
    v = t->views[i];
    t->current = i;
    t->sidebar_focused = false;
    v->preview = false;
    view_checkpoint(v);
    view_clear_filters(v);
    for (size_t k = 0; k < n; k++) view_set_filter(v, columns[k], LACE_FILTER_EQ, values[k], NULL);
    if (v->subview != SV_DATA && view_has_subview(v, SV_DATA)) v->subview = SV_DATA;
    grid_state_changed(app, v);
  } else {
    v = app_open_table(app, name); /* the schema is on its way: page 0 is read with the filters */
    if (v)
      for (size_t k = 0; k < n; k++) view_set_filter(v, columns[k], LACE_FILTER_EQ, values[k], NULL);
  }
  if (v) {
    char *cond = str_dup("");
    for (size_t k = 0; k < n; k++) {
      char *next = str_printf("%s%s%s = %s", cond, k ? ", " : "", columns[k], values[k]);
      free(cond);
      cond = next;
    }
    app_set_status(app, false, "%s where %s", name, cond);
    free(cond);
  }
  free(name);
}

static void follow(NcApp *app, View *v, const LaceForeignKey *fk) {
  if (!fk->ref_table || fk->num_ref_columns != fk->num_columns) {
    app_set_status(app, true, "The foreign key %s is incomplete", fk->name ? fk->name : "");
    return;
  }
  char **vals = row_values(app, v, fk->columns, fk->num_columns);
  if (!vals) return;
  /* copies: opening a view may reload the schema the key lives in */
  size_t n = fk->num_columns;
  char **cols = safe_calloc(n + 1, sizeof *cols);
  for (size_t k = 0; k < n; k++) cols[k] = str_dup(fk->ref_columns[k]);
  char *table = str_dup(fk->ref_table);
  open_filtered(app, table, cols, vals, n);
  free(table);
  for (size_t k = 0; k < n; k++) free(cols[k]);
  free(cols);
  free_values(vals, n);
}

typedef struct {
  uint64_t uid;
  size_t which; /* the index among the keys of the column (follow) or the dependents (referenced by) */
} FollowPick;

static bool follow_ready(NcApp *app, View *v) {
  if (!v || (v->kind == VIEW_QUERY && !v->source_table)) {
    app_set_status(app, false, "Foreign keys are followed from a table's rows");
    return false;
  }
  if (!v->schema) {
    app_set_status(app, false, "The table is still loading");
    return false;
  }
  if (page_cache_extent(v->cache) == 0) {
    app_set_status(app, false, "No row to follow");
    return false;
  }
  return true;
}

static const char *cursor_column(const View *v) {
  const LaceColumn *c = page_cache_column(v->cache, v->cursor_col);
  return c && c->name ? c->name : NULL;
}

static void picked_fk(NcApp *app, size_t index, void *data) {
  FollowPick *p = data;
  View *v = app_find_view(app, p->uid);
  if (index != SIZE_MAX && v && follow_ready(app, v)) {
    const LaceForeignKey *fks[LACE_MAX_PK_COLUMNS + 8];
    size_t n = fks_for_column(v, cursor_column(v), fks, sizeof fks / sizeof *fks);
    if (index < n) follow(app, v, fks[index]);
  }
  free(p);
}

void app_follow_fk(NcApp *app, View *v) {
  if (!follow_ready(app, v)) return;
  const char *name = cursor_column(v);
  const LaceForeignKey *fks[LACE_MAX_PK_COLUMNS + 8];
  size_t n = fks_for_column(v, name, fks, sizeof fks / sizeof *fks);
  if (!n) {
    app_set_status(app, false, "%s is not a foreign key column", name ? name : "This column");
    return;
  }
  if (n == 1) {
    follow(app, v, fks[0]);
    return;
  }
  char **items = safe_calloc(n, sizeof *items);
  for (size_t i = 0; i < n; i++) {
    char *cols = str_dup("");
    for (size_t k = 0; k < fks[i]->num_ref_columns; k++) {
      char *next = str_printf("%s%s%s", cols, k ? ", " : "", fks[i]->ref_columns[k]);
      free(cols);
      cols = next;
    }
    items[i] = str_printf("%s (%s)%s%s", fks[i]->ref_table ? fks[i]->ref_table : "?", cols,
                          fks[i]->name ? "  " : "", fks[i]->name ? fks[i]->name : "");
    free(cols);
  }
  FollowPick *p = safe_calloc(1, sizeof *p);
  p->uid = v->uid;
  pick_open(app, "Follow which key?", items, n, picked_fk, p);
  for (size_t i = 0; i < n; i++) free(items[i]);
  free(items);
}

static void picked_dep(NcApp *app, size_t index, void *data) {
  FollowPick *p = data;
  View *v = app_find_view(app, p->uid);
  if (index != SIZE_MAX && v && follow_ready(app, v) && index < v->num_deps) {
    const LaceDependent *d = &v->deps[index];
    size_t n = d->num_columns;
    char **vals = row_values(app, v, d->ref_columns, n);
    if (vals) {
      char **cols = safe_calloc(n + 1, sizeof *cols);
      for (size_t k = 0; k < n; k++) cols[k] = str_dup(d->columns[k]);
      char *table = str_dup(d->table ? d->table : "");
      open_filtered(app, table, cols, vals, n);
      free(table);
      for (size_t k = 0; k < n; k++) free(cols[k]);
      free(cols);
      free_values(vals, n);
    }
  }
  free(p);
}

void app_referenced_by(NcApp *app, View *v) {
  if (!follow_ready(app, v)) return;
  if (!v->deps_loaded) {
    if (v->deps_error) {
      app_set_status(app, true, "%s", v->deps_error);
      free(v->deps_error);
      v->deps_error = NULL;
      return;
    }
    v->referenced_by_pending = true; /* on_deps opens the list */
    app_view_load_deps(app, v);
    app_set_status(app, false, "Looking up the referencing tables\xe2\x80\xa6");
    return;
  }
  if (!v->num_deps) {
    app_set_status(app, false, "No table references %s", v->kind == VIEW_QUERY ? v->source_table : v->table);
    return;
  }
  char **items = safe_calloc(v->num_deps, sizeof *items);
  for (size_t i = 0; i < v->num_deps; i++) {
    const LaceDependent *d = &v->deps[i];
    char *cols = str_dup("");
    for (size_t k = 0; k < d->num_columns; k++) {
      char *next = str_printf("%s%s%s", cols, k ? ", " : "", d->columns[k]);
      free(cols);
      cols = next;
    }
    items[i] = str_printf("%s (%s)%s%s", d->table ? d->table : "?", cols, d->on_delete ? "  ON DELETE " : "",
                          d->on_delete ? d->on_delete : "");
    free(cols);
  }
  FollowPick *p = safe_calloc(1, sizeof *p);
  p->uid = v->uid;
  pick_open(app, "Referenced by", items, v->num_deps, picked_dep, p);
  for (size_t i = 0; i < v->num_deps; i++) free(items[i]);
  free(items);
}
