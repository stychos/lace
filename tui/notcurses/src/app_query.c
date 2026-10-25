/* Running statements of a query view: one statement (paged when it only
 * reads), a script of statements, a script inside a transaction. Every job
 * carries the view's uid and its run sequence number: an answer for a view
 * that was closed, or for an older run, is dropped. */
#include "app.h"
#include "model/workspace.h"
#include "ui/grid.h"
#include "util/mem.h"
#include "util/str.h"
#include <config/history.h>
#include <stdlib.h>
#include <string.h>

static bool is_cancel(const NcJob *job) { return job->cancelled || job->code == LACE_ERR_QUERY_CANCELLED; }

typedef struct { NcApp *app; uint64_t uid, seq; } QReq;

static QReq *qreq_new(NcApp *app, const View *v) {
  QReq *r = safe_calloc(1, sizeof *r);
  r->app = app;
  r->uid = v->uid;
  r->seq = v->run_seq;
  return r;
}

/* A live view of the request's current run, or NULL */
static View *qreq_view(const QReq *r) {
  View *v = app_find_view(r->app, r->uid);
  return v && v->run_seq == r->seq ? v : NULL;
}

View *app_open_query(NcApp *app, const char *text) {
  ConnTab *t = app_current_tab(app);
  if (!t || !t->conn) return NULL;
  View *v = conntab_add_view(t, view_new_query(text));
  app_view_attach(app, v);
  return v;
}

/* Nothing of the previous statement stays on screen */
static void reset_results(View *v, size_t page_size) {
  grid_edit_cancel(v);
  page_cache_clear(v->cache, page_size);
  view_columns_changed(v);
  lace_result_free(v->once);
  v->once = NULL;
  v->affected = -1;
  v->ran = false;
  free(v->error);
  v->error = NULL;
  v->cursor_row = v->cursor_col = v->scroll_row = v->scroll_col = 0;
  view_clear_selection(v);
  v->fail_set = false;
  v->jump_pending = false;
  v->count_pending = false;
  free(v->source_table);
  v->source_table = NULL;
  view_set_schema(v, NULL);
  str_secure_free(v->base_sql);
  v->base_sql = NULL;
  v->paged = v->wrap = false;
}

static void report_error(NcApp *app, View *v, const NcJob *job, const char *suffix) {
  if (is_cancel(job)) {
    app_set_status(app, false, "Cancelled%s", suffix ? suffix : "");
    return;
  }
  if (v) {
    free(v->error);
    v->error = str_dup(job->error ? job->error : "Unknown error");
  }
  app_set_status(app, true, "%s%s", job->error ? job->error : "Unknown error", suffix ? suffix : "");
}

/* A finished statement's result: rows go to a one-page cache sized to them,
 * a result without columns is a count of affected rows */
static void show_result(NcApp *app, View *v, LaceResult *r) {
  v->ran = true;
  if (r && r->num_columns > 0) {
    size_t n = r->num_rows;
    page_cache_clear(v->cache, n ? n : 1);
    view_columns_changed(v);
    bool truncated = r->truncated;
    page_cache_page_loaded(v->cache, page_cache_generation(v->cache), 0, r);
    page_cache_set_total(v->cache, n, true, false);
    view_columns_changed(v);
    view_measure(v, 0, n);
    if (truncated) app_set_status(app, false, "Showing the first %zu rows", n);
    return;
  }
  v->affected = r && r->rows_affected > 0 ? r->rows_affected : 0;
  if (r) lace_result_free(r);
  app_set_status(app, false, "%lld %s affected", (long long)v->affected, v->affected == 1 ? "row" : "rows");
}

/* ==========================================================================
 * Source table (editable results)
 * ========================================================================== */

static void on_source_schema(NcJob *job, void *data) {
  QReq *rq = data;
  app_log_job(rq->app, job, "schema");
  View *v = qreq_view(rq);
  if (!v || job->error) return; /* the results simply stay read-only */
  view_set_schema(v, nc_job_steal_schema(job));
  view_measure(v, 0, page_cache_extent(v->cache)); /* room for the key marks in the header */
}

/* ==========================================================================
 * One statement
 * ========================================================================== */

static void on_exec(NcJob *job, void *data) {
  QReq *rq = data;
  app_log_job(rq->app, job, "statement");
  View *v = qreq_view(rq);
  if (!v) return;
  v->running = false;
  if (job->error) {
    report_error(rq->app, v, job, NULL);
    return;
  }
  show_result(rq->app, v, nc_job_steal_result(job));
}

static void exec_once(NcApp *app, View *v, ConnTab *t, const char *sql) {
  v->paged = false;
  nc_connection_submit_full(t->conn, nc_job_exec(sql), t->owner, on_exec, qreq_new(app, v), free);
}

