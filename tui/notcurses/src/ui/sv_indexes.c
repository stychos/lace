/* The Indexes sub-view: the table's indexes and its other constraints (the
 * primary key, unique, check and exclusion), from the schema. The editors:
 * + an index (name, unique, columns in order, method, partial condition),
 * c a check constraint, x drop (the primary key is Structure's key editor's),
 * R or Enter rename (SQLite and MariaDB drop and create, the statement
 * shows it); every change goes through app_ddl_confirm. */
#include "ui/subviews.h"
#include "app_ddl.h"
#include "model/workspace.h"
#include "ui/dialogs/prompt.h"
#include "ui/form.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

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
    *message = str_dup("No indexes or constraints (+ creates one)");
    return false;
  }
  return true;
}

enum { F_NAME = 1, F_UNIQUE, F_COLS, F_METHOD, F_WHERE, F_COND };

typedef struct {
  NcApp *app;
  uint64_t uid;
  char *index; /* the index renamed */
} IxReq;

static void ixreq_free(void *p) {
  IxReq *r = p;
  free(r->index);
  free(r);
}

static const char *or_null(const char *s) { return s && *s ? s : NULL; }

/* ---- create ---- */

static void index_submit(NcApp *app, Form *f, void *data) {
  IxReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  if (!v || !v->schema) {
    form_set_message(f, "The table is gone", true);
    return;
  }
  const char *name = form_text(f, F_NAME);
  if (!*name) {
    form_set_message(f, "A name is needed", true);
    return;
  }
  char **cols;
  size_t n = form_names(f, F_COLS, &cols);
  if (!n) {
    form_set_message(f, "At least one column is needed", true);
    return;
  }
  char *name_s = str_dup(name), *method = str_dup(form_text(f, F_METHOD)), *where = str_dup(form_text(f, F_WHERE));
  bool unique = form_on(f, F_UNIQUE);
  char *err = NULL;
  char *sql = lace_ddl_create_index(subview_driver(app, v), v->table, name_s, unique, (const char *const *)cols, n,
                                    or_null(method), or_null(where), &err);
  if (!sql) {
    app_ddl_refused(app, f->uid, err);
  } else {
    char *joined = catalog_join(cols, n), *rows = app_ddl_rows_text(v);
    char *summary = str_printf("Create %sindex %s on %s (%s).\nLocks the table while it builds; Esc cancels.",
                               unique ? "unique " : "", name_s, rows, joined);
    app_ddl_confirm(app, v, "Create index", summary, sql, false, DDL_AFTER_SCHEMA, NULL, f->uid);
    free(summary);
    free(joined);
    free(rows);
  }
  free(name_s);
  free(method);
  free(where);
  lace_tables_free(cols, n);
}

static void open_index_editor(NcApp *app, View *v) {
  LaceDriver d = subview_driver(app, v);
  static const char *const pg_methods[] = {"btree", "hash", "gin", "gist", "brin", "spgist"};
  static const char *const my_methods[] = {"BTREE", "HASH", "FULLTEXT", "SPATIAL"};
  FormFields ff = {0};
  form_fields_text(&ff, F_NAME, "Name", NULL, NULL);
  form_fields_toggle(&ff, F_UNIQUE, "Unique", false);
  form_fields_list(&ff, F_COLS, "Columns", NULL);
  const char **names;
  size_t n = subview_column_names(v, &names);
  form_fields_options(&ff, F_COLS, names, n);
  free(names);
  form_fields_pick(&ff, F_METHOD, "Method", NULL, true);
  if (d == LACE_DRIVER_POSTGRES) form_fields_options(&ff, F_METHOD, pg_methods, 6);
  else if (subview_mysql(d)) form_fields_options(&ff, F_METHOD, my_methods, 4);
  else form_fields_disable(&ff, F_METHOD, "SQLite indexes have no method");
  form_fields_text(&ff, F_WHERE, "Partial (WHERE)", NULL, "a condition: status = 'open'");
  if (subview_mysql(d)) form_fields_disable(&ff, F_WHERE, "MySQL has no partial indexes");
  IxReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  char *title = str_printf("New index on %s", v->table);
  form_open(app, title, &ff, index_submit, rq, ixreq_free);
  free(title);
}

/* ---- a check constraint ---- */

static void check_submit(NcApp *app, Form *f, void *data) {
  IxReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  if (!v) {
    form_set_message(f, "The table is gone", true);
    return;
  }
  const char *cond = form_text(f, F_COND);
  if (!*cond) {
    form_set_message(f, "A condition is needed", true);
    return;
  }
  char *name = str_dup(form_text(f, F_NAME)), *cond_s = str_dup(cond);
  char *err = NULL, *sql = lace_ddl_add_check(subview_driver(app, v), v->table, or_null(name), cond_s, &err);
  if (!sql) {
    app_ddl_refused(app, f->uid, err);
  } else {
    char *summary = str_printf("Add the check constraint %s(%s) to %s.\nEvery row must satisfy it already.",
                               *name ? name : "", cond_s, v->table);
    app_ddl_confirm(app, v, "Add check constraint", summary, sql, false, DDL_AFTER_SCHEMA, NULL, f->uid);
    free(summary);
  }
  free(name);
  free(cond_s);
}

