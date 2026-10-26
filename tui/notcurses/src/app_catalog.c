/* What the sub-views besides Data load for a view: the table's information
 * and counter, the tables referencing it, the definition of a database
 * view, routine or trigger, and the exact row count asked for from
 * Information. Every job carries the view's uid; a late answer for a view
 * that is gone is dropped. */
#include "app.h"
#include "model/workspace.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  NcApp *app;
  uint64_t uid;
} CatReq;

static CatReq *req_new(NcApp *app, const View *v) {
  CatReq *r = safe_calloc(1, sizeof *r);
  r->app = app;
  r->uid = v->uid;
  return r;
}

static ConnTab *live_tab(NcApp *app, View *v) {
  ConnTab *t = app_view_tab(app, v);
  return t && t->conn && !nc_connection_closed(t->conn) ? t : NULL;
}

/* ---- information ---- */

static void on_info(NcJob *job, void *data) {
  CatReq *rq = data;
  NcApp *app = rq->app;
  char *label = str_printf("info %s", job->table ? job->table : "");
  app_log_job(app, job, label);
  free(label);
  View *v = app_find_view(app, rq->uid);
  if (!v) return;
  v->info_loading = false;
  if (job->error) {
    free(v->info_error);
    v->info_error = str_dup(job->cancelled ? "Cancelled" : job->error);
    return;
  }
  lace_table_info_free(v->info);
  lace_autoinc_free(v->autoinc);
  v->info = job->info;
  v->autoinc = job->autoinc;
  job->info = NULL;
  job->autoinc = NULL;
}

void app_view_load_info(NcApp *app, View *v) {
  ConnTab *t = live_tab(app, v);
  if (!t || !v->table || v->info_loading) return;
  free(v->info_error);
  v->info_error = NULL;
  v->info_loading = true;
  nc_connection_submit_full(t->conn, nc_job_table_info(v->table), t->owner, on_info, req_new(app, v), free);
}

/* ---- dependents (Referenced by) ---- */

static void on_deps(NcJob *job, void *data) {
  CatReq *rq = data;
  NcApp *app = rq->app;
  char *label = str_printf("dependents %s", job->table ? job->table : "");
  app_log_job(app, job, label);
  free(label);
  View *v = app_find_view(app, rq->uid);
  if (!v) return;
  v->deps_loading = false;
  if (job->error) {
    free(v->deps_error);
    v->deps_error = str_dup(job->cancelled ? "Cancelled" : job->error);
    v->referenced_by_pending = false;
    if (v->pending_action == 1) { /* the drop goes on without the list */
      v->pending_action = 0;
      app_table_drop_continue(app, v);
    }
    return;
  }
  lace_dependents_free(v->deps, v->num_deps);
  v->deps = job->deps;
  v->num_deps = job->num_deps;
  job->deps = NULL;
  job->num_deps = 0;
  v->deps_loaded = true;
  if (v->referenced_by_pending) {
    v->referenced_by_pending = false;
    app_referenced_by(app, v); /* the pick list that waited for the list */
  }
  if (v->pending_action == 1) {
    v->pending_action = 0;
    app_table_drop_continue(app, v); /* the drop's confirmation names them */
  }
}

void app_view_load_deps(NcApp *app, View *v) {
  ConnTab *t = live_tab(app, v);
  if (!t || !v->table || v->deps_loading) return;
  free(v->deps_error);
  v->deps_error = NULL;
  v->deps_loading = true;
  nc_connection_submit_full(t->conn, nc_job_dependents(v->table), t->owner, on_deps, req_new(app, v), free);
}

/* ---- definition ---- */

static void on_definition(NcJob *job, void *data) {
  CatReq *rq = data;
  NcApp *app = rq->app;
  char *label = str_printf("definition %s", job->sql ? job->sql : "");
  app_log_job(app, job, label);
  free(label);
  View *v = app_find_view(app, rq->uid);
  if (!v) return;
  v->definition_loading = false;
  if (job->error) {
    free(v->definition_error);
    v->definition_error = str_dup(job->cancelled ? "Cancelled" : job->error);
    return;
  }
  free(v->definition);
  v->definition = job->definition ? job->definition : str_dup("");
  job->definition = NULL;
}

void app_view_load_definition(NcApp *app, View *v) {
  ConnTab *t = live_tab(app, v);
  if (!t || !v->table || v->definition_loading || v->object == LACE_OBJ_TABLE) return;
  /* a trigger restored without its table: the object list knows it, once it is in */
  if (v->object == LACE_OBJ_TRIGGER && !v->object_table) {
    if (!nc_connection_tables_loaded(t->conn)) return; /* asked again by the next draw */
    const LaceObject *o = nc_connection_find_object(t->conn, LACE_OBJ_TRIGGER, v->table);
    if (!o || !o->table) {
      free(v->definition_error);
      v->definition_error = str_printf("Trigger %s was not found", v->table);
      return;
    }
    v->object_table = str_dup(o->table);
  }
  if (v->object == LACE_OBJ_ROUTINE && !v->object_detail && nc_connection_tables_loaded(t->conn)) {
    const LaceObject *o = nc_connection_find_object(t->conn, LACE_OBJ_ROUTINE, v->table);
    if (o && o->detail) v->object_detail = str_dup(o->detail);
  }
  free(v->definition_error);
  v->definition_error = NULL;
  v->definition_loading = true;
  nc_connection_submit_full(t->conn, nc_job_definition(v->object, v->table, v->object_table, v->object_detail),
                            t->owner, on_definition, req_new(app, v), free);
}

/* ---- the exact count ---- */

static void on_exact(NcJob *job, void *data) {
  CatReq *rq = data;
  NcApp *app = rq->app;
  app_log_job(app, job, "count");
  View *v = app_find_view(app, rq->uid);
  if (!v) return;
  v->info_count_pending = false;
  if (job->error || job->count < 0) {
    app_set_status(app, !job->cancelled, "%s", job->cancelled ? "Cancelled" : job->error ? job->error : "No count");
    return;
  }
  v->info_count = job->count;
}

void app_view_count_exact(NcApp *app, View *v) {
  ConnTab *t = live_tab(app, v);
  if (!t || !v->table || v->info_count_pending) return;
  v->info_count_pending = true;
  NcJob *job = nc_job_table_count(v->table, NULL, false);
  job->user_statement = true; /* asked for: the lock banner shows while it counts */
  nc_connection_submit_full(t->conn, job, t->owner, on_exact, req_new(app, v), free);
}

/* ---- refresh ---- */

void app_view_refresh_catalog(NcApp *app, View *v) {
  view_drop_catalog(v);
  if (view_has_subview(v, SV_DATA)) app_view_reload_schema(app, v); /* the columns, keys and indexes again */
}