void app_query_first_page_failed(NcApp *app, View *v) {
  v->first_page_pending = false;
  ConnTab *t = app_view_tab(app, v);
  if (!t || !t->conn || !v->base_sql) {
    v->running = false;
    return;
  }
  char *sql = str_dup(v->base_sql);
  exec_once(app, v, t, sql);
  str_secure_free(sql);
}

void app_query_first_page_ok(NcApp *app, View *v) {
  v->first_page_pending = false;
  v->running = false;
  v->ran = true;
  ConnTab *t = app_view_tab(app, v);
  if (t && t->conn && v->base_sql)
    nc_connection_record_history(t->conn, v->base_sql, lace_history_detect_type(v->base_sql));
  if (!page_cache_total_known(v->cache)) app_view_count(app, v);
}

/* Starts one statement; the caller has checked that nothing runs */
static void run_statement(NcApp *app, View *v, const char *sql) {
  ConnTab *t = app_view_tab(app, v);
  if (!t || !t->conn || nc_connection_closed(t->conn)) {
    v->running = false;
    app_set_status(app, true, "Not connected");
    return;
  }
  LaceDriver d = nc_connection_driver(t->conn);
  LaceSqlShape sh;
  lace_sql_shape(sql, d, &sh);
  if (sh.end == 0) {
    v->running = false;
    app_set_status(app, false, "Nothing to run");
    return;
  }
  app_view_attach(app, v);
  v->run_seq++;
  v->running = true;
  v->cancel_run = false;
  reset_results(v, TUI_PAGE_SIZE);
  v->base_sql = str_ndup(sql, sh.end);
  v->paged = lace_sql_shape_pageable(&sh);
  v->wrap = lace_sql_shape_needs_wrap(&sh);
  if (!v->paged) {
    exec_once(app, v, t, sql);
    return;
  }
  v->source_table = lace_sql_simple_select_table(v->base_sql, d);
  v->first_page_pending = true;
  page_cache_request(v->cache, 0);
  if (v->source_table) /* after the page: Esc must meet the page first */
    nc_connection_submit_full(t->conn, nc_job_schema(v->source_table), t->owner, on_source_schema,
                              qreq_new(app, v), free);
  if (!page_cache_page_pending(v->cache, 0)) { /* could not even be asked for */
    v->first_page_pending = false;
    app_query_first_page_failed(app, v);
  }
}

void app_query_run(NcApp *app, View *v, const char *sql) {
  if (!v || v->kind != VIEW_QUERY || !sql) return;
  if (v->running) {
    app_set_status(app, false, "A statement is already running");
    return;
  }
  run_statement(app, v, sql);
}

/* ==========================================================================
 * Scripts
 * ========================================================================== */

typedef struct {
  NcApp *app;
  uint64_t uid, seq;
  int refs; /* jobs in flight; main thread only */
  char *text;
  size_t pos;
  bool transaction, began, last_step;
  NcConnection *conn; /* referenced while a transaction may be open */
} Script;

static void script_unref(void *p) {
  Script *s = p;
  if (--s->refs > 0) return;
  str_secure_free(s->text);
  if (s->conn) nc_connection_unref(s->conn);
  free(s);
}

static void script_submit(Script *s, NcConnection *conn, NcOwner *owner, NcJob *job, NcJobDone done) {
  s->refs++;
  nc_connection_submit_full(conn, job, owner, done, s, script_unref);
}

static NcJob *plain_job(const char *sql) {
  NcJob *j = nc_job_exec(sql);
  j->record = false; /* BEGIN/COMMIT/ROLLBACK are not the user's statements */
  return j;
}

static void rollback_done(NcJob *job, void *data) {
  Script *s = data;
  app_log_job(s->app, job, "ROLLBACK");
  if (job->error) app_set_status(s->app, true, "ROLLBACK failed: %s", job->error);
}

/* ROLLBACK on the script's connection, whether or not its view is still there */
static void script_rollback(Script *s) {
  if (!s->began || !s->conn || nc_connection_closed(s->conn)) return;
  s->began = false;
  s->refs++;
  nc_connection_submit_full(s->conn, plain_job("ROLLBACK"), NULL, rollback_done, s, script_unref);
}

static void script_next(NcApp *app, View *v, Script *s);
static void on_step(NcJob *job, void *data);

/* Esc stopped the run: roll back what was begun and say so */
static void script_stopped(Script *s, View *v) {
  bool rolled = s->began;
  script_rollback(s);
  v->running = false;
  v->cancel_run = false;
  app_set_status(s->app, false, "Cancelled%s", rolled ? " (transaction rolled back)" : "");
}

