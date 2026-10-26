/*
 * Lace GTK frontend
 * LaceQueryView
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "query_view.h"
#include "ddl_helpers.h"
#include "core/catalog_rows.h"
#include "edit_ops.h"
#include "history_dialog.h"
#include <lace.h>
#include "util.h"
#include <gtksourceview/gtksource.h>
#include <stdlib.h>
#include <string.h>

/* A run of one or more statements */
typedef struct {
  GPtrArray *statements; /* char* */
  guint index;
  LaceRunMode mode;
  gboolean began;        /* BEGIN was sent */
  gboolean failed;
  gboolean cancelled;
  guint ran;
  double elapsed_ms;
} Script;

struct _LaceQueryView {
  GtkWidget parent_instance;
  LaceConnection *conn;
  const LacePrefs *prefs;

  GtkWidget *paned;
  GtkWidget *editor;      /* GtkSourceView */
  GtkSourceBuffer *buffer;
  GtkWidget *run_button, *run_all_button, *run_tx_button, *stop_button;
  GtkWidget *status_label;
  GtkWidget *banner;
  GtkWidget *stack;       /* empty / grid / message */
  GtkWidget *message_label;
  GtkWidget *footer_label;
  GtkWidget *footer_cell;
  GtkWidget *delete_button;
  LaceDataGrid *grid;
  LaceRowModel *model;

  Script *script;
  gboolean running;
  gboolean reload_tables; /* opened from "New view/function": the sidebar reloads once a statement ran */
  GtkWidget *box;         /* the lock banner over the paned */
  GtkWidget *lock_banner;
  guint lock_timer;

  /* The paged statement shown in the grid */
  char *base_sql;
  gboolean wrap;
  gboolean paged;
  gboolean count_pending;
  char *source_table;
  LaceSchema *schema;
  LaceEditContext edit;
  gulong style_handler;
};

enum { SIG_TITLE_CHANGED, SIG_OPEN_FILTERED, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef struct {
  LaceQueryView *view;
  guint64 gen;
  gsize page;
  gboolean first_of_run;
} PageReq;

G_DEFINE_TYPE(LaceQueryView, lace_query_view, GTK_TYPE_WIDGET)

static void run_next(LaceQueryView *v);
static void finish_script(LaceQueryView *v);
static void update_footer(LaceQueryView *v);
static void update_lock(LaceQueryView *v);
static void on_conn_busy(LaceConnection *c, LaceQueryView *v);

/* ==========================================================================
 * Status
 * ========================================================================== */

static void set_status(LaceQueryView *v, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_status(LaceQueryView *v, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *s = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  gtk_label_set_text(GTK_LABEL(v->status_label), s);
  g_free(s);
}

static void show_error(LaceQueryView *v, const char *msg) {
  adw_banner_set_title(ADW_BANNER(v->banner), msg ? msg : "Error");
  adw_banner_set_revealed(ADW_BANNER(v->banner), TRUE);
}

static void hide_error(LaceQueryView *v) {
  adw_banner_set_revealed(ADW_BANNER(v->banner), FALSE);
}

static void set_running(LaceQueryView *v, gboolean running) {
  v->running = running;
  gtk_widget_set_sensitive(v->run_button, !running);
  gtk_widget_set_sensitive(v->run_all_button, !running);
  gtk_widget_set_sensitive(v->run_tx_button, !running);
  gtk_widget_set_sensitive(v->stop_button, running);
}

static void update_footer(LaceQueryView *v) {
  if (lace_row_model_n_columns(v->model) == 0) {
    gtk_label_set_text(GTK_LABEL(v->footer_label), "");
    gtk_label_set_text(GTK_LABEL(v->footer_cell), "");
    gtk_widget_set_sensitive(v->delete_button, FALSE);
    return;
  }
  GString *s = g_string_new(NULL);
  gsize total = lace_row_model_get_total(v->model);
  char *n = lace_format_count(total);
  if (!lace_row_model_total_known(v->model))
    g_string_append_printf(s, "%s+ rows", n);
  else
    g_string_append_printf(s, "%s row%s", n, total == 1 ? "" : "s");
  g_free(n);
  gsize sel = lace_data_grid_get_selected_count(v->grid);
  if (sel > 0)
    g_string_append_printf(s, " · %" G_GSIZE_FORMAT " selected", sel);
  if (v->source_table && lace_data_grid_get_editable(v->grid))
    g_string_append_printf(s, " · editing %s", v->source_table);
  else if (lace_row_model_n_columns(v->model) > 0) {
    const char *why = lace_data_grid_get_readonly_reason(v->grid);
    g_string_append_printf(s, " · read only%s%s", why ? ": " : "", why ? why : "");
  }
  gtk_label_set_text(GTK_LABEL(v->footer_label), s->str);
  g_string_free(s, TRUE);
  gtk_widget_set_sensitive(v->delete_button, lace_data_grid_get_editable(v->grid) && sel > 0);
}

static void on_total_changed(LaceRowModel *m, LaceQueryView *v) {
  (void)m;
  update_footer(v);
}

static void on_selection_changed(GtkSelectionModel *m, guint p, guint n, LaceQueryView *v) {
  (void)m; (void)p; (void)n;
  update_footer(v);
}

static void on_cell_focused(LaceDataGrid *g, guint row, guint col, LaceQueryView *v) {
  (void)g;
  const LaceColumn *c = lace_row_model_get_column(v->model, col);
  const LaceValue *val = lace_row_model_get_cell(v->model, row, col);
  LaceValueType type = c ? c->type : LACE_TYPE_TEXT;
  if (val && !val->is_null && val->type != LACE_TYPE_NULL)
    type = val->type; /* Query columns carry no declared type */
  char *n = lace_format_count((gsize)row + 1);
  char *t = g_strdup_printf("Row %s · %s · %s", n, c && c->name ? c->name : "",
                            val && (val->is_null || val->type == LACE_TYPE_NULL) ? "NULL" : lace_type_short(type));
  gtk_label_set_text(GTK_LABEL(v->footer_cell), t);
  g_free(t);
  g_free(n);
}

/* ==========================================================================
 * Editor text
 * ========================================================================== */

char *lace_query_view_get_text(LaceQueryView *v) {
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(v->buffer), &s, &e);
  return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(v->buffer), &s, &e, TRUE);
}

void lace_query_view_set_text(LaceQueryView *v, const char *sql) {
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(v->buffer), sql ? sql : "", -1);
}

