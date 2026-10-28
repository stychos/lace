/* The Structure sub-view of a table (its columns) and the Definition
 * sub-view of a database view (its SQL, then its columns), a routine or a
 * trigger (the SQL alone); the rows come from core/catalog_rows.c. The
 * editors: a column form (add, Enter/e edit), x drop, R rename, P the
 * primary key editor, Space move (MySQL); every change goes through
 * app_ddl_confirm. */
#include "core/chrome.h"
#include "ui/subviews.h"
#include "app_ddl.h"
#include "model/workspace.h"
#include "ui/dialogs/pick.h"
#include "ui/dialogs/prompt.h"
#include "ui/form.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

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

/* ==========================================================================
 * The column editor
 * ========================================================================== */

enum { F_NAME = 1, F_TYPE, F_NULL, F_DEFAULT, F_COLLATION, F_COMMENT, F_AUTOINC, F_AFTER, F_COLS };
#define SQLITE_WHY "SQLite cannot alter this; create a new table and copy the rows"

typedef struct {
  NcApp *app;
  uint64_t uid;
  char *column; /* the column edited; NULL: a new one */
} ColReq;

static void colreq_free(void *p) {
  ColReq *r = p;
  free(r->column);
  free(r);
}

static const char *or_null(const char *s) { return s && *s ? s : NULL; }

/* The position the form's After field names: NULL (the end / in place), ""
 * (first) or a column */
static const char *after_of(const Form *f) {
  const char *a = form_text(f, F_AFTER);
  if (!*a) return NULL;
  if (!strcmp(a, "(first)")) return "";
  return a;
}

static void col_submit(NcApp *app, Form *f, void *data) {
  ColReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  if (!v || !v->schema) {
    form_set_message(f, "The table is gone", true);
    return;
  }
  LaceDriver d = subview_driver(app, v);
  bool mysql = subview_mysql(d);
  const char *name = form_text(f, F_NAME);
  if (!*name) {
    form_set_message(f, "A name is needed", true);
    return;
  }
  char *name_s = str_dup(name);
  LaceDdlColumn w = {.name = name_s,
                     .type = or_null(form_text(f, F_TYPE)),
                     .nullable = form_on(f, F_NULL),
                     .auto_increment = form_on(f, F_AUTOINC)};
  char *type_s = w.type ? str_dup(w.type) : NULL;
  char *dflt = str_dup(form_text(f, F_DEFAULT)), *coll = str_dup(form_text(f, F_COLLATION));
  char *comment = str_dup(form_text(f, F_COMMENT));
  w.type = type_s;
  w.default_sql = or_null(dflt);
  w.collation = or_null(coll);
  w.comment = d == LACE_DRIVER_SQLITE ? NULL : comment; /* "" clears */
  const char *after = mysql ? after_of(f) : NULL;
  char *after_s = after ? str_dup(after) : NULL;
  w.after = after_s;
  char *err = NULL, *sql = NULL, *summary = NULL;
  const char *title;
  if (!rq->column) { /* add */
    if (!w.comment || !*w.comment) w.comment = NULL;
    sql = lace_ddl_add_column(d, v->table, &w, &err);
    summary = str_printf("Add column %s to %s", name, v->table);
    title = "Add column";
  } else { /* alter, and rename when the name changed */
    const LaceColumn *cur = subview_column(v, rq->column);
    if (!cur) {
      form_set_message(f, "The column is gone (the structure was reloaded)", true);
      goto out;
    }
    w.name = cur->name;
    if (w.comment && !strcmp(w.comment, cur->comment ? cur->comment : "")) w.comment = NULL; /* unchanged */
    char *alter = lace_ddl_alter_column(d, v->table, cur, &w, &err);
    bool no_change = !alter && err && !strcmp(err, "No change");
    if (no_change) {
      free(err);
      err = NULL;
    }
    char *rename = NULL;
    if (strcmp(name, cur->name) != 0 && (alter || no_change)) rename = lace_ddl_rename_column(d, v->table, cur->name, name, &err);
    if (alter && rename) sql = str_printf("%s;\n%s", alter, rename); /* the alter names the old column */
    else if (alter) sql = str_dup(alter);
    else if (rename) sql = str_dup(rename);
    free(alter);
    free(rename);
    if (!sql && !err) {
      form_set_message(f, "No change", false);
      goto out;
    }
    summary = rename ? str_printf("Change column %s of %s and rename it to %s", cur->name, v->table, name)
                     : str_printf("Change column %s of %s", cur->name, v->table);
    title = "Change column";
  }
  if (!sql) {
    app_ddl_refused(app, f->uid, err);
    err = NULL;
    goto out;
  }
  app_ddl_confirm(app, v, title, summary, sql, false, DDL_AFTER_SCHEMA, NULL, f->uid);
  sql = NULL;
out:
  free(err);
  free(summary);
  free(sql);
  free(name_s);
  free(type_s);
  free(dflt);
  free(coll);
  free(comment);
  free(after_s);
}

