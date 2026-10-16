/* Loading table views: schema -> page 0 -> count, page fetches for the page
 * cache, reloads after a sort or filter, cancelling. Every job carries the
 * view's uid (never a pointer: tabs and views come and go while it runs) and
 * the cache generation it was asked under; a result for an older generation
 * goes to the cache, which drops it. */
#include "app.h"
#include "model/workspace.h"
#include "ui/grid.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  NcApp *app;
  uint64_t uid, gen;
  size_t page;
} Req;

View *app_find_view(NcApp *app, uint64_t uid) {
  for (size_t i = 0; i < app->num_workspaces; i++)
    for (size_t k = 0; k < app->workspaces[i]->num_tabs; k++) {
      ConnTab *t = app->workspaces[i]->tabs[k];
      for (size_t j = 0; j < t->num_views; j++)
        if (t->views[j]->uid == uid) return t->views[j];
    }
  return NULL;
}

ConnTab *app_view_tab(NcApp *app, const View *v) {
  for (size_t i = 0; i < app->num_workspaces; i++)
    for (size_t k = 0; k < app->workspaces[i]->num_tabs; k++) {
      ConnTab *t = app->workspaces[i]->tabs[k];
      for (size_t j = 0; j < t->num_views; j++)
        if (t->views[j] == v) return t;
    }
  return NULL;
}

static Req *req_new(NcApp *app, const View *v, size_t page) {
  Req *r = safe_calloc(1, sizeof *r);
  r->app = app;
  r->uid = v->uid;
  r->gen = page_cache_generation(v->cache);
  r->page = page;
  return r;
}

static bool is_cancel(const NcJob *job) {
  return job->cancelled || job->code == LACE_ERR_QUERY_CANCELLED;
}

/* A page that failed is not asked for again until the cursor or scroll moves */
static void note_fail(View *v, uint64_t gen, size_t page) {
  v->fail_set = true;
  v->fail_page = page;
  v->fail_gen = gen;
  v->fail_cursor = v->cursor_row;
  v->fail_scroll = v->scroll_row;
}

/* ==========================================================================
 * Schema
 * ========================================================================== */

static void load_started(NcApp *app, View *v);

static void on_schema(NcJob *job, void *data) {
  Req *rq = data;
  NcApp *app = rq->app;
  char *label = str_printf("schema %s", job->table ? job->table : "");
  app_log_job(app, job, label);
  free(label);
  View *v = app_find_view(app, rq->uid);
  if (!v) return;
  v->loading = false;
  if (job->error) {
    free(v->schema_error);
    v->schema_error = str_dup(job->error);
    if (is_cancel(job)) app_set_status(app, false, "Cancelled");
    else app_set_status(app, true, "%s: %s", v->table ? v->table : "table", job->error);
    return;
  }
  free(v->schema_error);
  v->schema_error = NULL;
  view_set_schema(v, nc_job_steal_schema(job));
  load_started(app, v);
}

static void load_schema(NcApp *app, View *v) {
  ConnTab *t = app_view_tab(app, v);
  if (!t || !t->conn) return;
  v->loading = true;
  nc_connection_submit_full(t->conn, nc_job_schema(v->table), t->owner, on_schema,
                            req_new(app, v, 0), free);
}

/* The schema is in: page 0, then the count */
static void load_started(NcApp *app, View *v) {
  page_cache_request(v->cache, 0);
  app_view_count(app, v);
}

/* ==========================================================================
 * Pages
 * ========================================================================== */