static void open_check_editor(NcApp *app, View *v) {
  if (subview_driver(app, v) == LACE_DRIVER_SQLITE) {
    app_set_status(app, true, "SQLite takes check constraints only at CREATE TABLE; create a new table and copy the rows");
    return;
  }
  FormFields ff = {0};
  form_fields_text(&ff, F_NAME, "Name", NULL, "optional");
  form_fields_text(&ff, F_COND, "Condition", NULL, "total >= 0");
  IxReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  char *title = str_printf("New check constraint on %s", v->table);
  form_open(app, title, &ff, check_submit, rq, ixreq_free);
  free(title);
}

/* ---- drop, rename ---- */

static const LaceIndex *tag_index(const View *v, int tag) {
  if (tag < CATALOG_TAG_INDEX || !v->schema) return NULL;
  size_t i = (size_t)(tag - CATALOG_TAG_INDEX);
  return i < v->schema->num_indexes ? &v->schema->indexes[i] : NULL;
}

static const char *kind_word(const LaceIndex *ix) {
  switch (ix->kind) {
  case LACE_INDEX_CHECK: return "check constraint";
  case LACE_INDEX_EXCLUSION: return "exclusion constraint";
  case LACE_INDEX_UNIQUE: return "unique index";
  default: return "index";
  }
}

static void drop_index(NcApp *app, View *v, const LaceIndex *ix) {
  if (ix->kind == LACE_INDEX_PRIMARY) {
    app_set_status(app, false, "The primary key is changed from Structure (p)");
    return;
  }
  LaceDriver d = subview_driver(app, v);
  char *err = NULL, *sql;
  if (ix->kind == LACE_INDEX_CHECK || ix->kind == LACE_INDEX_EXCLUSION)
    sql = lace_ddl_drop_constraint(d, v->table, ix->name, &err);
  else
    sql = lace_ddl_drop_index(d, v->table, ix->name, ix->constraint, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
    return;
  }
  char *summary = str_printf("Drop the %s %s of %s.", kind_word(ix), ix->name ? ix->name : "", v->table);
  app_ddl_confirm(app, v, "Drop index", summary, sql, true, DDL_AFTER_SCHEMA, NULL, 0);
  free(summary);
}

static void renamed(NcApp *app, char *text, void *data) {
  IxReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  char *name = text ? str_dup(text) : NULL;
  free(text);
  const LaceIndex *ix = NULL;
  for (size_t i = 0; v && v->schema && i < v->schema->num_indexes; i++)
    if (v->schema->indexes[i].name && !strcmp(v->schema->indexes[i].name, rq->index)) ix = &v->schema->indexes[i];
  if (!v || !ix || !name || !*name || !strcmp(name, rq->index)) {
    if (v && name && !*name) app_set_status(app, false, "No change");
    free(name);
    ixreq_free(rq);
    return;
  }
  LaceDriver d = subview_driver(app, v);
  char *err = NULL, *sql;
  if (ix->kind == LACE_INDEX_CHECK || ix->kind == LACE_INDEX_EXCLUSION)
    sql = lace_ddl_rename_constraint(d, v->table, ix->name, name, &err);
  else
    sql = lace_ddl_rename_index(d, v->table, ix, name, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
  } else {
    char *summary = str_printf("Rename the %s %s of %s to %s%s", kind_word(ix), ix->name, v->table, name,
                               strchr(sql, ';') ? " (dropped and created again)" : "");
    app_ddl_confirm(app, v, "Rename index", summary, sql, false, DDL_AFTER_SCHEMA, NULL, 0);
    free(summary);
  }
  free(name);
  ixreq_free(rq);
}

static void rename_index(NcApp *app, View *v, const LaceIndex *ix) {
  if (ix->kind == LACE_INDEX_PRIMARY) {
    app_set_status(app, false, "The primary key is changed from Structure (p)");
    return;
  }
  IxReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->uid = v->uid;
  rq->index = str_dup(ix->name ? ix->name : "");
  char *title = str_printf("Rename %s", ix->name ? ix->name : "index");
  prompt_open(app, title, "New name", ix->name, false, renamed, rq);
  free(title);
}

static bool indexes_action(NcApp *app, View *v, HotkeyAction a, int tag) {
  if (!v->schema) {
    app_set_status(app, false, "The structure is still loading");
    return true;
  }
  const LaceIndex *ix = tag_index(v, tag);
  switch (a) {
  case HOTKEY_STRUCT_ADD: open_index_editor(app, v); return true;
  case HOTKEY_STRUCT_CHECK: open_check_editor(app, v); return true;
  case HOTKEY_STRUCT_DROP:
    if (ix) drop_index(app, v, ix);
    else app_set_status(app, false, "Pick an index first");
    return true;
  case HOTKEY_STRUCT_RENAME:
  case HOTKEY_STRUCT_EDIT:
    if (ix) rename_index(app, v, ix);
    else app_set_status(app, false, "Pick an index first");
    return true;
  case HOTKEY_STRUCT_PRIMARY_KEY: app_set_status(app, false, "The primary key is changed from Structure (s, then p)"); return true;
  case HOTKEY_STRUCT_MOVE: return true;
  default: return false;
  }
}

static void indexes_activate(NcApp *app, View *v, int tag) {
  const LaceIndex *ix = tag_index(v, tag);
  if (ix) rename_index(app, v, ix);
}

const SvOps sv_indexes_ops = {indexes_build, indexes_activate, "Enter rename", indexes_action,
                              "+ index  c check  x drop  R rename"};
