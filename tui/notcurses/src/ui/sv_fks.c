/* The Foreign keys sub-view: the table's foreign keys from the schema, then
 * "Referenced by", the tables whose keys point here (lace_table_dependents,
 * loaded when first shown; Enter on one opens that table's Foreign keys
 * sub-view). The editors: + add, Enter/e edit (drop and add again), x drop,
 * R rename (PostgreSQL renames the constraint, the others drop and add);
 * every change goes through app_ddl_confirm. SQLite can do none of it and
 * says why. */
#include "ui/subviews.h"
#include "app_ddl.h"
#include "model/workspace.h"
#include "ui/dialogs/prompt.h"
#include "ui/form.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

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

enum { F_NAME = 1, F_COLS, F_REF_TABLE, F_REF_COLS, F_ON_DELETE, F_ON_UPDATE, F_DEFERRABLE, F_INIT_DEFERRED };
static const char *const ACTIONS[] = {"NO ACTION", "RESTRICT", "CASCADE", "SET NULL", "SET DEFAULT"};
#define SQLITE_WHY "SQLite cannot add or drop a foreign key on an existing table; create a new table and copy the rows"

typedef struct {
  NcApp *app;
  uint64_t uid;
  char *fk; /* the key edited or renamed (its name); NULL: a new one */
} FkReq;

static void fkreq_free(void *p) {
  FkReq *r = p;
  free(r->fk);
  free(r);
}

static const char *or_null(const char *s) { return s && *s ? s : NULL; }

static const LaceForeignKey *fk_named(const View *v, const char *name) {
  for (size_t i = 0; name && v->schema && i < v->schema->num_foreign_keys; i++)
    if (v->schema->foreign_keys[i].name && !strcmp(v->schema->foreign_keys[i].name, name))
      return &v->schema->foreign_keys[i];
  return NULL;
}

static const LaceForeignKey *tag_fk(const View *v, int tag) {
  if (tag < CATALOG_TAG_FK || tag >= CATALOG_TAG_DEP || !v->schema) return NULL;
  size_t i = (size_t)(tag - CATALOG_TAG_FK);
  return i < v->schema->num_foreign_keys ? &v->schema->foreign_keys[i] : NULL;
}

/* MySQL needs an index starting with the key's columns; the one it has, or NULL */
static bool indexed(const View *v, char *const *cols, size_t n) {
  for (size_t i = 0; v->schema && i < v->schema->num_indexes; i++) {
    const LaceIndex *ix = &v->schema->indexes[i];
    if (ix->kind == LACE_INDEX_CHECK || ix->kind == LACE_INDEX_EXCLUSION || ix->num_columns < n) continue;
    bool same = true;
    for (size_t k = 0; k < n && same; k++)
      same = ix->columns[k] && !strcmp(ix->columns[k], cols[k]);
    if (same) return true;
  }
  return false;
}

/* The ADD statement (and MySQL's index before it when the columns have
 * none); *note says so */
static char *add_sql(NcApp *app, View *v, const LaceDdlForeignKey *fk, char **note, char **err) {
  LaceDriver d = subview_driver(app, v);
  char *add = lace_ddl_add_foreign_key(d, v->table, fk, err);
  if (!add) return NULL;
  *note = NULL;
  if (subview_mysql(d) && !indexed(v, (char *const *)fk->columns, fk->num_columns)) {
    char *ixname = str_printf("fk_%s_%s", v->table, fk->columns[0]);
    char *ix = lace_ddl_create_index(d, v->table, ixname, false, fk->columns, fk->num_columns, NULL, NULL, err);
    if (!ix) {
      free(add);
      free(ixname);
      return NULL;
    }
    char *joined = catalog_join((char *const *)fk->columns, fk->num_columns);
    *note = str_printf("\nAnd an index %s on (%s), which MySQL requires.", ixname, joined);
    char *both = str_printf("%s;\n%s", ix, add);
    free(ix);
    free(add);
    free(ixname);
    free(joined);
    return both;
  }
  return add;
}

