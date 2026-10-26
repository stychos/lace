/* The table's actions (the shared table_menu action, m; the palette; a
 * click on the header's view segment): a pick list for the current table
 * view (rename, truncate, drop, new table/view/function) or object view
 * (rename, drop, edit in a query view), each through app_ddl_confirm. */
#include "app.h"
#include "app_ddl.h"
#include "model/workspace.h"
#include "ui/dialogs/confirm.h"
#include "ui/dialogs/pick.h"
#include "ui/dialogs/prompt.h"
#include "ui/subviews.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

enum { M_RENAME, M_TRUNCATE, M_DROP, M_NEW_TABLE, M_NEW_VIEW, M_NEW_FUNCTION, M_EDIT_QUERY, M_COUNT };

typedef struct {
  NcApp *app;
  uint64_t uid;
  int items[M_COUNT]; /* the menu's rows, in order */
  size_t n;
} MenuReq;

static const char *object_word(LaceObjectKind k) {
  switch (k) {
  case LACE_OBJ_VIEW: return "view";
  case LACE_OBJ_ROUTINE: return "routine";
  case LACE_OBJ_TRIGGER: return "trigger";
  default: return "table";
  }
}

/* ---- rename ---- */

static void renamed(NcApp *app, char *text, void *data) {
  MenuReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  char *name = text ? str_dup(text) : NULL;
  free(text);
  free(rq);
  if (!v || !name || !*name || !strcmp(name, v->table)) {
    if (v && name && !*name) app_set_status(app, false, "No change");
    free(name);
    return;
  }
  LaceDriver d = subview_driver(app, v);
  char *err = NULL;
  char *sql = v->object == LACE_OBJ_VIEW ? lace_ddl_rename_view(d, v->table, name, &err)
                                         : lace_ddl_rename_table(d, v->table, name, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
  } else {
    char *summary = str_printf("Rename the %s %s to %s.\nOpen tabs follow; query views keep their text.",
                               object_word(v->object), v->table, name);
    app_ddl_confirm(app, v, v->object == LACE_OBJ_VIEW ? "Rename view" : "Rename table", summary, sql, false,
                    DDL_AFTER_RENAME, str_dup(name), 0);
    free(summary);
  }
  free(name);
}

static void rename_object(NcApp *app, View *v) {
  MenuReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  char *title = str_printf("Rename %s %s", object_word(v->object), v->table);
  prompt_open(app, title, "New name", v->table, false, renamed, rq);
  free(title);
}

/* ---- truncate ---- */

static void truncate_table(NcApp *app, View *v) {
  char *err = NULL, *sql = lace_ddl_truncate(subview_driver(app, v), v->table, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
    return;
  }
  char *rows = app_ddl_rows_text(v);
  char *summary = str_printf("Delete every row of %s.", rows);
  app_ddl_confirm(app, v, "Truncate table", summary, sql, true, DDL_AFTER_SCHEMA, NULL, 0);
  free(summary);
  free(rows);
}

/* ---- drop ---- */

/* "order_items (fk_items_order), invoices (...)" */
static char *deps_text(const View *v) {
  char *out = str_dup("");
  for (size_t i = 0; i < v->num_deps; i++) {
    const LaceDependent *d = &v->deps[i];
    char *n = str_printf("%s%s%s%s%s%s", out, *out ? ", " : "", d->table ? d->table : "", d->name ? " (" : "",
                         d->name ? d->name : "", d->name ? ")" : "");
    free(out);
    out = n;
  }
  return out;
}

static void drop_cascade_confirmed(NcApp *app, bool yes, void *data) {
  MenuReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  free(rq);
  if (!yes || !v) return;
  char *err = NULL, *sql = lace_ddl_drop_table(LACE_DRIVER_POSTGRES, v->table, true, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
    return;
  }
  char *deps = deps_text(v);
  char *summary = str_printf("Drop table %s and the foreign keys of %s with it (CASCADE).", v->table, deps);
  app_ddl_confirm(app, v, "Drop table", summary, sql, true, DDL_AFTER_DROP, NULL, 0);
  free(summary);
  free(deps);
}

