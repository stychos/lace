/* The Information sub-view: the table's facts (lace_table_info and its
 * counter, loaded when first shown), the exact row count on demand, the
 * triggers on the table (from the connection's object list; Enter opens
 * one), and under "Actions" what can be changed from here: the counter, the
 * comment, MySQL's engine and charset, and the maintenance statements
 * (Analyze, Vacuum, Optimize), each through app_ddl_confirm. A database
 * view, routine or trigger shows what the catalog listed and "Edit in a
 * query view". */
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
  /* the actions */
  LaceDriver d = subview_driver(app, v);
  bool mysql = subview_mysql(d), pg = d == LACE_DRIVER_POSTGRES, sqlite = d == LACE_DRIVER_SQLITE;
  catalog_rows_blank(t);
  catalog_rows_heading(t, "Actions");
  if (v->object != LACE_OBJ_TABLE) {
    catalog_rows_span(t, SV_TAG_EDIT_DEFINITION, "Edit in a query view");
    return true;
  }
  if (v->autoinc && v->autoinc->column)
    catalog_rows_span(t, SV_TAG_AUTOINC, v->autoinc->has_counter ? "Set the next auto-increment value\xe2\x80\xa6"
                                                                  : "Set the next auto-increment value (no counter: the maximum + 1)");
  if (pg || mysql) catalog_rows_span(t, SV_TAG_COMMENT, "Set the comment\xe2\x80\xa6");
  if (mysql) {
    catalog_rows_span(t, SV_TAG_ENGINE, "Set the engine\xe2\x80\xa6");
    catalog_rows_span(t, SV_TAG_CHARSET, "Set the charset and collation\xe2\x80\xa6");
  }
  catalog_rows_span(t, SV_TAG_ANALYZE, "Analyze (update the statistics)");
  if (pg) catalog_rows_span(t, SV_TAG_VACUUM, "Vacuum (VACUUM ANALYZE the table)");
  if (sqlite) catalog_rows_span(t, SV_TAG_VACUUM, "Vacuum (the whole file: SQLite has no per-table VACUUM)");
  if (mysql) catalog_rows_span(t, SV_TAG_OPTIMIZE, "Optimize (OPTIMIZE TABLE)");
  return true;
}

/* ==========================================================================
 * The actions
 * ========================================================================== */

typedef struct {
  NcApp *app;
  uint64_t uid;
} InfoReq;

static InfoReq *req_new(NcApp *app, const View *v) {
  InfoReq *r = safe_calloc(1, sizeof *r);
  r->app = app;
  r->uid = v->uid;
  return r;
}

/* ---- the counter ---- */

static void autoinc_set(NcApp *app, char *text, void *data) {
  InfoReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  free(rq);
  int64_t n;
  bool ok = v && text && str_to_int64(text, &n);
  if (text && v && !ok && *text) app_set_status(app, true, "Not a number: %s", text);
  free(text);
  if (!ok || !v->autoinc) return;
  char *err = NULL, *sql = lace_ddl_set_autoincrement(subview_driver(app, v), v->table, v->autoinc, n, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
    return;
  }
  char *summary = str_printf("The next %s of %s becomes %lld%s", v->autoinc->column, v->table, (long long)n,
                             v->autoinc->max >= 0 && n <= v->autoinc->max ? " (MySQL refuses a value below the maximum; the rest allow it)" : "");
  app_ddl_confirm(app, v, "Set auto-increment", summary, sql, false, DDL_AFTER_SCHEMA, NULL, 0);
  free(summary);
}

static void autoinc_prompt(NcApp *app, View *v) {
  if (!v->autoinc || !v->autoinc->column) return;
  if (!v->autoinc->has_counter) {
    app_set_status(app, false, "A plain INTEGER PRIMARY KEY has no counter: the next rowid is the maximum + 1");
    return;
  }
  char *initial = v->autoinc->next >= 0 ? str_printf("%lld", (long long)v->autoinc->next) : str_dup("1");
  prompt_open(app, "Next auto-increment value", "Next value", initial, false, autoinc_set, req_new(app, v));
  free(initial);
}