static void fk_submit(NcApp *app, Form *f, void *data) {
  FkReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  if (!v || !v->schema) {
    form_set_message(f, "The table is gone", true);
    return;
  }
  char **cols, **ref_cols;
  size_t n = form_names(f, F_COLS, &cols), rn = form_names(f, F_REF_COLS, &ref_cols);
  const char *ref_table = form_text(f, F_REF_TABLE);
  if (!n || !*ref_table || rn != n) {
    form_set_message(f, !n ? "At least one column is needed"
                            : !*ref_table ? "The referenced table is needed"
                                          : "As many referenced columns as columns are needed", true);
    lace_tables_free(cols, n);
    lace_tables_free(ref_cols, rn);
    return;
  }
  char *name = str_dup(form_text(f, F_NAME)), *rt = str_dup(ref_table);
  char *od = str_dup(form_text(f, F_ON_DELETE)), *ou = str_dup(form_text(f, F_ON_UPDATE));
  LaceDdlForeignKey fk = {.name = or_null(name), .columns = (const char *const *)cols, .num_columns = n,
                          .ref_table = rt, .ref_columns = (const char *const *)ref_cols, .num_ref_columns = rn,
                          .on_delete = or_null(od), .on_update = or_null(ou),
                          .deferrable = form_on(f, F_DEFERRABLE), .initially_deferred = form_on(f, F_INIT_DEFERRED)};
  LaceDriver d = subview_driver(app, v);
  char *err = NULL, *note = NULL, *sql = add_sql(app, v, &fk, &note, &err);
  char *drop = NULL;
  if (sql && rq->fk) { /* a change: the old key goes first */
    drop = lace_ddl_drop_foreign_key(d, v->table, rq->fk, &err);
    if (drop) {
      char *both = str_printf("%s;\n%s", drop, sql);
      free(sql);
      sql = both;
    } else {
      free(sql);
      sql = NULL;
    }
  }
  if (!sql) {
    app_ddl_refused(app, f->uid, err);
  } else {
    char *joined = catalog_join(cols, n), *rjoined = catalog_join(ref_cols, rn);
    char *summary = rq->fk ? str_printf("Change the foreign key %s of %s: (%s) references %s (%s), by dropping it and adding it again.%s",
                                        rq->fk, v->table, joined, rt, rjoined, note ? note : "")
                           : str_printf("Add a foreign key to %s: (%s) references %s (%s).\nEvery row must satisfy it already.%s",
                                        v->table, joined, rt, rjoined, note ? note : "");
    app_ddl_confirm(app, v, rq->fk ? "Change foreign key" : "Add foreign key", summary, sql, rq->fk != NULL,
                    DDL_AFTER_SCHEMA, NULL, f->uid);
    free(summary);
    free(joined);
    free(rjoined);
  }
  free(drop);
  free(note);
  free(name);
  free(rt);
  free(od);
  free(ou);
  lace_tables_free(cols, n);
  lace_tables_free(ref_cols, rn);
}

/* cur NULL: a new key */
static void open_fk_editor(NcApp *app, View *v, const LaceForeignKey *cur) {
  LaceDriver d = subview_driver(app, v);
  if (d == LACE_DRIVER_SQLITE) {
    app_set_status(app, true, SQLITE_WHY);
    return;
  }
  ConnTab *t = app_view_tab(app, v);
  FormFields ff = {0};
  form_fields_text(&ff, F_NAME, "Name", cur ? cur->name : NULL, "optional");
  char *cols = cur ? catalog_join(cur->columns, cur->num_columns) : NULL;
  char *rcols = cur ? catalog_join(cur->ref_columns, cur->num_ref_columns) : NULL;
  form_fields_list(&ff, F_COLS, "Columns", cols);
  const char **names;
  size_t n = subview_column_names(v, &names);
  form_fields_options(&ff, F_COLS, names, n);
  free(names);
  form_fields_pick(&ff, F_REF_TABLE, "References", cur ? cur->ref_table : NULL, true);
  size_t nt = 0;
  char **tables = t && t->conn ? nc_connection_tables(t->conn, &nt) : NULL;
  if (nt) form_fields_options(&ff, F_REF_TABLE, (const char *const *)tables, nt);
  form_fields_text(&ff, F_REF_COLS, "Referenced columns", rcols, "comma-separated, in order");
  form_fields_pick(&ff, F_ON_DELETE, "ON DELETE", cur ? cur->on_delete : NULL, true);
  form_fields_options(&ff, F_ON_DELETE, ACTIONS, 5);
  form_fields_pick(&ff, F_ON_UPDATE, "ON UPDATE", cur ? cur->on_update : NULL, true);
  form_fields_options(&ff, F_ON_UPDATE, ACTIONS, 5);
  form_fields_toggle(&ff, F_DEFERRABLE, "Deferrable", cur && cur->deferrable);
  form_fields_toggle(&ff, F_INIT_DEFERRED, "Initially deferred", cur && cur->initially_deferred);
  if (d != LACE_DRIVER_POSTGRES) {
    form_fields_disable(&ff, F_DEFERRABLE, "Only PostgreSQL has deferrable constraints");
    form_fields_disable(&ff, F_INIT_DEFERRED, "Only PostgreSQL has deferrable constraints");
  }
  free(cols);
  free(rcols);
  FkReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  rq->fk = cur && cur->name ? str_dup(cur->name) : NULL;
  char *title = cur ? str_printf("Foreign key %s", cur->name ? cur->name : "") : str_printf("New foreign key on %s", v->table);
  form_open(app, title, &ff, fk_submit, rq, fkreq_free);
  free(title);
}

