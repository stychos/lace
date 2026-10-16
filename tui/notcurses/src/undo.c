#include "undo.h"
#include "conn/connection.h"
#include "conn/jobs.h"
#include "edit_ops.h"
#include "model/workspace.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  NcApp *app;
  uint64_t uid;
  int refs;
  size_t pending, done, failed;
  char *first_error;
  char *what;  /* "name back to old" for the status of a cell step */
  char *table; /* the table of a delete step: its dependents are looked at after */
  UndoKind kind;
} UndoRun;

static void run_unref(void *p) {
  UndoRun *u = p;
  if (--u->refs > 0) return;
  free(u->first_error);
  free(u->what);
  free(u->table);
  free(u);
}

/* Whether a referencing table's rows are changed by a delete of ours */
static bool rule_touches_rows(const char *on_delete) {
  if (!on_delete) return false;
  return !strcmp(on_delete, "CASCADE") || !strcmp(on_delete, "SET NULL") || !strcmp(on_delete, "SET DEFAULT");
}

/* The restore is done: say which referencing tables lost rows with the delete */
static void on_dependents(NcJob *job, void *data) {
  UndoRun *u = data;
  NcApp *app = u->app;
  app_log_job(app, job, "dependents");
  char *list = NULL;
  for (size_t i = 0; !job->error && i < job->num_deps; i++) {
    const LaceDependent *d = &job->deps[i];
    if (!rule_touches_rows(d->on_delete)) continue;
    if (list && strstr(list, d->table)) continue; /* two constraints of one table */
    char *n = str_printf("%s%s%s", list ? list : "", list ? ", " : "", d->table);
    free(list);
    list = n;
  }
  if (list) {
    app_set_status(app, true, "Restored %zu row%s; rows of %s the database removed or changed with them are not restored",
                   u->done, u->done == 1 ? "" : "s", list);
    free(list);
  }
}

static void run_finish(UndoRun *u) {
  NcApp *app = u->app;
  View *v = app_find_view(app, u->uid);
  if (u->failed)
    app_set_status(app, true, "Not undone: %s", u->first_error ? u->first_error : "error");
  else if (u->kind == UNDO_CELL)
    app_set_status(app, false, "Undone: %s", u->what);
  else
    app_set_status(app, false, "Restored %zu row%s", u->done, u->done == 1 ? "" : "s");
  if (!v || !u->done) return;
  app_view_reload(app, v); /* the rows come back, or the value, where the order puts them */
  if (u->kind == UNDO_DELETE && u->table) { /* what the delete took with it stays gone: say so */
    ConnTab *t = app_view_tab(app, v);
    if (t && t->conn) {
      u->refs++;
      nc_connection_submit_full(t->conn, nc_job_dependents(u->table), t->owner, on_dependents, u, run_unref);
    }
  }
}

static void on_done(NcJob *job, void *data) {
  UndoRun *u = data;
  app_log_job(u->app, job, "undo");
  if (job->error) {
    u->failed++;
    if (!u->first_error) u->first_error = str_dup(job->error);
  } else {
    u->done++;
  }
  if (--u->pending == 0) run_finish(u);
}

bool undo_last(NcApp *app, View *v) {
  UndoEntry *e = undo_stack_top(&v->undo);
  if (!e) {
    app_set_status(app, false, "Nothing to undo");
    return false;
  }
  EditContext ctx;
  const char *why = NULL;
  if (!edit_context_for_view(app, v, &ctx, &why)) {
    app_set_status(app, true, "Not undone: %s", why ? why : "the view is read-only");
    return false;
  }
  ConnTab *t = app_view_tab(app, v);
  if (e->kind == UNDO_DELETE && e->not_restorable) {
    app_set_status(app, true, "Cannot restore the deleted row%s: %s", e->num_rows == 1 ? "" : "s",
                   e->not_restorable);
    undo_stack_pop(&v->undo); /* the step is dropped: it can never succeed */
    return false;
  }
  UndoRun *u = safe_calloc(1, sizeof *u);
  u->app = app;
  u->uid = v->uid;
  u->refs = 1;
  u->kind = e->kind;
  u->table = str_dup(e->table);
  if (e->kind == UNDO_CELL) {
    /* A verified UPDATE back: the cell must still hold what the edit wrote */
    LaceRowSnapshot snap = {.names = &e->column, .values = &e->after, .count = 1};
    NcJob *job = nc_job_update(e->table, &e->key, &snap, e->column, &e->before);
    char *text = lace_value_to_edit_text(&e->before);
    u->what = str_printf("%s back to %s", e->column, e->before.is_null ? "NULL" : text ? text : "");
    str_secure_free(text);
    u->pending = 1;
    u->refs++;
    nc_connection_submit_full(ctx.conn, job, t ? t->owner : NULL, on_done, u, run_unref);
  } else {
    for (size_t i = 0; i < e->num_rows; i++) {
      const UndoRow *r = &e->rows[i];
      NcJob *job = nc_job_insert(e->table, (const char *const *)r->names, r->values, r->count);
      u->pending++;
      u->refs++;
      nc_connection_submit_full(ctx.conn, job, t ? t->owner : NULL, on_done, u, run_unref);
    }
  }
  undo_stack_pop(&v->undo); /* one chance: the step is consumed whether or not the write lands */
  if (u->pending == 0) run_finish(u);
  run_unref(u);
  return true;
}