void lace_query_view_insert_text(LaceQueryView *v, const char *sql) {
  GtkTextBuffer *b = GTK_TEXT_BUFFER(v->buffer);
  GtkTextIter s, e;
  if (gtk_text_buffer_get_selection_bounds(b, &s, &e))
    gtk_text_buffer_delete(b, &s, &e);
  gtk_text_buffer_get_iter_at_mark(b, &s, gtk_text_buffer_get_insert(b));
  /* On its own line */
  if (!gtk_text_iter_starts_line(&s))
    gtk_text_buffer_insert(b, &s, "\n", 1);
  gtk_text_buffer_insert(b, &s, sql, -1);
  gtk_widget_grab_focus(v->editor);
}

void lace_query_view_focus_editor(LaceQueryView *v) { gtk_widget_grab_focus(v->editor); }

void lace_query_view_focus_results(LaceQueryView *v) {
  if (lace_row_model_get_total(v->model) > 0)
    lace_data_grid_focus_cell(v->grid, 0, 0);
}

char *lace_query_view_get_title(LaceQueryView *v) {
  char *text = lace_query_view_get_text(v);
  const char *p = text;
  while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')
    p++;
  if (!*p) {
    g_free(text);
    return g_strdup("Query");
  }
  GString *t = g_string_new(NULL);
  int words = 0;
  gboolean in_word = FALSE;
  for (; *p && *p != '\n' && t->len < 28; p++) {
    if (*p == ' ' || *p == '\t') {
      if (in_word) {
        words++;
        in_word = FALSE;
        if (words >= 4)
          break;
        g_string_append_c(t, ' ');
      }
      continue;
    }
    in_word = TRUE;
    g_string_append_c(t, *p);
  }
  g_free(text);
  return g_string_free(t, FALSE);
}

static void on_buffer_changed(GtkTextBuffer *b, LaceQueryView *v) {
  (void)b;
  g_signal_emit(v, signals[SIG_TITLE_CHANGED], 0);
}

static void apply_style_scheme(LaceQueryView *v) {
  gboolean dark = adw_style_manager_get_dark(adw_style_manager_get_default());
  GtkSourceStyleSchemeManager *mgr = gtk_source_style_scheme_manager_get_default();
  GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(mgr, dark ? "Adwaita-dark" : "Adwaita");
  if (scheme)
    gtk_source_buffer_set_style_scheme(v->buffer, scheme);
}

static void on_dark_changed(GObject *o, GParamSpec *p, LaceQueryView *v) {
  (void)o; (void)p;
  apply_style_scheme(v);
}

/* ==========================================================================
 * Results: paging and editability
 * ========================================================================== */

static void count_done(LaceJob *job, gpointer data) {
  LaceQueryView *v = data;
  v->count_pending = FALSE;
  if (GPOINTER_TO_SIZE(job->tag) != lace_row_model_get_generation(v->model))
    return;
  if (!job->error && job->count >= 0)
    lace_row_model_set_total(v->model, (gsize)job->count, TRUE, FALSE);
}

static void start_count(LaceQueryView *v) {
  if (v->count_pending || !v->base_sql)
    return;
  v->count_pending = TRUE;
  char *sql = lace_sql_count_of_statement(v->base_sql);
  LaceJob *job = lace_job_count_sql(sql);
  free(sql);
  job->tag = GSIZE_TO_POINTER(lace_row_model_get_generation(v->model));
  lace_connection_submit(v->conn, job, G_OBJECT(v), count_done, v);
}

static void set_editable_state(LaceQueryView *v) {
  gboolean editable = FALSE;
  const char *why = NULL;
  if (!v->source_table) {
    why = "not a single-table SELECT";
  } else if (!v->schema) {
    why = "the table's structure could not be read";
  } else {
    gboolean has_pk = FALSE;
    for (size_t i = 0; i < v->schema->num_columns; i++)
      if (v->schema->columns[i].primary_key)
        has_pk = TRUE;
    if (!has_pk) {
      why = "the table has no primary key";
    } else if (lace_row_model_get_total(v->model) > 0 && lace_row_model_row_loaded(v->model, 0)) {
      gsize in_page;
      LaceResult *page = lace_row_model_row_result(v->model, 0, &in_page);
      LaceRowKey key;
      const char *reason = NULL;
      LaceRowKeyStatus st = lace_row_key_build(&key, page, in_page, v->schema, TRUE, &reason);
      if (st == LACE_ROW_KEY_OK) {
        editable = TRUE;
        lace_row_key_free(&key);
      } else if (st == LACE_ROW_KEY_BAD) {
        editable = TRUE; /* Other rows may be fine; refused per row */
      } else {
        why = "every primary key column must appear once in the results";
      }
    } else {
      editable = TRUE;
    }
  }
  lace_data_grid_set_editable(v->grid, editable);
  lace_data_grid_set_readonly_reason(v->grid, why);
  v->edit.schema = v->schema;
  v->edit.table = v->source_table;
  update_footer(v);
}

static void schema_done(LaceJob *job, gpointer data) {
  LaceQueryView *v = data;
  if (GPOINTER_TO_SIZE(job->tag) != lace_row_model_get_generation(v->model))
    return;
  lace_schema_free(v->schema);
  v->schema = job->error ? NULL : lace_job_steal_schema(job);
  lace_data_grid_set_schema(v->grid, v->schema);
  lace_data_grid_set_fk_columns(v->grid, v->schema);
  if (v->schema)
    lace_data_grid_set_row_numbers(v->grid, lace_prefs_row_numbers_for(v->prefs, v->schema));
  set_editable_state(v);
}

static void detect_source_table(LaceQueryView *v, const char *sql) {
  g_clear_pointer(&v->source_table, g_free);
  lace_data_grid_set_fk_columns(v->grid, NULL);
  lace_data_grid_set_schema(v->grid, NULL);
  g_clear_pointer(&v->schema, lace_schema_free);
  char *t = lace_sql_simple_select_table(sql, lace_connection_get_driver(v->conn));
  if (t) {
    v->source_table = g_strdup(t);
    free(t);
    LaceJob *job = lace_job_schema(v->source_table);
    job->tag = GSIZE_TO_POINTER(lace_row_model_get_generation(v->model));
    lace_connection_submit(v->conn, job, G_OBJECT(v), schema_done, v);
  }
  set_editable_state(v);
}

static void show_whole_result(LaceQueryView *v, LaceResult *result, const char *sql, double ms);