static void on_begin(NcJob *job, void *data) {
  Script *s = data;
  app_log_job(s->app, job, "BEGIN");
  View *v = app_find_view(s->app, s->uid);
  bool alive = v && v->run_seq == s->seq;
  if (!job->error) s->began = true;
  if (!alive) { /* the view is gone: do not leave the transaction open */
    script_rollback(s);
    return;
  }
  if (job->error) {
    v->running = false;
    report_error(s->app, v, job, NULL);
    return;
  }
  if (v->cancel_run) {
    script_stopped(s, v);
    return;
  }
  script_next(s->app, v, s);
}

static void on_commit(NcJob *job, void *data) {
  Script *s = data;
  app_log_job(s->app, job, "COMMIT");
  View *v = app_find_view(s->app, s->uid);
  bool alive = v && v->run_seq == s->seq;
  if (alive) v->committing = false;
  if (!job->error) {
    s->began = false;
  } else {
    script_rollback(s); /* whether or not the view is still there */
  }
  if (!alive) return;
  v->running = false;
  if (job->error) {
    report_error(s->app, v, job, " (not committed)");
    return;
  }
  app_set_status(s->app, false, "Committed");
}

static void script_next(NcApp *app, View *v, Script *s) {
  ConnTab *t = app_view_tab(app, v);
  if (!t || !t->conn || nc_connection_closed(t->conn)) {
    v->running = false;
    return;
  }
  if (v->cancel_run) {
    script_stopped(s, v);
    return;
  }
  LaceDriver d = nc_connection_driver(t->conn);
  size_t a, b, next, a2, b2, n2;
  if (!lace_sql_next_statement(s->text, s->pos, d, &a, &b, &next)) {
    v->running = false;
    return;
  }
  s->last_step = !lace_sql_next_statement(s->text, next, d, &a2, &b2, &n2);
  s->pos = next;
  char *stmt = str_ndup(s->text + a, b - a);
  if (s->last_step && !s->transaction) {
    run_statement(app, v, stmt); /* the last one is shown as a single run would show it */
  } else {
    if (s->last_step) { /* inside the transaction: shown whole, the COMMIT follows */
      str_secure_free(v->base_sql);
      v->base_sql = str_dup(stmt);
      v->paged = false;
    }
    script_submit(s, t->conn, t->owner, nc_job_exec(stmt), on_step);
  }
  str_secure_free(stmt);
}

static void on_step(NcJob *job, void *data) {
  Script *s = data;
  app_log_job(s->app, job, "statement");
  NcApp *app = s->app;
  View *v = app_find_view(app, s->uid);
  bool alive = v && v->run_seq == s->seq;
  if (job->error) {
    bool rolled = s->began;
    script_rollback(s);
    if (alive) {
      v->running = false;
      v->cancel_run = false;
      report_error(app, v, job, rolled ? " (transaction rolled back)" : NULL);
    }
    return;
  }
  if (!alive) {
    script_rollback(s);
    return;
  }
  if (!s->last_step) {
    script_next(app, v, s);
    return;
  }
  if (v->cancel_run) { /* Esc landed after this statement finished */
    script_stopped(s, v);
    return;
  }
  /* The last statement of a transaction: keep its result, then COMMIT */
  show_result(app, v, nc_job_steal_result(job));
  ConnTab *t = app_view_tab(app, v);
  if (!t || !t->conn) {
    v->running = false;
    return;
  }
  v->committing = true; /* Esc is ignored until it answers (app_cancel_running) */
  script_submit(s, t->conn, t->owner, plain_job("COMMIT"), on_commit);
}

void app_query_run_all(NcApp *app, View *v, bool transaction) {
  if (!v || v->kind != VIEW_QUERY) return;
  ConnTab *t = app_view_tab(app, v);
  if (!t || !t->conn || nc_connection_closed(t->conn)) {
    app_set_status(app, true, "Not connected");
    return;
  }
  if (v->running) {
    app_set_status(app, false, "A statement is already running");
    return;
  }
  if (transaction && nc_connection_tx_state(t->conn) != NC_TX_NONE) {
    app_set_status(app, true, "A transaction is already open");
    return;
  }
  LaceDriver d = nc_connection_driver(t->conn);
  size_t a, b, next;
  if (!v->text || !lace_sql_next_statement(v->text, 0, d, &a, &b, &next)) {
    app_set_status(app, false, "Nothing to run");
    return;
  }
  Script *s = safe_calloc(1, sizeof *s);
  s->app = app;
  s->uid = v->uid;
  s->text = str_dup(v->text);
  s->transaction = transaction;
  app_view_attach(app, v);
  v->run_seq++;
  s->seq = v->run_seq;
  v->running = true;
  v->cancel_run = false;
  reset_results(v, TUI_PAGE_SIZE);
  if (transaction) {
    s->conn = nc_connection_ref(t->conn);
    script_submit(s, t->conn, t->owner, plain_job("BEGIN"), on_begin);
  } else {
    s->refs = 1; /* held while script_next runs */
    script_next(app, v, s);
    script_unref(s);
  }
}