/* ---- the comment ---- */

static void comment_set(NcApp *app, char *text, void *data) {
  InfoReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  free(rq);
  if (!v || !text) {
    free(text);
    return;
  }
  char *err = NULL, *sql = lace_ddl_set_table_option(subview_driver(app, v), v->table, LACE_DDL_OPT_COMMENT, text, NULL, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
  } else {
    char *summary = *text ? str_printf("Set the comment of %s to \"%s\"", v->table, text)
                          : str_printf("Clear the comment of %s", v->table);
    app_ddl_confirm(app, v, "Set comment", summary, sql, false, DDL_AFTER_SCHEMA, NULL, 0);
    free(summary);
  }
  free(text);
}

/* ---- the engine ---- */

typedef struct {
  NcApp *app;
  uint64_t uid;
  char **names;
  size_t n;
} EngineReq;

static void engine_free(EngineReq *r) {
  lace_tables_free(r->names, r->n);
  free(r);
}

static void engine_picked(NcApp *app, size_t index, void *data) {
  EngineReq *r = data;
  View *v = app_find_view(app, r->uid);
  if (v && index < r->n) {
    char *err = NULL, *sql = lace_ddl_set_table_option(subview_driver(app, v), v->table, LACE_DDL_OPT_ENGINE, r->names[index], NULL, &err);
    if (!sql) {
      app_ddl_refused(app, 0, err);
    } else {
      char *summary = str_printf("Change the engine of %s to %s.\nThe table is rebuilt: it is locked meanwhile.", v->table, r->names[index]);
      app_ddl_confirm(app, v, "Set engine", summary, sql, false, DDL_AFTER_SCHEMA, NULL, 0);
      free(summary);
    }
  }
  engine_free(r);
}

static void on_engines(NcJob *job, void *data) {
  EngineReq *r = data;
  NcApp *app = r->app;
  app_log_job(app, job, "engines");
  View *v = app_find_view(app, r->uid);
  if (!v || job->error || !job->num_tables) {
    if (v) app_set_status(app, true, "No engines to pick from%s%s", job->error ? ": " : "", job->error ? job->error : "");
    engine_free(r);
    return;
  }
  r->names = job->tables;
  r->n = job->num_tables;
  job->tables = NULL;
  job->num_tables = 0;
  pick_open(app, "Engine", r->names, r->n, engine_picked, r);
}

static void engine_pick(NcApp *app, View *v) {
  ConnTab *t = app_view_tab(app, v);
  if (!t || !t->conn) return;
  EngineReq *r = safe_calloc(1, sizeof *r);
  r->app = app;
  r->uid = v->uid;
  nc_connection_submit_full(t->conn, nc_job_names("engines", NULL), t->owner, on_engines, r, NULL);
}

/* ---- charset and collation ---- */

enum { F_CHARSET = 1, F_COLLATION };

static void charset_submit(NcApp *app, Form *f, void *data) {
  InfoReq *rq = data;
  View *v = app_find_view(app, rq->uid);
  if (!v) {
    form_set_message(f, "The table is gone", true);
    return;
  }
  const char *cs = form_text(f, F_CHARSET);
  if (!*cs) {
    form_set_message(f, "A charset is needed", true);
    return;
  }
  char *cs_s = str_dup(cs), *coll = str_dup(form_text(f, F_COLLATION));
  char *err = NULL, *sql = lace_ddl_set_table_option(subview_driver(app, v), v->table, LACE_DDL_OPT_CHARSET, cs_s,
                                                     *coll ? coll : NULL, &err);
  if (!sql) {
    app_ddl_refused(app, f->uid, err);
  } else {
    char *summary = str_printf("Set the default charset of %s to %s%s%s (existing columns keep theirs)", v->table, cs_s,
                               *coll ? " with collation " : "", coll);
    app_ddl_confirm(app, v, "Set charset", summary, sql, false, DDL_AFTER_SCHEMA, NULL, f->uid);
    free(summary);
  }
  free(cs_s);
  free(coll);
}

