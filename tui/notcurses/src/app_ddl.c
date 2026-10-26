#include "app_ddl.h"
#include "model/workspace.h"
#include "ui/dialogs/confirm.h"
#include "ui/form.h"
#include "ui/subviews.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  NcApp *app;
  uint64_t uid; /* the view the change is about */
  NcConnection *conn; /* referenced: the after-effects reach every tab holding it */
  LaceObjectKind kind;
  char *object;       /* the object's name (the view's table) */
  char *title, *sql, *arg;
  DdlAfter after;
  uint64_t form_uid;
} DdlReq;

static void req_free(void *p) {
  DdlReq *r = p;
  if (r->conn) nc_connection_unref(r->conn);
  free(r->object);
  free(r->title);
  str_secure_free(r->sql);
  free(r->arg);
  free(r);
}

char *app_ddl_rows_text(const View *v) {
  const char *name = v->table ? v->table : "";
  if (v->kind != VIEW_TABLE || !v->cache || !page_cache_total_known(v->cache) || v->num_filters) return str_dup(name);
  char *n = catalog_commas((int64_t)page_cache_extent(v->cache));
  char *out = str_printf("%s (%s%s %s)", name, page_cache_total_approximate(v->cache) ? "\xe2\x89\x88" : "", n,
                         page_cache_extent(v->cache) == 1 ? "row" : "rows");
  free(n);
  return out;
}

void app_ddl_refused(NcApp *app, uint64_t form_uid, char *error) {
  const char *why = error && *error ? error : "Not possible here";
  app_set_status(app, true, "%s", why);
  Form *f = form_uid ? form_find(app, form_uid) : NULL;
  if (f) form_set_message(f, why, true);
  free(error);
}

/* ==========================================================================
 * After a successful change
 * ========================================================================== */

/* The views showing the object on the connection, in every workspace */
static bool same_object(const View *v, LaceObjectKind kind, const char *object) {
  if (v->kind != VIEW_TABLE || !v->table || !object) return false;
  bool table_like_a = v->object == LACE_OBJ_TABLE || v->object == LACE_OBJ_VIEW;
  bool table_like_b = kind == LACE_OBJ_TABLE || kind == LACE_OBJ_VIEW;
  if (table_like_a != table_like_b || (!table_like_a && v->object != kind)) return false;
  return strcmp(v->table, object) == 0;
}

static void after_change(NcApp *app, DdlReq *r) {
  bool sidebar = r->after == DDL_AFTER_TABLES || r->after == DDL_AFTER_RENAME || r->after == DDL_AFTER_DROP ||
                 r->after == DDL_AFTER_CREATE_TABLE;
  for (size_t w = 0; w < app->num_workspaces; w++)
    for (size_t k = 0; k < app->workspaces[w]->num_tabs; k++) {
      ConnTab *t = app->workspaces[w]->tabs[k];
      if (t->conn != r->conn) continue;
      for (size_t i = t->num_views; i-- > 0;) {
        View *v = t->views[i];
        if (!same_object(v, r->kind, r->object)) continue;
        if (r->after == DDL_AFTER_DROP) {
          conntab_remove_view(t, i); /* a tab left without views shows its hint */
          continue;
        }
        if (r->after == DDL_AFTER_RENAME && r->arg) {
          free(v->table);
          v->table = str_dup(r->arg);
        }
        undo_stack_clear(&v->undo); /* the steps name columns that may be gone */
        view_drop_catalog(v);
        if (view_has_subview(v, SV_DATA)) app_view_reload_schema(app, v);
      }
    }
  if (sidebar && !nc_connection_closed(r->conn)) nc_connection_refresh_tables(r->conn);
  if (r->after == DDL_AFTER_CREATE_TABLE && r->arg) {
    ConnTab *t = app_current_tab(app);
    if (t && t->conn == r->conn) {
      View *nv = app_open_table(app, r->arg);
      if (nv) subview_set(app, nv, SV_STRUCTURE);
    }
  }
}