static void on_page(NcJob *job, void *data) {
  Req *rq = data;
  NcApp *app = rq->app;
  app_log_job(app, job, "page");
  View *v = app_find_view(app, rq->uid);
  if (!v) return;
  PageCache *pc = v->cache;
  bool current = rq->gen == page_cache_generation(pc);
  if (job->error) {
    page_cache_page_failed(pc, rq->gen, rq->page);
    if (!current) return;
    if (v->kind == VIEW_QUERY && rq->page == 0 && v->first_page_pending) {
      if (is_cancel(job)) { /* Esc: the statement stops here */
        v->first_page_pending = false;
        v->running = false;
        app_set_status(app, false, "Cancelled");
      } else {
        app_query_first_page_failed(app, v); /* the paged form failed: run it as written */
      }
      return;
    }
    note_fail(v, rq->gen, rq->page);
    if (is_cancel(job)) app_set_status(app, false, "Cancelled");
    else {
      free(v->error);
      v->error = str_dup(job->error);
      app_set_status(app, true, "%s", job->error);
    }
    if (v->has_checkpoint && page_cache_stale(pc)) { /* the reload failed: back to what was shown */
      view_revert(v);
      page_cache_unstale(pc);
      v->fail_set = false;
      v->jump_pending = false;
    }
    return;
  }
  LaceResult *rows = nc_job_steal_result(job);
  page_cache_page_loaded(pc, rq->gen, rq->page, rows);
  if (!current) return;
  v->fail_set = false;
  free(v->error);
  v->error = NULL;
  view_checkpoint_drop(v); /* the new state is on screen */
  if (view_columns_changed(v)) v->scroll_col = 0;
  size_t ps = page_cache_page_size(pc);
  view_measure(v, rq->page * ps, (rq->page + 1) * ps);
  if (v->kind == VIEW_QUERY && rq->page == 0 && v->first_page_pending) app_query_first_page_ok(app, v);
  grid_resolve_jump(app, v);
}

void app_view_request_page(NcApp *app, View *v, size_t page) {
  PageCache *pc = v->cache;
  uint64_t gen = page_cache_generation(pc);
  ConnTab *t = app_view_tab(app, v);
  char *sql = t && t->conn && !nc_connection_closed(t->conn)
                  ? view_page_sql(v, nc_connection_driver(t->conn), page)
                  : NULL;
  if (!sql) {
    page_cache_page_failed(pc, gen, page);
    note_fail(v, gen, page);
    return;
  }
  NcJob *job = nc_job_read(sql);
  free(sql);
  job->tag = (void *)(uintptr_t)page;
  nc_connection_submit_full(t->conn, job, t->owner, on_page, req_new(app, v, page), free);
}

static void fetch_cb(PageCache *pc, size_t page, void *data) {
  (void)pc;
  Req *ctx = data; /* the view's fetch context: app and uid */
  View *v = app_find_view(ctx->app, ctx->uid);
  if (v) app_view_request_page(ctx->app, v, page);
  else page_cache_page_failed(pc, page_cache_generation(pc), page);
}

/* ==========================================================================
 * Count
 * ========================================================================== */

static void on_count(NcJob *job, void *data) {
  Req *rq = data;
  NcApp *app = rq->app;
  app_log_job(app, job, "count");
  View *v = app_find_view(app, rq->uid);
  if (!v) return;
  PageCache *pc = v->cache;
  if (rq->gen != page_cache_generation(pc)) return;
  v->count_pending = false;
  if (job->error || job->count < 0) {
    if (is_cancel(job)) {
      app_set_status(app, false, "Cancelled"); /* the total stays as it was */
    } else {
      page_cache_set_total(pc, 0, false, false); /* no total: the loaded rows are all that is known */
      app_set_status(app, true, "count: %s", job->error ? job->error : "no result");
    }
    v->jump_pending = false;
    return;
  }
  page_cache_set_total(pc, (size_t)job->count, true, job->count_approximate);
  size_t total = (size_t)job->count;
  if (total && v->cursor_row >= total) v->cursor_row = total - 1;
  else if (!total) v->cursor_row = 0;
  if (v->scroll_row >= total) v->scroll_row = total ? total - 1 : 0;
  grid_resolve_jump(app, v);
}

