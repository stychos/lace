/*
 * Lace GTK frontend
 * Verified cell updates and row deletes
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "edit_ops.h"
#include "sql/row_key.h"
#include "sql/sql_builder.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  LaceEditContext *ctx;
  gsize row, col;
  LaceValue value;
  guint64 gen;
} EditReq;

static const char *readonly_reason(LaceEditContext *ctx) {
  const char *why = lace_data_grid_get_readonly_reason(ctx->grid);
  return why ? why : "this result cannot be edited";
}

static void update_done(LaceJob *job, gpointer data) {
  EditReq *req = data;
  LaceEditContext *ctx = req->ctx;
  if (job->error) {
    lace_error_dialog(ctx->widget, "Not updated", lace_job_error(job));
  } else if (req->gen == lace_row_model_get_generation(ctx->model)) {
    const LaceColumn *c = lace_row_model_get_column(ctx->model, req->col);
    lace_row_model_set_cell(ctx->model, req->row, req->col, req->value);
    req->value = lace_value_null();
    lace_toast(ctx->widget, "Updated %s", c && c->name ? c->name : "cell");
  }
}

static void edit_req_free(gpointer data) {
  EditReq *req = data;
  lace_value_free(&req->value);
  g_free(req);
}

void lace_edit_cell(LaceEditContext *ctx, gsize row, gsize col,
                    const char *text, gboolean set_null) {
  if (!lace_data_grid_get_editable(ctx->grid) || !ctx->schema || !ctx->table) {
    lace_toast(ctx->widget, "Read only: %s", readonly_reason(ctx));
    return;
  }
  gsize in_page;
  LaceResult *page = lace_row_model_row_result(ctx->model, row, &in_page);
  const LaceValue *orig = lace_row_model_get_cell(ctx->model, row, col);
  const LaceColumn *mc = lace_row_model_get_column(ctx->model, col);
  if (!page || !orig || !mc)
    return;
  if (orig->omitted) {
    lace_toast(ctx->widget, "This value is too large to load and cannot be edited");
    return;
  }
  LaceValueType type = mc->type;
  gboolean in_table = FALSE;
  for (size_t i = 0; i < ctx->schema->num_columns; i++) {
    if (ctx->schema->columns[i].name && mc->name &&
        strcmp(ctx->schema->columns[i].name, mc->name) == 0) {
      type = ctx->schema->columns[i].type;
      in_table = TRUE;
    }
  }
  if (!in_table) {
    lace_toast(ctx->widget, "%s is not a column of %s", mc->name ? mc->name : "This", ctx->table);
    return;
  }
  LaceValue value;
  bool unchanged;
  char *err = NULL;
  if (!sql_edit_value(orig, type, set_null ? NULL : text, &value, &unchanged, &err)) {
    lace_error_dialog(ctx->widget, "Not updated", err ? err : "invalid value");
    free(err);
    return;
  }
  if (unchanged) {
    lace_value_free(&value);
    return;
  }
  RowKey key;
  const char *why = NULL;
  if (row_key_build(&key, page, in_page, ctx->schema, ctx->unique_names, &why) != ROW_KEY_OK) {
    lace_error_dialog(ctx->widget, "Not updated", why ? why : "the row cannot be addressed");
    lace_value_free(&value);
    return;
  }
  RowSnapshot snap;
  row_snapshot_build(&snap, page, in_page, ctx->snapshot_all ? SIZE_MAX : col, ctx->schema);
  EditReq *req = g_new0(EditReq, 1);
  req->ctx = ctx;
  req->row = row;
  req->col = col;
  req->value = value;
  req->gen = lace_row_model_get_generation(ctx->model);
  LaceJob *job = lace_job_update(ctx->table, &key, &snap, mc->name, &value);
  row_key_free(&key);
  row_snapshot_free(&snap);
  lace_connection_submit_full(ctx->conn, job, ctx->owner, update_done, req, edit_req_free);
}

static void delete_done(LaceJob *job, gpointer data) {
  LaceEditContext *ctx = data;
  ctx->pending--;
  if (job->error) {
    if (!ctx->error)
      ctx->error = g_strdup(lace_job_error(job));
  } else {
    ctx->ok++;
  }
  if (ctx->pending > 0)
    return;
  if (ctx->ok > 0) {
    lace_data_grid_unselect_all(ctx->grid);
    lace_row_model_rows_removed(ctx->model, ctx->min_row, (gsize)ctx->ok);
    lace_toast(ctx->widget, "Deleted %d row%s", ctx->ok, ctx->ok == 1 ? "" : "s");
    if (ctx->after_delete)
      ctx->after_delete((gsize)ctx->ok, ctx->user_data);
  }
  if (ctx->error) {
    lace_error_dialog(ctx->widget, "Not deleted", ctx->error);
    g_clear_pointer(&ctx->error, g_free);
  }
}

static void delete_now(LaceEditContext *ctx, const gsize *rows, gsize n) {
  ctx->pending = 0;
  ctx->ok = 0;
  ctx->min_row = rows[0];
  g_clear_pointer(&ctx->error, g_free);
  for (gsize i = 0; i < n; i++) {
    gsize in_page;
    LaceResult *page = lace_row_model_row_result(ctx->model, rows[i], &in_page);
    if (!page)
      continue;
    RowKey key;
    const char *why = NULL;
    if (row_key_build(&key, page, in_page, ctx->schema, ctx->unique_names, &why) != ROW_KEY_OK) {
      if (!ctx->error)
        ctx->error = g_strdup(why ? why : "the row cannot be addressed");
      continue;
    }
    RowSnapshot snap;
    row_snapshot_build(&snap, page, in_page, SIZE_MAX, ctx->schema);
    LaceJob *job = lace_job_delete(ctx->table, &key, &snap);
    row_key_free(&key);
    row_snapshot_free(&snap);
    ctx->pending++;
    lace_connection_submit(ctx->conn, job, ctx->owner, delete_done, ctx);
  }
  if (ctx->pending == 0 && ctx->error) {
    lace_error_dialog(ctx->widget, "Not deleted", ctx->error);
    g_clear_pointer(&ctx->error, g_free);
  }
}

typedef struct {
  LaceEditContext *ctx;
  GObject *owner;
  gsize *rows;
  gsize n;
} DeleteReq;

static void delete_confirmed(gboolean ok, gpointer data) {
  DeleteReq *req = data;
  if (ok)
    delete_now(req->ctx, req->rows, req->n);
  g_object_unref(req->owner);
  g_free(req->rows);
  g_free(req);
}

void lace_edit_delete_selected(LaceEditContext *ctx) {
  if (!lace_data_grid_get_editable(ctx->grid) || !ctx->schema || !ctx->table) {
    lace_toast(ctx->widget, "Read only: %s", readonly_reason(ctx));
    return;
  }
  gsize n;
  gsize *rows = lace_data_grid_get_selected_rows(ctx->grid, &n);
  if (n == 0) {
    gsize row;
    if (!lace_data_grid_get_focus_cell(ctx->grid, &row, NULL)) {
      g_free(rows);
      return;
    }
    g_free(rows);
    rows = g_new(gsize, 1);
    rows[0] = row;
    n = 1;
  }
  if (!ctx->confirm_delete) {
    delete_now(ctx, rows, n);
    g_free(rows);
    return;
  }
  DeleteReq *req = g_new0(DeleteReq, 1);
  req->ctx = ctx;
  req->owner = g_object_ref(ctx->owner);
  req->rows = rows;
  req->n = n;
  char *body = g_strdup_printf("Delete %" G_GSIZE_FORMAT " row%s from %s? This cannot be undone.",
                               n, n == 1 ? "" : "s", ctx->table);
  lace_confirm(ctx->widget, "Delete rows", body, "_Delete", TRUE, delete_confirmed, req);
  g_free(body);
}

void lace_edit_context_clear(LaceEditContext *ctx) {
  g_clear_pointer(&ctx->error, g_free);
}