/* cur NULL: a new column after the cursor's (MySQL) or at the end */
static void open_column_editor(NcApp *app, View *v, const LaceColumn *cur) {
  LaceDriver d = subview_driver(app, v);
  bool mysql = subview_mysql(d), sqlite = d == LACE_DRIVER_SQLITE;
  FormFields ff = {0};
  form_fields_text(&ff, F_NAME, "Name", cur ? cur->name : NULL, NULL);
  form_fields_pick(&ff, F_TYPE, "Type", cur ? cur->type_name : NULL, true);
  form_fields_toggle(&ff, F_NULL, "Nullable", cur ? cur->nullable : true);
  form_fields_text(&ff, F_DEFAULT, "Default", cur ? cur->default_val : NULL, "SQL: 'text', 0, CURRENT_TIMESTAMP");
  form_fields_pick(&ff, F_COLLATION, "Collation", cur ? cur->collation : NULL, true);
  form_fields_text(&ff, F_COMMENT, "Comment", cur ? cur->comment : NULL, NULL);
  form_fields_toggle(&ff, F_AUTOINC, "Auto-increment", cur && (cur->auto_increment || cur->identity));
  if (mysql) {
    form_fields_pick(&ff, F_AFTER, cur ? "Move after" : "After", NULL, false);
    const char **names;
    size_t n = subview_column_names(v, &names);
    const char **opts = safe_calloc(n + 1, sizeof *opts);
    size_t k = 0;
    opts[k++] = "(first)";
    for (size_t i = 0; i < n; i++)
      if (!cur || strcmp(names[i], cur->name)) opts[k++] = names[i];
    form_fields_options(&ff, F_AFTER, opts, k);
    free(opts);
    free(names);
  }
  if (sqlite) {
    form_fields_disable(&ff, F_COMMENT, "SQLite has no column comments");
    if (cur) {
      form_fields_disable(&ff, F_TYPE, SQLITE_WHY);
      form_fields_disable(&ff, F_NULL, SQLITE_WHY);
      form_fields_disable(&ff, F_DEFAULT, SQLITE_WHY);
      form_fields_disable(&ff, F_COLLATION, SQLITE_WHY);
      form_fields_disable(&ff, F_AUTOINC, SQLITE_WHY);
    } else {
      form_fields_disable(&ff, F_AUTOINC, "SQLite: only INTEGER PRIMARY KEY AUTOINCREMENT at CREATE TABLE");
    }
  } else if (cur && cur->generated) {
    form_fields_disable(&ff, F_TYPE, "A generated column: drop it and add it again");
    form_fields_disable(&ff, F_DEFAULT, "A generated column: drop it and add it again");
  }
  ColReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  rq->column = cur ? str_dup(cur->name) : NULL;
  char *title = cur ? str_printf("Column %s", cur->name) : str_printf("Add column to %s", v->table);
  Form *f = form_open(app, title, &ff, col_submit, rq, colreq_free);
  free(title);
  app_ddl_fetch_names(app, v, f->uid, F_TYPE, "types", NULL);
  app_ddl_fetch_names(app, v, f->uid, F_COLLATION, "collations", NULL);
}

/* ==========================================================================
 * Drop, rename, move, the primary key
 * ========================================================================== */

/* "index a, foreign key b": what the schema says uses the column */
static char *column_uses(const View *v, const char *name) {
  char *out = str_dup("");
  for (size_t i = 0; v->schema && i < v->schema->num_indexes; i++) {
    const LaceIndex *ix = &v->schema->indexes[i];
    for (size_t k = 0; k < ix->num_columns; k++)
      if (ix->columns[k] && !strcmp(ix->columns[k], name)) {
        char *n = str_printf("%s%s%s %s", out, *out ? ", " : "",
                             ix->kind == LACE_INDEX_PRIMARY ? "the primary key" : ix->kind == LACE_INDEX_CHECK ? "check" : "index",
                             ix->name ? ix->name : "");
        free(out);
        out = n;
        break;
      }
  }
  for (size_t i = 0; v->schema && i < v->schema->num_foreign_keys; i++) {
    const LaceForeignKey *fk = &v->schema->foreign_keys[i];
    for (size_t k = 0; k < fk->num_columns; k++)
      if (fk->columns[k] && !strcmp(fk->columns[k], name)) {
        char *n = str_printf("%s%sforeign key %s", out, *out ? ", " : "", fk->name ? fk->name : "");
        free(out);
        out = n;
        break;
      }
  }
  return out;
}