static void on_done(NcJob *job, void *data) {
  DdlReq *r = data;
  NcApp *app = r->app;
  app_log_job(app, job, r->title);
  Form *f = r->form_uid ? form_find(app, r->form_uid) : NULL;
  if (job->error) {
    const char *why = job->cancelled ? "Cancelled" : job->error;
    app_set_status(app, !job->cancelled, "%s: %s", r->title, why);
    if (f) form_set_busy(f, false, NULL), form_set_message(f, why, !job->cancelled);
    return;
  }
  app_set_status(app, false, "%s: done", r->title);
  if (f) form_close(app, f);
  after_change(app, r);
}

/* ==========================================================================
 * Confirming and running
 * ========================================================================== */

static void run(NcApp *app, DdlReq *r) {
  if (nc_connection_closed(r->conn)) {
    app_set_status(app, true, "Not connected");
    req_free(r);
    return;
  }
  Form *f = r->form_uid ? form_find(app, r->form_uid) : NULL;
  if (f) form_set_busy(f, true, "Running\xe2\x80\xa6");
  NcJob *job = nc_job_exec(r->sql);
  job->user_statement = true; /* the lock banner */
  nc_connection_submit_full(r->conn, job, NULL, on_done, r, req_free);
}

static void confirmed(NcApp *app, bool yes, void *data) {
  DdlReq *r = data;
  if (!yes) {
    app_set_status(app, false, "%s: cancelled", r->title);
    req_free(r);
    return;
  }
  run(app, r);
}

void app_ddl_confirm(NcApp *app, View *v, const char *title, const char *summary, char *sql, bool destructive,
                     DdlAfter after, char *arg, uint64_t form_uid) {
  ConnTab *t = v ? app_view_tab(app, v) : NULL;
  if (!t || !t->conn || nc_connection_closed(t->conn) || !sql) {
    app_set_status(app, true, "Not connected");
    str_secure_free(sql);
    free(arg);
    return;
  }
  DdlReq *r = safe_calloc(1, sizeof *r);
  r->app = app;
  r->uid = v->uid;
  r->conn = nc_connection_ref(t->conn);
  r->kind = v->object;
  r->object = str_dup(v->table ? v->table : "");
  r->title = str_dup(title ? title : "Change");
  r->sql = sql;
  r->arg = arg;
  r->after = after;
  r->form_uid = form_uid;
  const char *tx_note = "";
  if (nc_connection_tx_state(t->conn) == NC_TX_OPEN) {
    LaceDriver d = nc_connection_driver(t->conn);
    tx_note = d == LACE_DRIVER_MYSQL || d == LACE_DRIVER_MARIADB
                  ? "\n\nMySQL commits the open transaction first."
                  : "\n\nRuns inside the open transaction: ROLLBACK undoes it.";
  }
  char *msg = str_printf("%s\n\n%s%s", summary ? summary : "", sql, tx_note);
  confirm_open(app, r->title, msg, destructive ? r->title : "Run", destructive, confirmed, r);
  free(msg);
}

/* ==========================================================================
 * Pickers' names, a query view for templates
 * ========================================================================== */

typedef struct {
  NcApp *app;
  uint64_t form_uid;
  int field_id;
} NamesReq;

static void on_names(NcJob *job, void *data) {
  NamesReq *rq = data;
  app_log_job(rq->app, job, job->column ? job->column : "names");
  Form *f = form_find(rq->app, rq->form_uid);
  if (!f || job->error) return; /* the field keeps its free text */
  form_set_options(f, rq->field_id, job->tables, job->num_tables);
  job->tables = NULL;
  job->num_tables = 0;
}

void app_ddl_fetch_names(NcApp *app, View *v, uint64_t form_uid, int field_id, const char *what, const char *arg) {
  ConnTab *t = app_view_tab(app, v);
  if (!t || !t->conn || nc_connection_closed(t->conn)) return;
  NamesReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = app;
  rq->form_uid = form_uid;
  rq->field_id = field_id;
  nc_connection_submit_full(t->conn, nc_job_names(what, arg), t->owner, on_names, rq, free);
}

View *app_ddl_open_query(NcApp *app, View *v, const char *text) {
  ConnTab *t = app_view_tab(app, v);
  if (!t || !t->conn) return NULL;
  View *q = conntab_add_view(t, view_new_query(text));
  app_view_attach(app, q);
  q->reload_tables_on_run = true;
  t->sidebar_focused = false;
  return q;
}