static void drop_fk(NcApp *app, View *v, const LaceForeignKey *fk) {
  if (!fk->name) {
    app_set_status(app, true, "The key has no name to drop it by");
    return;
  }
  char *err = NULL, *sql = lace_ddl_drop_foreign_key(subview_driver(app, v), v->table, fk->name, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
    return;
  }
  char *summary = str_printf("Drop the foreign key %s of %s (references %s).", fk->name, v->table,
                             fk->ref_table ? fk->ref_table : "");
  app_ddl_confirm(app, v, "Drop foreign key", summary, sql, true, DDL_AFTER_SCHEMA, NULL, 0);
  free(summary);
}

static void renamed(NcApp *app, char *text, void *data) {
  FkReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  const LaceForeignKey *fk = v ? fk_named(v, rq->fk) : NULL;
  char *name = text ? str_dup(text) : NULL;
  free(text);
  if (!v || !fk || !name || !*name || !strcmp(name, rq->fk)) {
    if (v && name && !*name) app_set_status(app, false, "No change");
    free(name);
    fkreq_free(rq);
    return;
  }
  LaceDriver d = subview_driver(app, v);
  char *err = NULL, *sql = NULL, *note = NULL;
  if (d == LACE_DRIVER_POSTGRES) {
    sql = lace_ddl_rename_constraint(d, v->table, fk->name, name, &err);
  } else {
    LaceDdlForeignKey nk = {.name = name, .columns = (const char *const *)fk->columns, .num_columns = fk->num_columns,
                            .ref_table = fk->ref_table, .ref_columns = (const char *const *)fk->ref_columns,
                            .num_ref_columns = fk->num_ref_columns, .on_delete = fk->on_delete, .on_update = fk->on_update};
    char *add = add_sql(app, v, &nk, &note, &err);
    char *drop = add ? lace_ddl_drop_foreign_key(d, v->table, fk->name, &err) : NULL;
    if (add && drop) sql = str_printf("%s;\n%s", drop, add);
    free(add);
    free(drop);
  }
  if (!sql) {
    app_ddl_refused(app, 0, err);
  } else {
    char *summary = str_printf("Rename the foreign key %s of %s to %s%s", fk->name, v->table, name,
                               d == LACE_DRIVER_POSTGRES ? "" : " (dropped and added again)");
    app_ddl_confirm(app, v, "Rename foreign key", summary, sql, false, DDL_AFTER_SCHEMA, NULL, 0);
    free(summary);
  }
  free(note);
  free(name);
  fkreq_free(rq);
}

static void rename_fk(NcApp *app, View *v, const LaceForeignKey *fk) {
  if (subview_driver(app, v) == LACE_DRIVER_SQLITE) {
    app_set_status(app, true, SQLITE_WHY);
    return;
  }
  if (!fk->name) {
    app_set_status(app, true, "The key has no name");
    return;
  }
  FkReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  rq->fk = str_dup(fk->name);
  char *title = str_printf("Rename %s", fk->name);
  prompt_open(app, title, "New name", fk->name, false, renamed, rq);
  free(title);
}

static bool fks_action(NcApp *app, View *v, HotkeyAction a, int tag) {
  if (!v->schema) {
    app_set_status(app, false, "The structure is still loading");
    return true;
  }
  const LaceForeignKey *fk = tag_fk(v, tag);
  switch (a) {
  case HOTKEY_STRUCT_ADD: open_fk_editor(app, v, NULL); return true;
  case HOTKEY_STRUCT_EDIT:
    if (fk) open_fk_editor(app, v, fk);
    else app_set_status(app, false, "Pick a foreign key first");
    return true;
  case HOTKEY_STRUCT_DROP:
    if (subview_driver(app, v) == LACE_DRIVER_SQLITE) app_set_status(app, true, SQLITE_WHY);
    else if (fk) drop_fk(app, v, fk);
    else app_set_status(app, false, "Pick a foreign key first");
    return true;
  case HOTKEY_STRUCT_RENAME:
    if (fk) rename_fk(app, v, fk);
    else app_set_status(app, false, "Pick a foreign key first");
    return true;
  case HOTKEY_STRUCT_PRIMARY_KEY: case HOTKEY_STRUCT_MOVE: case HOTKEY_STRUCT_CHECK: return true;
  default: return false;
  }
}

static void fks_activate(NcApp *app, View *v, int tag) {
  const LaceForeignKey *fk = tag_fk(v, tag);
  if (fk) {
    open_fk_editor(app, v, fk);
    return;
  }
  if (tag < CATALOG_TAG_DEP || (size_t)(tag - CATALOG_TAG_DEP) >= v->num_deps) return;
  const char *table = v->deps[tag - CATALOG_TAG_DEP].table;
  if (!table) return;
  char *name = str_dup(table); /* opening may reload the dependents */
  View *nv = app_open_table(app, name);
  if (nv) subview_set(app, nv, SV_FOREIGN_KEYS);
  free(name);
}

const SvOps sv_fks_ops = {fks_build, fks_activate, "Enter edit/open", fks_action, "+ add  x drop  R rename"};