static void drop_column(NcApp *app, View *v, const LaceColumn *c) {
  LaceDriver d = subview_driver(app, v);
  char *err = NULL, *sql = lace_ddl_drop_column(d, v->table, c->name, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
    return;
  }
  char *rows = app_ddl_rows_text(v), *uses = column_uses(v, c->name);
  char *summary = *uses ? str_printf("Drop column %s from %s.\nIt is part of %s: PostgreSQL drops those with it, "
                                     "MySQL and SQLite may refuse.", c->name, rows, uses)
                        : str_printf("Drop column %s from %s.", c->name, rows);
  app_ddl_confirm(app, v, "Drop column", summary, sql, true, DDL_AFTER_SCHEMA, NULL, 0);
  free(summary);
  free(rows);
  free(uses);
}

static void renamed(NcApp *app, char *text, void *data) {
  ColReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  char *name = text ? str_dup(text) : NULL;
  free(text);
  if (!v || !name || !*name || !strcmp(name, rq->column)) {
    if (v && name && !*name) app_set_status(app, false, "No change");
    free(name);
    colreq_free(rq);
    return;
  }
  char *err = NULL, *sql = lace_ddl_rename_column(subview_driver(app, v), v->table, rq->column, name, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
  } else {
    char *summary = str_printf("Rename column %s of %s to %s", rq->column, v->table, name);
    app_ddl_confirm(app, v, "Rename column", summary, sql, false, DDL_AFTER_SCHEMA, NULL, 0);
    free(summary);
  }
  free(name);
  colreq_free(rq);
}

static void rename_column(NcApp *app, View *v, const LaceColumn *c) {
  ColReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  rq->column = str_dup(c->name);
  char *title = str_printf("Rename column %s", c->name);
  prompt_open(app, title, "New name", c->name, false, renamed, rq);
  free(title);
}

/* ---- move (MySQL: AFTER) ---- */

static void moved(NcApp *app, size_t index, void *data) {
  ColReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  const LaceColumn *cur = v ? subview_column(v, rq->column) : NULL;
  if (!v || !cur || index == SIZE_MAX) {
    colreq_free(rq);
    return;
  }
  /* index 0: first; else the k-th other column */
  const char *after = "";
  if (index > 0) {
    size_t k = 0;
    for (size_t i = 0; v->schema && i < v->schema->num_columns; i++) {
      if (!strcmp(v->schema->columns[i].name, cur->name)) continue;
      if (++k == index) after = v->schema->columns[i].name;
    }
  }
  LaceDdlColumn w = {.name = cur->name, .type = cur->type_name, .nullable = cur->nullable,
                     .default_sql = cur->default_val, .collation = cur->collation,
                     .auto_increment = cur->auto_increment || cur->identity != 0, .after = after};
  char *err = NULL, *sql = lace_ddl_alter_column(subview_driver(app, v), v->table, cur, &w, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
  } else {
    char *summary = *after ? str_printf("Move column %s of %s after %s", cur->name, v->table, after)
                           : str_printf("Move column %s of %s first", cur->name, v->table);
    app_ddl_confirm(app, v, "Move column", summary, sql, false, DDL_AFTER_SCHEMA, NULL, 0);
    free(summary);
  }
  colreq_free(rq);
}

static void move_column(NcApp *app, View *v, const LaceColumn *c) {
  if (!subview_mysql(subview_driver(app, v))) {
    app_set_status(app, false, "Only MySQL can reorder columns; the others keep the creation order");
    return;
  }
  size_t n = v->schema->num_columns;
  char **items = safe_calloc(n + 1, sizeof *items);
  size_t k = 0;
  items[k++] = str_dup("(first)");
  for (size_t i = 0; i < n; i++)
    if (strcmp(v->schema->columns[i].name, c->name)) items[k++] = str_printf("after %s", v->schema->columns[i].name);
  ColReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  rq->column = str_dup(c->name);
  char *title = str_printf("Move column %s", c->name);
  pick_open(app, title, items, k, moved, rq);
  free(title);
  for (size_t i = 0; i < k; i++) free(items[i]);
  free(items);
}

/* ---- the primary key ---- */

static void pk_submit(NcApp *app, Form *f, void *data) {
  ColReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  if (!v || !v->schema) {
    form_set_message(f, "The table is gone", true);
    return;
  }
  char **names;
  size_t n = form_names(f, F_COLS, &names);
  for (size_t i = 0; i < n; i++)
    if (!subview_column(v, names[i])) {
      char *msg = str_printf("%s is not a column of %s", names[i], v->table);
      form_set_message(f, msg, true);
      free(msg);
      lace_tables_free(names, n);
      return;
    }
  const char *existing = NULL;
  for (size_t i = 0; i < v->schema->num_indexes; i++)
    if (v->schema->indexes[i].kind == LACE_INDEX_PRIMARY) {
      const LaceIndex *ix = &v->schema->indexes[i];
      existing = ix->constraint && *ix->constraint ? ix->constraint : ix->name;
    }
  char *err = NULL;
  char *sql = lace_ddl_set_primary_key(subview_driver(app, v), v->table, (const char *const *)names, n, existing, &err);
  if (!sql) {
    app_ddl_refused(app, f->uid, err);
    lace_tables_free(names, n);
    return;
  }
  char *cols = catalog_join(names, n);
  char *summary = n ? str_printf("Set the primary key of %s to (%s)", v->table, cols)
                    : str_printf("Drop the primary key of %s", v->table);
  app_ddl_confirm(app, v, n ? "Set primary key" : "Drop primary key", summary, sql, n == 0, DDL_AFTER_SCHEMA, NULL, f->uid);
  free(summary);
  free(cols);
  lace_tables_free(names, n);
}