static void charset_form(NcApp *app, View *v) {
  FormFields ff = {0};
  form_fields_text(&ff, F_CHARSET, "Charset", v->info ? v->info->charset : NULL, "utf8mb4");
  form_fields_pick(&ff, F_COLLATION, "Collation", v->info ? v->info->collation : NULL, true);
  char *title = str_printf("Charset of %s", v->table);
  Form *f = form_open(app, title, &ff, charset_submit, req_new(app, v), free);
  free(title);
  app_ddl_fetch_names(app, v, f->uid, F_COLLATION, "collations", v->info ? v->info->charset : NULL);
}

/* ---- maintenance ---- */

static void maintenance(NcApp *app, View *v, LaceDdlMaintenance kind) {
  LaceDriver d = subview_driver(app, v);
  char *err = NULL, *sql = lace_ddl_maintenance(d, v->table, kind, &err);
  if (!sql) {
    app_ddl_refused(app, 0, err);
    return;
  }
  const char *title = kind == LACE_DDL_ANALYZE ? "Analyze" : kind == LACE_DDL_VACUUM ? "Vacuum" : "Optimize";
  char *rows = app_ddl_rows_text(v);
  char *summary = kind == LACE_DDL_VACUUM && d == LACE_DRIVER_SQLITE
                      ? str_printf("Rebuild the whole database file (SQLite's VACUUM is not per table).\nLocks the database while it runs; Esc cancels.")
                      : str_printf("%s %s.\nLocks the table while it runs; Esc cancels.", title, rows);
  app_ddl_confirm(app, v, title, summary, sql, false, DDL_AFTER_SCHEMA, NULL, 0);
  free(summary);
  free(rows);
}

/* ==========================================================================
 * Enter and the keys
 * ========================================================================== */

static void info_activate(NcApp *app, View *v, int tag) {
  if (tag == CATALOG_TAG_ROWS) {
    if (!v->info_count_pending) app_view_count_exact(app, v);
    return;
  }
  switch (tag) {
  case SV_TAG_AUTOINC: autoinc_prompt(app, v); return;
  case SV_TAG_COMMENT: {
    char *title = str_printf("Comment on %s", v->table);
    prompt_open(app, title, "Comment", v->info ? v->info->comment : NULL, false, comment_set, req_new(app, v));
    free(title);
    return;
  }
  case SV_TAG_ENGINE: engine_pick(app, v); return;
  case SV_TAG_CHARSET: charset_form(app, v); return;
  case SV_TAG_ANALYZE: maintenance(app, v, LACE_DDL_ANALYZE); return;
  case SV_TAG_VACUUM: maintenance(app, v, LACE_DDL_VACUUM); return;
  case SV_TAG_OPTIMIZE: maintenance(app, v, LACE_DDL_OPTIMIZE); return;
  case SV_TAG_EDIT_DEFINITION:
    if (!v->definition) {
      app_view_load_definition(app, v);
      app_set_status(app, false, "The definition is still loading: open Definition first");
      return;
    }
    app_ddl_open_query(app, v, v->definition);
    return;
  default: break;
  }
  if (tag < CATALOG_TAG_TRIGGER || tag >= CATALOG_TAG_COLUMN) return;
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

static bool info_action(NcApp *app, View *v, HotkeyAction a, int tag) {
  if (a == HOTKEY_STRUCT_EDIT) {
    info_activate(app, v, tag);
    return true;
  }
  app_set_status(app, false, "Enter runs the action under the cursor; the table's actions are under m");
  return true;
}

const SvOps sv_info_ops = {info_build, info_activate, CHROME_SV_INFO_ENTER, info_action, NULL};