static void page_done(LaceJob *job, gpointer data) {
  PageReq *req = data;
  LaceQueryView *v = req->view;
  gboolean current = req->gen == lace_row_model_get_generation(v->model);
  if (job->error) {
    lace_row_model_page_failed(v->model, req->gen, req->page);
    if (current && req->first_of_run && !job->cancelled && v->script) {
      /* The paged form was refused: run the statement as written */
      v->paged = FALSE;
      Script *s = v->script;
      const char *sql = g_ptr_array_index(s->statements, s->index - 1);
      LaceJob *whole = lace_job_exec(sql);
      whole->tag = GSIZE_TO_POINTER(1);
      whole->user_statement = TRUE;
      extern void lace_query_view_exec_done(LaceJob *job, gpointer data);
      lace_connection_submit(v->conn, whole, G_OBJECT(v), lace_query_view_exec_done, v);
    } else if (current && !job->cancelled) {
      show_error(v, lace_job_error(job));
      if (req->first_of_run && v->script) {
        v->script->failed = TRUE;
        finish_script(v);
      }
    } else if (current && job->cancelled && req->first_of_run && v->script) {
      v->script->cancelled = TRUE;
      finish_script(v);
    }
  } else {
    gboolean first = current && lace_row_model_n_columns(v->model) == 0;
    lace_row_model_page_loaded(v->model, req->gen, req->page, lace_job_steal_result(job));
    if (current) {
      if (first)
        lace_data_grid_measure_columns(v->grid);
      if (req->first_of_run) {
        gtk_stack_set_visible_child_name(GTK_STACK(v->stack), "grid");
        if (v->script) {
          Script *s = v->script;
          s->elapsed_ms += job->elapsed_ms;
          s->ran++;
          lace_connection_record_history(v->conn, g_ptr_array_index(s->statements, s->index - 1),
                                         LACE_HISTORY_QUERY);
          detect_source_table(v, v->base_sql);
          if (!lace_row_model_total_known(v->model))
            start_count(v);
          run_next(v);
        }
      } else if (!lace_row_model_total_known(v->model)) {
        start_count(v);
      }
      if (v->source_table && v->schema)
        set_editable_state(v);
    }
  }
  update_footer(v);
  lace_trim_heap();
}

static void fetch_page(LaceRowModel *m, gsize page, gpointer data) {
  LaceQueryView *v = data;
  if (!v->paged || !v->base_sql)
    return;
  gsize size = lace_row_model_get_page_size(m);
  char *sql = lace_sql_page_of_statement(v->base_sql, v->wrap, size, page * size);
  PageReq *req = g_new0(PageReq, 1);
  req->view = v;
  req->gen = lace_row_model_get_generation(m);
  req->page = page;
  req->first_of_run = FALSE;
  LaceJob *job = lace_job_read(sql);
  free(sql);
  lace_connection_submit_full(v->conn, job, G_OBJECT(v), page_done, req, g_free);
}

/* Show a paged statement: reset the model and ask for the first page */
static void start_paged(LaceQueryView *v, const char *base, gboolean wrap) {
  g_free(v->base_sql);
  v->base_sql = g_strdup(base);
  v->wrap = wrap;
  v->paged = TRUE;
  v->count_pending = FALSE;
  lace_row_model_reset(v->model);
  lace_row_model_set_page_size(v->model, (gsize)v->prefs->page_size);
  gsize size = lace_row_model_get_page_size(v->model);
  char *sql = lace_sql_page_of_statement(base, wrap, size, 0);
  PageReq *req = g_new0(PageReq, 1);
  req->view = v;
  req->gen = lace_row_model_get_generation(v->model);
  req->page = 0;
  req->first_of_run = TRUE;
  LaceJob *job = lace_job_read(sql);
  job->user_statement = TRUE; /* the statement itself: the lock banner */
  free(sql);
  /* Mark the page pending so the model does not ask again (a request
   * would fetch it a second time: the cancel would then hit that copy) */
  lace_row_model_mark_pending(v->model, 0);
  lace_connection_submit_full(v->conn, job, G_OBJECT(v), page_done, req, g_free);
}

static void show_whole_result(LaceQueryView *v, LaceResult *result, const char *sql, double ms) {
  (void)sql;
  v->paged = FALSE;
  g_clear_pointer(&v->base_sql, g_free);
  lace_row_model_reset(v->model);
  if (result && result->num_columns > 0) {
    gsize n = result->num_rows;
    lace_row_model_set_page_size(v->model, n > 0 ? n : 1);
    gboolean truncated = result->truncated;
    lace_row_model_page_loaded(v->model, lace_row_model_get_generation(v->model), 0, result);
    lace_row_model_set_total(v->model, n, TRUE, FALSE);
    lace_data_grid_measure_columns(v->grid);
    gtk_stack_set_visible_child_name(GTK_STACK(v->stack), "grid");
    char *count = lace_format_count(n);
    char *el = lace_format_elapsed(ms);
    set_status(v, "%s row%s%s · %s", count, n == 1 ? "" : "s",
               truncated ? " (truncated by the daemon's limit)" : "", el);
    g_free(count);
    g_free(el);
    detect_source_table(v, sql);
  } else {
    int64_t affected = result ? result->rows_affected : -1;
    lace_result_free(result);
    char *el = lace_format_elapsed(ms);
    char *msg = affected >= 0
                    ? g_strdup_printf("%" G_GINT64_FORMAT " row%s affected · %s", affected,
                                      affected == 1 ? "" : "s", el)
                    : g_strdup_printf("Done · %s", el);
    gtk_label_set_text(GTK_LABEL(v->message_label), msg);
    set_status(v, "%s", msg);
    g_free(msg);
    g_free(el);
    gtk_stack_set_visible_child_name(GTK_STACK(v->stack), "message");
    g_clear_pointer(&v->source_table, g_free);
    lace_data_grid_set_editable(v->grid, FALSE);
  }
  update_footer(v);
}

/* ==========================================================================
 * Running
 * ========================================================================== */

static void script_free(Script *s) {
  if (!s)
    return;
  g_ptr_array_unref(s->statements);
  g_free(s);
}

