#include "edit_ops.h"
#include "model/workspace.h"
#include "ui/dialogs/confirm.h"
#include "ui/grid.h"
#include "util/json_helpers.h"
#include "util/mem.h"
#include "util/str.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A reason that outlives the call (the callers show it at once) */
static char g_why[512];
static const char *why_fmt(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_why, sizeof g_why, fmt, ap);
  va_end(ap);
  return g_why;
}

/* ==========================================================================
 * Deciding
 * ========================================================================== */

EditDecision edit_decide(const LaceValue *orig, LaceValueType col_type, const char *text,
                         bool set_null, bool set_empty, LaceValue *out, char **err) {
  memset(out, 0, sizeof *out);
  bool orig_null = !orig || orig->is_null || orig->type == LACE_TYPE_NULL;
  if (orig && orig->omitted) {
    if (err) *err = str_dup("The value is too large to edit");
    return EDIT_REFUSED;
  }
  if (set_null || !text) {
    if (orig_null) return EDIT_NOTHING;
    *out = lace_value_null();
    return EDIT_WRITE;
  }
  if (set_empty && orig_null) { /* the explicit action: '' even over NULL */
    return lace_value_from_edit_text("", col_type, out, err) ? EDIT_WRITE : EDIT_REFUSED;
  }
  bool unchanged = false;
  if (!lace_edit_value(orig, col_type, text, out, &unchanged, err)) return EDIT_REFUSED;
  if (unchanged) {
    lace_value_free(out);
    memset(out, 0, sizeof *out);
    return EDIT_NOTHING;
  }
  return EDIT_WRITE;
}

/* ==========================================================================
 * Context
 * ========================================================================== */

static bool has_primary_key(const LaceSchema *s) {
  for (size_t i = 0; s && i < s->num_columns; i++)
    if (s->columns[i].primary_key) return true;
  return false;
}

bool edit_context_for_view(NcApp *app, View *v, EditContext *out, const char **why) {
  memset(out, 0, sizeof *out);
  ConnTab *t = v ? app_view_tab(app, v) : NULL;
  if (!t || !t->conn || nc_connection_closed(t->conn)) {
    if (why) *why = "Not connected";
    return false;
  }
  if (v->kind == VIEW_TABLE) {
    if (!v->schema) {
      if (why) *why = "The table is still loading";
      return false;
    }
    out->table = v->table;
  } else {
    if (!v->source_table || !v->schema) {
      if (why) *why = "These results cannot be edited: they are not a single table's rows";
      return false;
    }
    out->table = v->source_table;
    out->unique_names = true;
    out->snapshot_all = true;
  }
  if (!has_primary_key(v->schema)) {
    if (why) *why = "The table has no primary key";
    return false;
  }
  out->app = app;
  out->conn = t->conn;
  out->view = v;
  return true;
}

static const LaceColumn *schema_column(const LaceSchema *s, const char *name) {
  for (size_t i = 0; s && name && i < s->num_columns; i++)
    if (s->columns[i].name && !strcmp(s->columns[i].name, name)) return &s->columns[i];
  return NULL;
}

/* ==========================================================================
 * Updating
 * ========================================================================== */

typedef struct {
  NcApp *app;
  uint64_t uid, gen;
  size_t row, col;
  LaceValue value;
} EditReq;

static void edit_req_free(void *p) {
  EditReq *rq = p;
  lace_value_free(&rq->value);
  free(rq);
}

static void on_update(NcJob *job, void *data) {
  EditReq *rq = data;
  NcApp *app = rq->app;
  app_log_job(app, job, "update");
  View *v = app_find_view(app, rq->uid);
  if (job->error) {
    if (job->cancelled || job->code == LACE_ERR_QUERY_CANCELLED) app_set_status(app, false, "Cancelled");
    else app_set_status(app, true, "Not updated: %s", job->error);
    return;
  }
  app_set_status(app, false, "Updated 1 row");
  if (!v) return;
  if (rq->gen == page_cache_generation(v->cache)) {
    page_cache_set_cell(v->cache, rq->row, rq->col, rq->value);
    rq->value = lace_value_null(); /* adopted */
    view_measure(v, rq->row, rq->row + 1);
  } else {
    app_view_reload(app, v); /* the pages moved on while the write ran */
  }
}

const char *edit_check_cell(EditContext *ctx, size_t row, size_t col) {
  View *v = ctx->view;
  PageCache *pc = v->cache;
  if (page_cache_stale(pc)) return "Loading\xe2\x80\xa6";
  const LaceValue *orig = page_cache_cell(pc, row, col);
  const LaceColumn *pcol = page_cache_column(pc, col);
  if (!orig || !pcol || !pcol->name) return "The row is not loaded yet";
  if (orig->omitted) return "The value is too large to edit";
  const LaceColumn *scol = schema_column(v->schema, pcol->name);
  if (!scol) return why_fmt("%s is not a column of the table", pcol->name);
  if (scol->generated) return why_fmt("%s is computed by the database", pcol->name);
  return NULL;
}