/* The dependents are known: the confirmation(s) */
static void drop_now(NcApp *app, View *v) {
  LaceDriver d = subview_driver(app, v);
  char *rows = app_ddl_rows_text(v), *deps = deps_text(v);
  const char *word = object_word(v->object);
  char *err = NULL, *sql = NULL;
  switch (v->object) {
  case LACE_OBJ_VIEW: sql = lace_ddl_drop_view(d, v->table, &err); break;
  case LACE_OBJ_ROUTINE: {
    LaceObject o = {.kind = LACE_OBJ_ROUTINE, .name = v->table, .detail = v->object_detail};
    sql = lace_ddl_drop_routine(d, &o, &err);
    break;
  }
  case LACE_OBJ_TRIGGER: sql = lace_ddl_drop_trigger(d, v->table, v->object_table, &err); break;
  default: sql = lace_ddl_drop_table(d, v->table, false, &err); break;
  }
  if (!sql) {
    app_ddl_refused(app, 0, err);
    free(rows);
    free(deps);
    return;
  }
  if (v->object == LACE_OBJ_TABLE && d == LACE_DRIVER_POSTGRES && v->num_deps) {
    /* PostgreSQL refuses while others reference it: CASCADE, after a second confirmation */
    free(sql);
    char *msg = str_printf("Drop table %s?\nReferenced by %s: PostgreSQL drops the table only with CASCADE, "
                           "which also drops those foreign keys. The next confirmation asks for it.", rows, deps);
    MenuReq *rq = safe_calloc(1, sizeof *rq);
    rq->app = app;
    rq->uid = v->uid;
    confirm_open(app, "Drop table", msg, "Continue", true, drop_cascade_confirmed, rq);
    free(msg);
  } else {
    char *summary = v->object == LACE_OBJ_TABLE
                        ? (*deps ? str_printf("Drop table %s.\nReferenced by %s: the server may refuse, or drop their rows' keys.", rows, deps)
                                 : str_printf("Drop table %s.", rows))
                        : str_printf("Drop the %s %s.", word, v->table);
    char *title = str_printf("Drop %s", word);
    app_ddl_confirm(app, v, title, summary, sql, true, DDL_AFTER_DROP, NULL, 0);
    free(title);
    free(summary);
  }
  free(rows);
  free(deps);
}

void app_table_drop_continue(NcApp *app, View *v) { drop_now(app, v); }

static void drop_object(NcApp *app, View *v) {
  if (v->object == LACE_OBJ_TABLE && !v->deps_loaded && !v->deps_error) {
    v->pending_action = 1; /* app_catalog.c continues once the dependents are in */
    app_view_load_deps(app, v);
    app_set_status(app, false, "Checking what references %s\xe2\x80\xa6", v->table);
    return;
  }
  drop_now(app, v);
}

/* ---- new table, view, function ---- */

static void new_table_named(NcApp *app, char *text, void *data) {
  MenuReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  free(rq);
  char *name = text ? str_dup(text) : NULL;
  free(text);
  if (!v || !name || !*name) {
    free(name);
    return;
  }
  char *err = NULL, *sql = lace_ddl_create_table_with_id(subview_driver(app, v), name, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
  } else {
    char *summary = str_printf("Create the table %s with one column, id, then open it on Structure.", name);
    app_ddl_confirm(app, v, "New table", summary, sql, false, DDL_AFTER_CREATE_TABLE, str_dup(name), 0);
    free(summary);
  }
  free(name);
}

static void new_object_named(NcApp *app, char *text, void *data) {
  MenuReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  bool function = rq->items[0] == M_NEW_FUNCTION; /* what was asked, kept in the request */
  free(rq);
  char *name = text ? str_dup(text) : NULL;
  free(text);
  if (!v || !name || !*name) {
    free(name);
    return;
  }
  char *err = NULL;
  LaceDriver d = subview_driver(app, v);
  char *tpl = function ? lace_ddl_routine_template(d, name, &err) : lace_ddl_view_template(d, name, &err);
  if (!tpl) app_ddl_refused(app, 0, err);
  else if (app_ddl_open_query(app, v, tpl))
    app_set_status(app, false, "Edit the statement and run it (Ctrl+R); the sidebar reloads then");
  free(tpl);
  free(name);
}