static void finish_script(LaceQueryView *v) {
  Script *s = v->script;
  if (!s)
    return;
  char *el = lace_format_elapsed(s->elapsed_ms);
  if (s->cancelled)
    set_status(v, "Cancelled after %u statement%s · %s", s->ran, s->ran == 1 ? "" : "s", el);
  else if (s->failed)
    set_status(v, "Failed after %u statement%s · %s", s->ran, s->ran == 1 ? "" : "s", el);
  else if (s->statements->len > 1)
    set_status(v, "%u statements · %s", s->ran, el);
  else if (v->paged)
    set_status(v, "First page in %s", el);
  g_free(el);
  gboolean ran = s->ran > 0 && !s->failed;
  v->script = NULL;
  script_free(s);
  if (ran && v->reload_tables && !lace_connection_is_closed(v->conn))
    lace_connection_refresh_tables(v->conn); /* the statement may have created something */
  set_running(v, FALSE);
}

static void control_done(LaceJob *job, gpointer data);

/* Send BEGIN / COMMIT / ROLLBACK */
static void send_control(LaceQueryView *v, const char *sql) {
  LaceJob *job = lace_job_exec(sql);
  job->record = FALSE;
  lace_connection_submit(v->conn, job, G_OBJECT(v), control_done, v);
}

static void control_done(LaceJob *job, gpointer data) {
  LaceQueryView *v = data;
  Script *s = v->script;
  if (!s)
    return;
  const char *sql = job->sql ? job->sql : "";
  if (g_str_has_prefix(sql, "BEGIN")) {
    if (job->error) {
      show_error(v, lace_job_error(job));
      s->failed = TRUE;
      finish_script(v);
      return;
    }
    s->began = TRUE;
    run_next(v);
    return;
  }
  if (g_str_has_prefix(sql, "COMMIT")) {
    if (job->error) {
      show_error(v, lace_job_error(job));
      s->failed = TRUE;
    } else {
      lace_toast(GTK_WIDGET(v), "Committed %u statement%s", s->ran, s->ran == 1 ? "" : "s");
    }
    finish_script(v);
    return;
  }
  /* ROLLBACK */
  if (!job->error)
    lace_toast(GTK_WIDGET(v), "Rolled back");
  finish_script(v);
}

void lace_query_view_exec_done(LaceJob *job, gpointer data) {
  LaceQueryView *v = data;
  Script *s = v->script;
  if (!s)
    return;
  gboolean show = GPOINTER_TO_INT(job->tag) == 1 || s->index == s->statements->len;
  s->elapsed_ms += job->elapsed_ms;
  if (job->error) {
    show_error(v, lace_job_error(job));
    if (job->cancelled)
      s->cancelled = TRUE;
    else
      s->failed = TRUE;
    if (s->mode == LACE_RUN_TRANSACTION && s->began) {
      send_control(v, "ROLLBACK");
      return;
    }
    finish_script(v);
    return;
  }
  s->ran++;
  if (show)
    show_whole_result(v, lace_job_steal_result(job), job->sql, job->elapsed_ms);
  run_next(v);
}

static void run_next(LaceQueryView *v) {
  Script *s = v->script;
  if (!s)
    return;
  if (s->index >= s->statements->len) {
    if (s->mode == LACE_RUN_TRANSACTION && s->began) {
      send_control(v, "COMMIT");
      return;
    }
    finish_script(v);
    return;
  }
  const char *sql = g_ptr_array_index(s->statements, s->index++);
  gboolean last = s->index == s->statements->len;
  LaceSqlShape shape;
  lace_sql_shape(sql, lace_connection_get_driver(v->conn), &shape);
  /* Only the last statement's rows are shown, and only a plain read is
   * paged; everything else runs once, as written */
  if (last && lace_sql_shape_pageable(&shape)) {
    char *base = g_strndup(sql, shape.end);
    start_paged(v, base, lace_sql_shape_needs_wrap(&shape));
    g_free(base);
    return;
  }
  LaceJob *job = lace_job_exec(sql);
  job->tag = GSIZE_TO_POINTER(0);
  job->user_statement = TRUE;
  lace_connection_submit(v->conn, job, G_OBJECT(v), lace_query_view_exec_done, v);
}

void lace_query_view_run(LaceQueryView *v, LaceRunMode mode) {
  if (v->running) {
    lace_toast(GTK_WIDGET(v), "A statement is running (Esc stops it)");
    return;
  }
  GtkTextBuffer *b = GTK_TEXT_BUFFER(v->buffer);
  char *text = lace_query_view_get_text(v);
  LaceDriver driver = lace_connection_get_driver(v->conn);
  GPtrArray *stmts = g_ptr_array_new_with_free_func(g_free);
  GtkTextIter s, e;
  if (mode == LACE_RUN_AT_CURSOR && gtk_text_buffer_get_selection_bounds(b, &s, &e)) {
    /* A selection runs as one statement */
    char *sel = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    g_strstrip(sel);
    if (*sel)
      g_ptr_array_add(stmts, sel);
    else
      g_free(sel);
  } else if (mode == LACE_RUN_AT_CURSOR) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
    gsize cursor = (gsize)gtk_text_iter_get_offset(&it);
    /* Offsets are characters; convert to bytes */
    char *prefix = g_utf8_substring(text, 0, (glong)cursor);
    gsize byte_cursor = strlen(prefix);
    g_free(prefix);
    gsize start, end;
    if (lace_sql_statement_at(text, byte_cursor, driver, &start, &end))
      g_ptr_array_add(stmts, g_strndup(text + start, end - start));
  } else {
    gsize start, end, next = 0;
    while (lace_sql_next_statement(text, next, driver, &start, &end, &next))
      g_ptr_array_add(stmts, g_strndup(text + start, end - start));
  }
  g_free(text);
  if (stmts->len == 0) {
    g_ptr_array_unref(stmts);
    lace_toast(GTK_WIDGET(v), "Nothing to run");
    return;
  }
  if (mode == LACE_RUN_TRANSACTION) {
    LaceTxState tx = lace_connection_get_tx_state(v->conn);
    if (tx != LACE_TX_NONE) {
      g_ptr_array_unref(stmts);
      lace_toast(GTK_WIDGET(v), "A transaction is already open; COMMIT or ROLLBACK it first");
      return;
    }
  }
  hide_error(v);
  Script *sc = g_new0(Script, 1);
  sc->statements = stmts;
  sc->mode = mode;
  v->script = sc;
  set_running(v, TRUE);
  set_status(v, "Running…");
  if (mode == LACE_RUN_TRANSACTION)
    send_control(v, "BEGIN");
  else
    run_next(v);
}