bool edit_cell(EditContext *ctx, size_t row, size_t col, const char *text, bool set_null,
               bool set_empty, uint64_t gen, const char **why) {
  View *v = ctx->view;
  PageCache *pc = v->cache;
  if (gen != page_cache_generation(pc)) {
    *why = "The data was reloaded, edit again";
    return false;
  }
  if ((*why = edit_check_cell(ctx, row, col))) return false;
  const LaceValue *orig = page_cache_cell(pc, row, col);
  const LaceColumn *pcol = page_cache_column(pc, col);
  const LaceColumn *scol = schema_column(v->schema, pcol->name);
  LaceValue out;
  char *err = NULL;
  EditDecision d = edit_decide(orig, scol->type, text, set_null, set_empty, &out, &err);
  if (d == EDIT_NOTHING) {
    *why = "No change";
    return false;
  }
  if (d == EDIT_REFUSED) {
    *why = why_fmt("Not updated: %s", err ? err : "invalid value");
    free(err);
    return false;
  }
  size_t in_page = 0;
  const LaceResult *res = page_cache_row_result(pc, row, &in_page);
  LaceRowKey key = {0};
  const char *kwhy = NULL;
  if (!res || lace_row_key_build(&key, res, in_page, v->schema, ctx->unique_names, &kwhy) != LACE_ROW_KEY_OK) {
    *why = why_fmt("Not updated: %s", kwhy ? kwhy : "the row cannot be addressed");
    lace_row_key_free(&key);
    lace_value_free(&out);
    return false;
  }
  LaceRowSnapshot snap = {0};
  lace_row_snapshot_build(&snap, res, in_page, ctx->snapshot_all ? SIZE_MAX : col, v->schema);
  NcJob *job = nc_job_update(ctx->table, &key, &snap, pcol->name, &out);
  lace_row_key_free(&key);
  lace_row_snapshot_free(&snap);
  EditReq *rq = safe_calloc(1, sizeof *rq);
  rq->app = ctx->app;
  rq->uid = v->uid;
  rq->gen = gen;
  rq->row = row;
  rq->col = col;
  rq->value = out;
  ConnTab *t = app_view_tab(ctx->app, v);
  nc_connection_submit_full(ctx->conn, job, t ? t->owner : NULL, on_update, rq, edit_req_free);
  return true;
}

bool edit_paste_cell(EditContext *ctx, size_t row, size_t col, const char *text, uint64_t gen,
                     const char **why) {
  if (!text) {
    *why = "The clipboard is empty";
    return false;
  }
  return edit_cell(ctx, row, col, text, false, false, gen, why);
}

/* ==========================================================================
 * Deleting
 * ========================================================================== */

typedef struct {
  NcApp *app;
  uint64_t uid;
  int refs;            /* the jobs and the submitting loop */
  size_t pending, deleted, failed, min_row;
  char *first_error;
} DelRun;

static void del_unref(void *p) {
  DelRun *d = p;
  if (--d->refs > 0) return;
  free(d->first_error);
  free(d);
}

static void del_finish(DelRun *d) {
  NcApp *app = d->app;
  View *v = app_find_view(app, d->uid);
  if (v && d->deleted) {
    page_cache_rows_removed(v->cache, d->min_row, d->deleted);
    view_clear_selection(v);
    v->jump_pending = false;
    view_set_cursor(v, v->cursor_row, v->cursor_col);
    app_view_reload(app, v);
  }
  if (d->failed && d->deleted)
    app_set_status(app, true, "Deleted %zu, %zu failed: %s", d->deleted, d->failed,
                   d->first_error ? d->first_error : "error");
  else if (d->failed)
    app_set_status(app, true, "Not deleted: %s", d->first_error ? d->first_error : "error");
  else
    app_set_status(app, false, "Deleted %zu row%s", d->deleted, d->deleted == 1 ? "" : "s");
}

static void on_delete(NcJob *job, void *data) {
  DelRun *d = data;
  app_log_job(d->app, job, "delete");
  if (job->error) {
    d->failed++;
    if (!d->first_error) d->first_error = str_dup(job->error);
  } else {
    d->deleted++;
  }
  if (--d->pending == 0) del_finish(d);
}