static void new_object(NcApp *app, View *v, int which) {
  MenuReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  rq->items[0] = which;
  if (which == M_NEW_TABLE) prompt_open(app, "New table", "Name", NULL, false, new_table_named, rq);
  else prompt_open(app, which == M_NEW_VIEW ? "New view" : "New function", "Name", NULL, false, new_object_named, rq);
}

/* ---- edit in a query view ---- */

static void edit_in_query(NcApp *app, View *v) {
  if (!v->definition) {
    app_view_load_definition(app, v);
    app_set_status(app, false, "The definition is still loading: try again in a moment");
    return;
  }
  app_ddl_open_query(app, v, v->definition);
}

/* ==========================================================================
 * The menu
 * ========================================================================== */

static const char *item_label(int item, LaceObjectKind kind) {
  switch (item) {
  case M_RENAME: return kind == LACE_OBJ_VIEW ? "Rename view\xe2\x80\xa6" : "Rename table\xe2\x80\xa6";
  case M_TRUNCATE: return "Truncate table\xe2\x80\xa6";
  case M_DROP:
    return kind == LACE_OBJ_VIEW ? "Drop view\xe2\x80\xa6" : kind == LACE_OBJ_ROUTINE ? "Drop routine\xe2\x80\xa6"
           : kind == LACE_OBJ_TRIGGER ? "Drop trigger\xe2\x80\xa6" : "Drop table\xe2\x80\xa6";
  case M_NEW_TABLE: return "New table\xe2\x80\xa6";
  case M_NEW_VIEW: return "New view\xe2\x80\xa6";
  case M_NEW_FUNCTION: return "New function\xe2\x80\xa6";
  case M_EDIT_QUERY: return "Edit in a query view";
  default: return "";
  }
}

static void picked(NcApp *app, size_t index, void *data) {
  MenuReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  if (!v || index >= rq->n) {
    free(rq);
    return;
  }
  int item = rq->items[index];
  free(rq);
  switch (item) {
  case M_RENAME: rename_object(app, v); break;
  case M_TRUNCATE: truncate_table(app, v); break;
  case M_DROP: drop_object(app, v); break;
  case M_NEW_TABLE: case M_NEW_VIEW: case M_NEW_FUNCTION: new_object(app, v, item); break;
  case M_EDIT_QUERY: edit_in_query(app, v); break;
  default: break;
  }
}

void app_table_menu(NcApp *app) {
  View *v = app_current_view(app);
  if (!v || v->kind != VIEW_TABLE) {
    app_set_status(app, false, "The table actions need a table tab");
    return;
  }
  LaceDriver d = subview_driver(app, v);
  MenuReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  switch (v->object) {
  case LACE_OBJ_TABLE:
    rq->items[rq->n++] = M_RENAME;
    rq->items[rq->n++] = M_TRUNCATE;
    rq->items[rq->n++] = M_DROP;
    break;
  case LACE_OBJ_VIEW:
    rq->items[rq->n++] = M_RENAME;
    rq->items[rq->n++] = M_DROP;
    rq->items[rq->n++] = M_EDIT_QUERY;
    break;
  default:
    rq->items[rq->n++] = M_DROP;
    rq->items[rq->n++] = M_EDIT_QUERY;
    break;
  }
  rq->items[rq->n++] = M_NEW_TABLE;
  rq->items[rq->n++] = M_NEW_VIEW;
  if (d != LACE_DRIVER_SQLITE) rq->items[rq->n++] = M_NEW_FUNCTION;
  char **labels = safe_calloc(rq->n, sizeof *labels);
  for (size_t i = 0; i < rq->n; i++) labels[i] = str_dup(item_label(rq->items[i], v->object));
  char *title = str_printf("%s %s", catalog_object_word(v->object), v->table ? v->table : "");
  pick_open(app, title, labels, rq->n, picked, rq);
  free(title);
  for (size_t i = 0; i < rq->n; i++) free(labels[i]);
  free(labels);
}