void lace_query_view_cancel(LaceQueryView *v) {
  if (!v->running)
    return;
  lace_connection_cancel(v->conn);
  set_status(v, "Cancelling…");
}

gboolean lace_query_view_is_running(LaceQueryView *v) { return v->running; }

static void use_history(const char *sql, gpointer data) {
  lace_query_view_insert_text(LACE_QUERY_VIEW(data), sql);
}

void lace_query_view_show_history(LaceQueryView *v) {
  lace_history_dialog_show(GTK_WIDGET(v), v->conn, use_history, v);
}

static void goto_row_cb(const char *text, gpointer data) {
  LaceQueryView *v = data;
  if (text) {
    long long n = strtoll(text, NULL, 10);
    if (n >= 1) {
      gsize total = lace_row_model_get_total(v->model);
      gsize row = (gsize)n - 1;
      if (total > 0 && row >= total)
        row = total - 1;
      lace_data_grid_scroll_to_row(v->grid, row);
    }
  }
  g_object_unref(v);
}

void lace_query_view_goto_row(LaceQueryView *v) {
  if (lace_row_model_get_total(v->model) == 0)
    return;
  lace_prompt(GTK_WIDGET(v), "Go to row", "Row number", NULL, "_Go", FALSE, goto_row_cb, g_object_ref(v));
}

void lace_query_view_delete_selected(LaceQueryView *v) {
  lace_edit_delete_selected(&v->edit);
}

void lace_query_view_refresh(LaceQueryView *v) {
  if (v->paged && v->base_sql && !v->running) {
    lace_row_model_invalidate(v->model);
    start_count(v);
  }
}

static void on_edit_cell(LaceDataGrid *g, guint row, guint col, const char *text,
                         gboolean set_null, LaceQueryView *v) {
  (void)g;
  lace_edit_cell(&v->edit, row, col, text, set_null);
}

static void on_delete_rows(LaceDataGrid *g, LaceQueryView *v) {
  (void)g;
  lace_query_view_delete_selected(v);
}

static void on_grid_refresh(LaceDataGrid *g, LaceQueryView *v) {
  (void)g;
  lace_query_view_refresh(v);
}

static void after_delete(gsize deleted, gpointer data) {
  (void)deleted;
  update_footer(LACE_QUERY_VIEW(data));
}

static gboolean on_editor_key(GtkEventControllerKey *c, guint keyval, guint code,
                              GdkModifierType state, LaceQueryView *v) {
  (void)c; (void)code;
  gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
  gboolean shift = (state & GDK_SHIFT_MASK) != 0;
  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && ctrl) {
    lace_query_view_run(v, shift ? LACE_RUN_ALL : LACE_RUN_AT_CURSOR);
    return TRUE;
  }
  if (keyval == GDK_KEY_Escape && v->running) {
    lace_query_view_cancel(v);
    return TRUE;
  }
  return FALSE;
}

/* ==========================================================================
 * Following foreign keys from the results (one table's rows)
 * ========================================================================== */

static char **result_values(LaceQueryView *v, char *const *columns, size_t n) {
  gsize row = 0, col = 0;
  if (!lace_data_grid_get_focus_cell(v->grid, &row, &col)) {
    lace_toast(GTK_WIDGET(v), "No row to follow");
    return NULL;
  }
  char **vals = g_new0(char *, n + 1);
  for (size_t k = 0; k < n; k++) {
    gsize ci = (gsize)-1;
    for (gsize c = 0; c < lace_row_model_n_columns(v->model); c++) {
      const LaceColumn *mc = lace_row_model_get_column(v->model, c);
      if (mc && mc->name && columns[k] && g_str_equal(mc->name, columns[k])) {
        ci = c;
        break;
      }
    }
    const LaceValue *cell = ci == (gsize)-1 ? NULL : lace_row_model_get_cell(v->model, row, ci);
    const char *why = ci == (gsize)-1 ? "is not in the results" : !cell ? "is not loaded yet"
                      : cell->is_null ? "is NULL: nothing to follow" : cell->omitted ? "is too large to load" : NULL;
    if (why) {
      lace_toast(GTK_WIDGET(v), "%s %s", columns[k], why);
      g_strfreev(vals);
      return NULL;
    }
    char *t = lace_value_to_edit_text(cell);
    vals[k] = g_strdup(t ? t : "");
    free(t);
  }
  return vals;
}

static void emit_filtered(LaceQueryView *v, const char *table, char *const *columns, char **values, size_t n) {
  LaceFilter *filters = g_new0(LaceFilter, n + 1);
  for (size_t k = 0; k < n; k++) {
    filters[k].column = g_strdup(columns[k]);
    filters[k].op = LACE_FILTER_EQ;
    filters[k].value = g_strdup(values[k]);
  }
  g_signal_emit(v, signals[SIG_OPEN_FILTERED], 0, table, filters, (guint)n);
  for (size_t k = 0; k < n; k++) {
    g_free(filters[k].column);
    g_free(filters[k].value);
  }
  g_free(filters);
}

static void follow_key(LaceQueryView *v, const LaceForeignKey *fk) {
  if (!fk->ref_table || fk->num_ref_columns != fk->num_columns) {
    lace_toast(GTK_WIDGET(v), "The foreign key %s is incomplete", fk->name ? fk->name : "");
    return;
  }
  char **vals = result_values(v, fk->columns, fk->num_columns);
  if (!vals)
    return;
  emit_filtered(v, fk->ref_table, fk->ref_columns, vals, fk->num_columns);
  g_strfreev(vals);
}

static void act_follow_key(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a;
  LaceQueryView *v = data;
  guint64 i = g_variant_get_uint64(p);
  if (v->schema && i < v->schema->num_foreign_keys)
    follow_key(v, &v->schema->foreign_keys[i]);
}

static void popup_choices(LaceQueryView *v, GMenu *menu) {
  GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  gtk_widget_set_parent(pop, GTK_WIDGET(v->grid));
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
  g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
  gtk_popover_popup(GTK_POPOVER(pop));
}