static void run_delete(NcApp *app, uint64_t uid, uint64_t gen, const size_t *rows, size_t n) {
  View *v = app_find_view(app, uid);
  EditContext ctx;
  const char *why = NULL;
  if (!v || !edit_context_for_view(app, v, &ctx, &why)) {
    app_set_status(app, true, "Not deleted: %s", why ? why : "the view is gone");
    return;
  }
  PageCache *pc = v->cache;
  if (page_cache_stale(pc)) {
    app_set_status(app, true, "Not deleted: Loading\xe2\x80\xa6");
    return;
  }
  if (gen != page_cache_generation(pc)) {
    app_set_status(app, true, "The data was reloaded, delete again");
    return;
  }
  ConnTab *t = app_view_tab(app, v);
  DelRun *d = safe_calloc(1, sizeof *d);
  d->app = app;
  d->uid = uid;
  d->refs = 1;
  d->min_row = SIZE_MAX;
  char *first_bad = NULL;
  size_t bad = 0;
  for (size_t i = 0; i < n; i++) {
    size_t in_page = 0;
    const LaceResult *res = page_cache_row_result(pc, rows[i], &in_page);
    LaceRowKey key = {0};
    const char *kwhy = NULL;
    if (!res || lace_row_key_build(&key, res, in_page, v->schema, ctx.unique_names, &kwhy) != LACE_ROW_KEY_OK) {
      bad++;
      if (!first_bad) first_bad = str_dup(res ? (kwhy ? kwhy : "the row cannot be addressed") : "the row is not loaded");
      lace_row_key_free(&key);
      continue;
    }
    LaceRowSnapshot snap = {0};
    lace_row_snapshot_build(&snap, res, in_page, SIZE_MAX, v->schema);
    NcJob *job = nc_job_delete(ctx.table, &key, &snap);
    lace_row_key_free(&key);
    lace_row_snapshot_free(&snap);
    if (rows[i] < d->min_row) d->min_row = rows[i];
    d->pending++;
    d->refs++;
    nc_connection_submit_full(ctx.conn, job, t ? t->owner : NULL, on_delete, d, del_unref);
  }
  if (bad) {
    d->failed += bad;
    d->first_error = first_bad;
  } else {
    free(first_bad);
  }
  if (d->pending == 0) del_finish(d);
  del_unref(d);
}

typedef struct {
  NcApp *app;
  uint64_t uid, gen;
  size_t *rows, n;
} DelAsk;

static void delete_answer(NcApp *app, bool yes, void *data) {
  DelAsk *a = data;
  if (yes) run_delete(app, a->uid, a->gen, a->rows, a->n);
  free(a->rows);
  free(a);
}

void edit_delete_rows(EditContext *ctx, const size_t *rows, size_t n, uint64_t gen) {
  if (!n) return;
  NcApp *app = ctx->app;
  if (page_cache_stale(ctx->view->cache)) {
    app_set_status(app, true, "Loading\xe2\x80\xa6");
    return;
  }
  if (!app->config->general.delete_confirmation) {
    run_delete(app, ctx->view->uid, gen, rows, n);
    return;
  }
  DelAsk *a = safe_calloc(1, sizeof *a);
  a->app = app;
  a->uid = ctx->view->uid;
  a->gen = gen;
  a->n = n;
  a->rows = safe_malloc(n * sizeof *a->rows);
  memcpy(a->rows, rows, n * sizeof *a->rows);
  char *msg = n == 1 ? str_printf("Delete row %zu of %s?", rows[0] + 1, ctx->table)
                     : str_printf("Delete %zu rows of %s?", n, ctx->table);
  confirm_open(app, "Delete", msg, "Delete", true, delete_answer, a);
  free(msg);
}

/* ==========================================================================
 * Clipboard
 * ========================================================================== */

char *edit_base64(const unsigned char *data, size_t n) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char *out = safe_malloc((n + 2) / 3 * 4 + 1);
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned)data[i] << 16;
    if (i + 1 < n) v |= (unsigned)data[i + 1] << 8;
    if (i + 2 < n) v |= data[i + 2];
    out[o++] = T[v >> 18 & 63];
    out[o++] = T[v >> 12 & 63];
    out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
    out[o++] = i + 2 < n ? T[v & 63] : '=';
  }
  out[o] = 0;
  return out;
}

#define OSC52_MAX 100000 /* terminals refuse more */

static void osc52_send(const char *text) {
  static int fd = -2;
  if (fd == -2) fd = open("/dev/tty", O_WRONLY | O_CLOEXEC);
  size_t n = strlen(text);
  if (fd < 0 || n > OSC52_MAX) return;
  char *b64 = edit_base64((const unsigned char *)text, n);
  char *seq = str_printf("\x1b]52;c;%s\x1b\\", b64);
  str_secure_free(b64);
  size_t len = strlen(seq), off = 0;
  while (off < len) {
    ssize_t w = write(fd, seq + off, len - off);
    if (w <= 0) break;
    off += (size_t)w;
  }
  str_secure_free(seq);
}

void app_clipboard_set(NcApp *app, const char *text) {
  str_secure_free(app->clipboard);
  app->clipboard = str_dup(text ? text : "");
  if (app->config && json_get_bool(config_section(app->config, "notcurses"), "clipboard_osc52", true))
    osc52_send(app->clipboard);
}

const char *app_clipboard_get(const NcApp *app) { return app->clipboard; }