void app_view_count(NcApp *app, View *v) {
  ConnTab *t = app_view_tab(app, v);
  if (!t || !t->conn || nc_connection_closed(t->conn)) return;
  LaceDriver d = nc_connection_driver(t->conn);
  NcJob *job;
  if (v->kind == VIEW_TABLE) {
    char *where = view_where(v, d);
    job = nc_job_table_count(v->table, where, app->config->general.approximate_counts);
    free(where);
  } else {
    char *sql = view_count_sql(v, d);
    if (!sql) return;
    job = nc_job_count_sql(sql);
    free(sql);
  }
  v->count_pending = true;
  v->count_gen = page_cache_generation(v->cache);
  nc_connection_submit_full(t->conn, job, t->owner, on_count, req_new(app, v, 0), free);
}

/* ==========================================================================
 * Opening, reloading, cancelling
 * ========================================================================== */

void app_view_reload(NcApp *app, View *v) {
  if (v->kind == VIEW_QUERY) {
    if (!v->base_sql || v->running) return;
    if (!v->paged) { /* shown whole: ask again, only when that cannot write */
      ConnTab *t = app_view_tab(app, v);
      if (!t || !t->conn) return;
      if (!lace_sql_read_only(v->base_sql, (int)nc_connection_driver(t->conn))) {
        app_set_status(app, false, "Run the statement again to refresh it");
        return;
      }
      char *sql = str_dup(v->base_sql);
      app_query_run(app, v, sql);
      str_secure_free(sql);
      return;
    }
    v->fail_set = false;
    free(v->error);
    v->error = NULL;
    view_clear_selection(v);
    page_cache_reset_soft(v->cache);
    page_cache_request(v->cache, v->scroll_row);
    app_view_count(app, v);
    return;
  }
  if (!v->schema) { /* the schema never arrived: ask again */
    if (!v->loading) {
      free(v->schema_error);
      v->schema_error = NULL;
      load_schema(app, v);
    }
    return;
  }
  v->fail_set = false;
  free(v->error);
  v->error = NULL;
  view_clear_selection(v); /* selections are row positions of the replaced data */
  page_cache_reset_soft(v->cache);
  page_cache_request(v->cache, v->scroll_row);
  app_view_count(app, v);
}

void app_view_attach(NcApp *app, View *v) {
  if (v->fetch_ctx) return;
  Req *ctx = safe_calloc(1, sizeof *ctx);
  ctx->app = app;
  ctx->uid = v->uid;
  v->fetch_ctx = ctx;
  page_cache_set_fetch(v->cache, fetch_cb, ctx);
}

static View *open_table(NcApp *app, const char *table, bool preview) {
  ConnTab *t = app_current_tab(app);
  if (!t || !t->conn || !table) return NULL;
  t->sidebar_focused = false;
  size_t i = conntab_find_table_view(t, table);
  if (i != SIZE_MAX) {
    t->current = i;
    if (!preview) t->views[i]->preview = false; /* opened for keeps */
    return t->views[i];
  }
  View *v = view_new_table(table);
  v->preview = preview;
  size_t old = preview ? conntab_find_preview(t) : SIZE_MAX;
  if (old != SIZE_MAX) conntab_replace_view(t, old, v); /* the preview tab stays; what it shows changes */
  else conntab_add_view(t, v);
  app_view_attach(app, v);
  load_schema(app, v);
  return v;
}

View *app_open_table(NcApp *app, const char *table) { return open_table(app, table, false); }

View *app_preview_table(NcApp *app, const char *table) { return open_table(app, table, true); }

bool app_cancel_running(NcApp *app) {
  ConnTab *t = app_current_tab(app);
  if (!t || !t->conn || !nc_connection_busy(t->conn)) return false;
  /* A cancel would reach the COMMIT itself and leave it undecided: let it
   * finish (Esc is still consumed) */
  for (size_t i = 0; i < t->num_views; i++)
    if (t->views[i]->committing) {
      app_set_status(app, false, "COMMIT in progress");
      return true;
    }
  for (size_t i = 0; i < t->num_views; i++)
    if (t->views[i]->running) t->views[i]->cancel_run = true; /* a script stops, not only its statement */
  nc_connection_cancel(t->conn);
  return true;
}