static void on_follow_fk(LaceDataGrid *g, LaceQueryView *v) {
  (void)g;
  if (!v->source_table || !v->schema) {
    lace_toast(GTK_WIDGET(v), "Foreign keys are followed from a table's rows");
    return;
  }
  gsize row = 0, col = 0;
  if (!lace_data_grid_get_focus_cell(v->grid, &row, &col)) {
    lace_toast(GTK_WIDGET(v), "No row to follow");
    return;
  }
  const LaceColumn *c = lace_row_model_get_column(v->model, col);
  const char *name = c && c->name ? c->name : NULL;
  size_t matches[16];
  size_t n = 0;
  for (size_t i = 0; name && i < v->schema->num_foreign_keys && n < 16; i++) {
    const LaceForeignKey *fk = &v->schema->foreign_keys[i];
    if (fk->num_columns && fk->columns[0] && g_str_equal(fk->columns[0], name))
      matches[n++] = i;
  }
  if (!n) {
    lace_toast(GTK_WIDGET(v), "%s is not a foreign key column", name ? name : "This column");
    return;
  }
  if (n == 1) {
    follow_key(v, &v->schema->foreign_keys[matches[0]]);
    return;
  }
  GMenu *menu = g_menu_new();
  for (size_t k = 0; k < n; k++) {
    const LaceForeignKey *fk = &v->schema->foreign_keys[matches[k]];
    char *cols = catalog_join(fk->ref_columns, fk->num_ref_columns);
    char *title = g_strdup_printf("%s (%s)", fk->ref_table ? fk->ref_table : "?", cols);
    GMenuItem *it = g_menu_item_new(title, NULL);
    g_menu_item_set_action_and_target(it, "query.follow-key", "t", (guint64)matches[k]);
    g_menu_append_item(menu, it);
    g_object_unref(it);
    g_free(title);
    free(cols);
  }
  popup_choices(v, menu);
  g_object_unref(menu);
}

typedef struct { LaceDependent *deps; size_t n; } QueryDeps;

static void query_deps_free(gpointer data) {
  QueryDeps *d = data;
  if (d) {
    lace_dependents_free(d->deps, d->n);
    g_free(d);
  }
}

static void act_follow_dep(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a;
  LaceQueryView *v = data;
  QueryDeps *qd = g_object_get_data(G_OBJECT(v), "deps");
  guint64 i = g_variant_get_uint64(p);
  if (!qd || i >= qd->n)
    return;
  const LaceDependent *d = &qd->deps[i];
  char **vals = result_values(v, d->ref_columns, d->num_columns);
  if (!vals)
    return;
  emit_filtered(v, d->table, d->columns, vals, d->num_columns);
  g_strfreev(vals);
}

static void deps_done(LaceJob *job, gpointer data) {
  LaceQueryView *v = data;
  if (job->error) {
    lace_toast(GTK_WIDGET(v), "%s", lace_job_error(job));
    return;
  }
  if (!job->num_deps) {
    lace_toast(GTK_WIDGET(v), "No table references %s", job->table ? job->table : "");
    return;
  }
  QueryDeps *qd = g_new0(QueryDeps, 1);
  qd->deps = job->deps;
  qd->n = job->num_deps;
  job->deps = NULL;
  job->num_deps = 0;
  g_object_set_data_full(G_OBJECT(v), "deps", qd, query_deps_free);
  GMenu *menu = g_menu_new();
  for (size_t i = 0; i < qd->n; i++) {
    const LaceDependent *d = &qd->deps[i];
    char *cols = catalog_join(d->columns, d->num_columns);
    char *title = g_strdup_printf("%s (%s)%s%s", d->table ? d->table : "?", cols, d->on_delete ? "  ON DELETE " : "",
                                  d->on_delete ? d->on_delete : "");
    GMenuItem *it = g_menu_item_new(title, NULL);
    g_menu_item_set_action_and_target(it, "query.follow-dep", "t", (guint64)i);
    g_menu_append_item(menu, it);
    g_object_unref(it);
    g_free(title);
    free(cols);
  }
  popup_choices(v, menu);
  g_object_unref(menu);
}

static void on_referenced_by(LaceDataGrid *g, LaceQueryView *v) {
  (void)g;
  gsize row = 0, col = 0;
  if (!v->source_table || !lace_data_grid_get_focus_cell(v->grid, &row, &col)) {
    lace_toast(GTK_WIDGET(v), "Foreign keys are followed from a table's rows");
    return;
  }
  lace_connection_submit(v->conn, lace_job_dependents(v->source_table), G_OBJECT(v), deps_done, v);
}

static void act_query(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)p;
  LaceQueryView *v = data;
  const char *name = g_action_get_name(G_ACTION(a));
  if (g_str_equal(name, "run")) lace_query_view_run(v, LACE_RUN_AT_CURSOR);
  else if (g_str_equal(name, "run-all")) lace_query_view_run(v, LACE_RUN_ALL);
  else if (g_str_equal(name, "run-tx")) lace_query_view_run(v, LACE_RUN_TRANSACTION);
  else if (g_str_equal(name, "stop")) lace_query_view_cancel(v);
  else if (g_str_equal(name, "history")) lace_query_view_show_history(v);
  else if (g_str_equal(name, "goto-row")) lace_query_view_goto_row(v);
  else if (g_str_equal(name, "delete-rows")) lace_query_view_delete_selected(v);
  else if (g_str_equal(name, "refresh")) lace_query_view_refresh(v);
}

/* ==========================================================================
 * Object
 * ========================================================================== */

static void lace_query_view_dispose(GObject *obj) {
  LaceQueryView *v = LACE_QUERY_VIEW(obj);
  if (v->style_handler) {
    g_signal_handler_disconnect(adw_style_manager_get_default(), v->style_handler);
    v->style_handler = 0;
  }
  if (v->grid) {
    lace_data_grid_set_schema(v->grid, NULL);
    lace_data_grid_set_model(v->grid, NULL);
  }
  if (v->lock_timer) {
    g_source_remove(v->lock_timer);
    v->lock_timer = 0;
  }
  if (v->conn)
    g_signal_handlers_disconnect_by_func(v->conn, on_conn_busy, v);
  g_clear_pointer(&v->box, gtk_widget_unparent);
  v->paned = NULL;
  v->grid = NULL;
  g_clear_object(&v->model);
  g_clear_object(&v->buffer);
  g_clear_object(&v->conn);
  g_clear_pointer(&v->schema, lace_schema_free);
  g_clear_pointer(&v->base_sql, g_free);
  g_clear_pointer(&v->source_table, g_free);
  if (v->script) {
    script_free(v->script);
    v->script = NULL;
  }
  lace_edit_context_clear(&v->edit);
  G_OBJECT_CLASS(lace_query_view_parent_class)->dispose(obj);
}