static void open_pk_editor(NcApp *app, View *v) {
  /* the current key in position order */
  size_t n = v->schema->num_columns;
  char *current = str_dup("");
  for (int pos = 1; pos <= (int)n; pos++)
    for (size_t i = 0; i < n; i++)
      if (v->schema->columns[i].key_position == pos) {
        char *t = str_printf("%s%s%s", current, *current ? ", " : "", v->schema->columns[i].name);
        free(current);
        current = t;
      }
  FormFields ff = {0};
  form_fields_list(&ff, F_COLS, "Key columns", current);
  const char **names;
  size_t nn = subview_column_names(v, &names);
  form_fields_options(&ff, F_COLS, names, nn);
  free(names);
  free(current);
  if (subview_driver(app, v) == LACE_DRIVER_SQLITE) form_fields_disable(&ff, F_COLS, SQLITE_WHY);
  ColReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  char *title = str_printf("Primary key of %s", v->table);
  form_open(app, title, &ff, pk_submit, rq, colreq_free);
  free(title);
}

/* ==========================================================================
 * The keys
 * ========================================================================== */

static const LaceColumn *tag_column(const View *v, int tag) {
  if (tag < CATALOG_TAG_COLUMN || !v->schema) return NULL;
  size_t i = (size_t)(tag - CATALOG_TAG_COLUMN);
  return i < v->schema->num_columns ? &v->schema->columns[i] : NULL;
}

static bool structure_action(NcApp *app, View *v, HotkeyAction a, int tag) {
  if (!v->schema) {
    app_set_status(app, false, "The structure is still loading");
    return true;
  }
  const LaceColumn *c = tag_column(v, tag);
  switch (a) {
  case HOTKEY_STRUCT_ADD: open_column_editor(app, v, NULL); return true;
  case HOTKEY_STRUCT_EDIT:
    if (c) open_column_editor(app, v, c);
    else app_set_status(app, false, "Pick a column first");
    return true;
  case HOTKEY_STRUCT_DROP:
    if (c) drop_column(app, v, c);
    else app_set_status(app, false, "Pick a column first");
    return true;
  case HOTKEY_STRUCT_RENAME:
    if (c) rename_column(app, v, c);
    else app_set_status(app, false, "Pick a column first");
    return true;
  case HOTKEY_STRUCT_MOVE:
    if (c) move_column(app, v, c);
    else app_set_status(app, false, "Pick a column first");
    return true;
  case HOTKEY_STRUCT_PRIMARY_KEY: open_pk_editor(app, v); return true;
  case HOTKEY_STRUCT_CHECK: app_set_status(app, false, "Check constraints are added from Indexes (y)"); return true;
  default: return false;
  }
}

static void structure_activate(NcApp *app, View *v, int tag) {
  const LaceColumn *c = tag_column(v, tag);
  if (c) open_column_editor(app, v, c);
}

const SvOps sv_structure_ops = {structure_build, structure_activate, CHROME_SV_STRUCTURE_ENTER, structure_action,
                                CHROME_SV_STRUCTURE_KEYS};

/* ==========================================================================
 * Definition (a database view, routine or trigger)
 * ========================================================================== */

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

/* Enter: the definition in a query view, to change and run */
static void definition_activate(NcApp *app, View *v, int tag) {
  (void)tag;
  if (!v->definition) {
    app_set_status(app, false, "The definition is still loading");
    return;
  }
  app_ddl_open_query(app, v, v->definition);
}

static bool definition_action(NcApp *app, View *v, HotkeyAction a, int tag) {
  if (a == HOTKEY_STRUCT_EDIT) {
    definition_activate(app, v, tag);
    return true;
  }
  app_set_status(app, false, "A %s is changed in a query view (Enter); its actions are under m",
                 v->object == LACE_OBJ_VIEW ? "view" : v->object == LACE_OBJ_ROUTINE ? "routine" : "trigger");
  return true;
}

const SvOps sv_definition_ops = {definition_build, definition_activate, CHROME_SV_DEFINITION_ENTER,
                                 definition_action, NULL};