static void lace_query_view_class_init(LaceQueryViewClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = lace_query_view_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "lace-queryview");
  signals[SIG_OPEN_FILTERED] = g_signal_new("open-filtered", LACE_TYPE_QUERY_VIEW,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_UINT);
  signals[SIG_TITLE_CHANGED] = g_signal_new("title-changed", LACE_TYPE_QUERY_VIEW,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void lace_query_view_init(LaceQueryView *v) { (void)v; }

/* ---- the lock banner: a statement the user asked for runs on the connection ---- */

static gboolean lock_tick(gpointer data) {
  update_lock(LACE_QUERY_VIEW(data));
  return G_SOURCE_CONTINUE;
}

static void update_lock(LaceQueryView *v) {
  const char *what = NULL;
  gint64 since = 0;
  gboolean locked = v->conn && lace_connection_user_busy(v->conn, &what, &since);
  if (locked) {
    char *text = lace_ddl_banner_text(what, (g_get_monotonic_time() - since) / 1000, 60);
    adw_banner_set_title(ADW_BANNER(v->lock_banner), text);
    g_free(text);
    if (!v->lock_timer)
      v->lock_timer = g_timeout_add(1000, lock_tick, v);
  } else if (v->lock_timer) {
    g_source_remove(v->lock_timer);
    v->lock_timer = 0;
  }
  adw_banner_set_revealed(ADW_BANNER(v->lock_banner), locked);
  gtk_widget_set_sensitive(v->paned, !locked);
}

static void on_conn_busy(LaceConnection *c, LaceQueryView *v) {
  (void)c;
  update_lock(v);
}

static void on_lock_cancel(AdwBanner *b, LaceQueryView *v) {
  (void)b;
  if (v->conn)
    lace_connection_cancel(v->conn);
}

void lace_query_view_set_reload_tables(LaceQueryView *v, gboolean reload) { v->reload_tables = reload; }

const char *lace_query_view_get_status_text(LaceQueryView *v) { return gtk_label_get_text(GTK_LABEL(v->status_label)); }

gboolean lace_query_view_is_locked(LaceQueryView *v) {
  return v->conn && lace_connection_user_busy(v->conn, NULL, NULL);
}

LaceQueryView *lace_query_view_new(LaceConnection *conn, const LacePrefs *prefs) {
  LaceQueryView *v = g_object_new(LACE_TYPE_QUERY_VIEW, NULL);
  v->conn = g_object_ref(conn);
  v->prefs = prefs;

  v->paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_wide_handle(GTK_PANED(v->paned), FALSE);
  v->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(v->box, GTK_WIDGET(v));
  v->lock_banner = adw_banner_new("");
  adw_banner_set_button_label(ADW_BANNER(v->lock_banner), "Cancel");
  gtk_widget_add_css_class(v->lock_banner, "lock-banner");
  g_signal_connect(v->lock_banner, "button-clicked", G_CALLBACK(on_lock_cancel), v);
  gtk_box_append(GTK_BOX(v->box), v->lock_banner);
  gtk_widget_set_vexpand(v->paned, TRUE);
  gtk_box_append(GTK_BOX(v->box), v->paned);
  g_signal_connect(conn, "busy-changed", G_CALLBACK(on_conn_busy), v);

  /* Editor half */
  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class(toolbar, "toolbar");
  gtk_widget_add_css_class(toolbar, "query-toolbar");
  v->run_button = lace_icon_button("media-playback-start-symbolic", "Run the statement at the cursor (Ctrl+Enter)", "query.run");
  v->run_all_button = lace_icon_button("media-skip-forward-symbolic", "Run every statement (Ctrl+Shift+Enter)", "query.run-all");
  v->run_tx_button = lace_icon_button("emblem-ok-symbolic", "Run everything in one transaction (Ctrl+Shift+T)", "query.run-tx");
  v->stop_button = lace_icon_button("media-playback-stop-symbolic", "Stop (Esc)", "query.stop");
  gtk_widget_set_sensitive(v->stop_button, FALSE);
  gtk_box_append(GTK_BOX(toolbar), v->run_button);
  gtk_box_append(GTK_BOX(toolbar), v->run_all_button);
  gtk_box_append(GTK_BOX(toolbar), v->run_tx_button);
  gtk_box_append(GTK_BOX(toolbar), v->stop_button);
  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_margin_start(sep, 4);
  gtk_widget_set_margin_end(sep, 4);
  gtk_box_append(GTK_BOX(toolbar), sep);
  v->status_label = gtk_label_new("Ctrl+Enter runs the statement at the cursor");
  gtk_widget_add_css_class(v->status_label, "dim-label");
  gtk_widget_add_css_class(v->status_label, "caption");
  gtk_label_set_ellipsize(GTK_LABEL(v->status_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(v->status_label), 0);
  gtk_widget_set_hexpand(v->status_label, TRUE);
  gtk_box_append(GTK_BOX(toolbar), v->status_label);
  gtk_box_append(GTK_BOX(toolbar), lace_icon_button("document-open-recent-symbolic", "Query history (Ctrl+H)", "query.history"));
  gtk_box_append(GTK_BOX(top), toolbar);

  v->buffer = gtk_source_buffer_new(NULL);
  GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
  GtkSourceLanguage *lang = gtk_source_language_manager_get_language(lm, "sql");
  if (lang)
    gtk_source_buffer_set_language(v->buffer, lang);
  gtk_source_buffer_set_highlight_syntax(v->buffer, TRUE);
  gtk_source_buffer_set_highlight_matching_brackets(v->buffer, TRUE);
  apply_style_scheme(v);
  v->style_handler = g_signal_connect(adw_style_manager_get_default(), "notify::dark",
                                      G_CALLBACK(on_dark_changed), v);
  v->editor = gtk_source_view_new_with_buffer(v->buffer);
  gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(v->editor), TRUE);
  gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(v->editor), TRUE);
  gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(v->editor), 4);
  gtk_source_view_set_insert_spaces_instead_of_tabs(GTK_SOURCE_VIEW(v->editor), TRUE);
  gtk_source_view_set_auto_indent(GTK_SOURCE_VIEW(v->editor), TRUE);
  gtk_source_view_set_smart_backspace(GTK_SOURCE_VIEW(v->editor), TRUE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(v->editor), TRUE);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(v->editor), 6);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(v->editor), 6);
  gtk_widget_add_css_class(v->editor, "sql-editor");
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_editor_key), v);
  gtk_widget_add_controller(v->editor, keys);
  g_signal_connect(v->buffer, "changed", G_CALLBACK(on_buffer_changed), v);
  GtkWidget *escroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(escroll), v->editor);
  gtk_widget_set_vexpand(escroll, TRUE);
  gtk_widget_set_size_request(escroll, -1, 80);
  gtk_box_append(GTK_BOX(top), escroll);
  gtk_paned_set_start_child(GTK_PANED(v->paned), top);
  gtk_paned_set_resize_start_child(GTK_PANED(v->paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(v->paned), FALSE);

  /* Results half */
  GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  v->banner = adw_banner_new("");
  gtk_box_append(GTK_BOX(bottom), v->banner);
  v->stack = gtk_stack_new();
  gtk_widget_set_vexpand(v->stack, TRUE);
  GtkWidget *empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "lace-query-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(empty), "Results appear here");
  adw_status_page_set_description(ADW_STATUS_PAGE(empty),
      "Reads are paged as you scroll. Results of a plain single-table SELECT can be edited in place.");
  gtk_widget_add_css_class(empty, "compact");
  gtk_stack_add_named(GTK_STACK(v->stack), empty, "empty");
  v->grid = lace_data_grid_new();
  lace_data_grid_set_sortable(v->grid, FALSE);
  lace_data_grid_set_row_numbers(v->grid, prefs->row_numbers != LACE_ROW_NUMBERS_NEVER);
  gtk_stack_add_named(GTK_STACK(v->stack), GTK_WIDGET(v->grid), "grid");
  v->message_label = gtk_label_new("");
  gtk_widget_add_css_class(v->message_label, "title-3");
  gtk_widget_add_css_class(v->message_label, "dim-label");
  gtk_stack_add_named(GTK_STACK(v->stack), v->message_label, "message");
  gtk_box_append(GTK_BOX(bottom), v->stack);

  GtkWidget *bar = gtk_action_bar_new();
  gtk_widget_add_css_class(bar, "table-footer");
  v->footer_label = gtk_label_new("");
  gtk_widget_add_css_class(v->footer_label, "caption");
  gtk_label_set_ellipsize(GTK_LABEL(v->footer_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_start(v->footer_label, 6);
  gtk_action_bar_pack_start(GTK_ACTION_BAR(bar), v->footer_label);
  v->footer_cell = gtk_label_new("");
  gtk_widget_add_css_class(v->footer_cell, "caption");
  gtk_widget_add_css_class(v->footer_cell, "dim-label");
  gtk_label_set_ellipsize(GTK_LABEL(v->footer_cell), PANGO_ELLIPSIZE_END);
  gtk_action_bar_set_center_widget(GTK_ACTION_BAR(bar), v->footer_cell);
  gtk_action_bar_pack_end(GTK_ACTION_BAR(bar), lace_icon_button("go-jump-symbolic", "Go to row (Ctrl+G)", "query.goto-row"));
  gtk_action_bar_pack_end(GTK_ACTION_BAR(bar), lace_icon_button("view-refresh-symbolic", "Reload the results (F5)", "query.refresh"));
  v->delete_button = lace_icon_button("user-trash-symbolic", "Delete selected rows (Delete)", "query.delete-rows");
  gtk_widget_set_sensitive(v->delete_button, FALSE);
  gtk_action_bar_pack_end(GTK_ACTION_BAR(bar), v->delete_button);
  gtk_box_append(GTK_BOX(bottom), bar);
  gtk_paned_set_end_child(GTK_PANED(v->paned), bottom);
  gtk_paned_set_resize_end_child(GTK_PANED(v->paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(v->paned), FALSE);
  gtk_paned_set_position(GTK_PANED(v->paned), 180);

  v->model = lace_row_model_new((gsize)prefs->page_size);
  lace_row_model_set_fetch(v->model, fetch_page, v);
  lace_data_grid_set_model(v->grid, v->model);
  g_signal_connect(v->model, "total-changed", G_CALLBACK(on_total_changed), v);
  g_signal_connect(gtk_column_view_get_model(lace_data_grid_get_column_view(v->grid)),
                   "selection-changed", G_CALLBACK(on_selection_changed), v);
  g_signal_connect(v->grid, "edit-cell", G_CALLBACK(on_edit_cell), v);
  g_signal_connect(v->grid, "delete-rows", G_CALLBACK(on_delete_rows), v);
  g_signal_connect(v->grid, "cell-focused", G_CALLBACK(on_cell_focused), v);
  g_signal_connect(v->grid, "refresh", G_CALLBACK(on_grid_refresh), v);
  g_signal_connect(v->grid, "follow-fk", G_CALLBACK(on_follow_fk), v);
  g_signal_connect(v->grid, "referenced-by", G_CALLBACK(on_referenced_by), v);

  v->edit.owner = G_OBJECT(v);
  v->edit.widget = GTK_WIDGET(v);
  v->edit.conn = conn;
  v->edit.model = v->model;
  v->edit.grid = v->grid;
  v->edit.unique_names = TRUE;
  v->edit.snapshot_all = TRUE;
  v->edit.confirm_delete = prefs->confirm_delete;
  v->edit.after_delete = after_delete;
  v->edit.user_data = v;

  static const GActionEntry entries[] = {
      {"run", act_query, NULL, NULL, NULL, {0}},
      {"run-all", act_query, NULL, NULL, NULL, {0}},
      {"run-tx", act_query, NULL, NULL, NULL, {0}},
      {"stop", act_query, NULL, NULL, NULL, {0}},
      {"history", act_query, NULL, NULL, NULL, {0}},
      {"goto-row", act_query, NULL, NULL, NULL, {0}},
      {"delete-rows", act_query, NULL, NULL, NULL, {0}},
      {"refresh", act_query, NULL, NULL, NULL, {0}},
      {"follow-key", act_follow_key, "t", NULL, NULL, {0}},
      {"follow-dep", act_follow_dep, "t", NULL, NULL, {0}},
  };
  GSimpleActionGroup *group = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(group), entries, G_N_ELEMENTS(entries), v);
  gtk_widget_insert_action_group(GTK_WIDGET(v), "query", G_ACTION_GROUP(group));
  g_object_unref(group);
  update_footer(v);
  return v;
}

LaceConnection *lace_query_view_get_connection(LaceQueryView *v) { return v->conn; }
LaceDataGrid *lace_query_view_get_grid(LaceQueryView *v) { return v->grid; }
