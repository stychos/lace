/*
 * Lace GTK frontend
 * LaceTableView
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "table_view.h"
#include "add_row_dialog.h"
#include "edit_ops.h"
#include "filter_bar.h"
#include "schema_dialog.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

struct _LaceTableView {
  GtkWidget parent_instance;

  LaceConnection *conn;
  char *table;
  const LacePrefs *prefs;
  LaceSchema *schema;
  LaceRowModel *model;
  LaceDataGrid *grid;
  LaceFilterBar *filters;
  GtkWidget *box;
  GtkWidget *filter_revealer;
  GtkWidget *banner;
  GtkWidget *status_label;
  GtkWidget *cell_label;
  GtkWidget *spinner;
  GtkWidget *delete_button;
  GtkWidget *filter_button;

  char *where;
  char *order_by;
  gboolean count_pending;
  gboolean error_shown;
  gboolean editable;
  gboolean restoring;
  gboolean want_focus; /* Focus the grid once rows exist */
  LaceEditContext edit;
};

typedef struct {
  LaceTableView *view;
  guint64 gen;
  gsize page;
} PageReq;

enum { SIG_TOUCHED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(LaceTableView, lace_table_view, GTK_TYPE_WIDGET)

static void touched(LaceTableView *v) { g_signal_emit(v, signals[SIG_TOUCHED], 0); }

static void start_load(LaceTableView *v);
static void update_status(LaceTableView *v);

/* ==========================================================================
 * Status
 * ========================================================================== */

static void show_error(LaceTableView *v, const char *msg) {
  adw_banner_set_title(ADW_BANNER(v->banner), msg ? msg : "Error");
  adw_banner_set_revealed(ADW_BANNER(v->banner), TRUE);
  v->error_shown = TRUE;
}

static void hide_error(LaceTableView *v) {
  adw_banner_set_revealed(ADW_BANNER(v->banner), FALSE);
  v->error_shown = FALSE;
}

static void update_status(LaceTableView *v) {
  GString *s = g_string_new(NULL);
  gsize total = lace_row_model_get_total(v->model);
  char *n = lace_format_count(total);
  if (!lace_row_model_total_known(v->model))
    g_string_append_printf(s, "%s+ rows", n);
  else if (lace_row_model_total_approximate(v->model))
    g_string_append_printf(s, "≈ %s rows", n);
  else
    g_string_append_printf(s, "%s row%s", n, total == 1 ? "" : "s");
  g_free(n);
  if (!lace_filter_bar_is_empty(v->filters))
    g_string_append(s, " · filtered");
  gsize sel = lace_data_grid_get_selected_count(v->grid);
  if (sel > 0)
    g_string_append_printf(s, " · %" G_GSIZE_FORMAT " selected", sel);
  if (!v->editable && v->schema) {
    const char *why = lace_data_grid_get_readonly_reason(v->grid);
    g_string_append_printf(s, " · read only%s%s", why ? ": " : "", why ? why : "");
  }
  gtk_label_set_text(GTK_LABEL(v->status_label), s->str);
  g_string_free(s, TRUE);
  gtk_widget_set_sensitive(v->delete_button, v->editable && sel > 0);
  gtk_widget_set_visible(v->spinner, lace_connection_is_busy(v->conn));
}

static void on_total_changed(LaceRowModel *m, LaceTableView *v) {
  (void)m;
  update_status(v);
}

static void on_selection_changed(GtkSelectionModel *m, guint pos, guint n, LaceTableView *v) {
  (void)m; (void)pos; (void)n;
  update_status(v);
}

static void on_busy_changed(LaceConnection *c, LaceTableView *v) {
  (void)c;
  gtk_widget_set_visible(v->spinner, lace_connection_is_busy(v->conn));
}

static void on_cell_focused(LaceDataGrid *g, guint row, guint col, LaceTableView *v) {
  (void)g;
  const LaceColumn *c = lace_row_model_get_column(v->model, col);
  char *n = lace_format_count((gsize)row + 1);
  char *t = g_strdup_printf("Row %s · %s · %s", n, c && c->name ? c->name : "",
                            c && c->type_name && *c->type_name ? c->type_name
                                                              : lace_type_short(c ? c->type : LACE_TYPE_TEXT));
  gtk_label_set_text(GTK_LABEL(v->cell_label), t);
  g_free(t);
  g_free(n);
}

/* ==========================================================================
 * Loading
 * ========================================================================== */

static void count_done(LaceJob *job, gpointer data) {
  LaceTableView *v = data;
  guint64 gen = GPOINTER_TO_SIZE(job->tag);
  v->count_pending = FALSE;
  if (gen != lace_row_model_get_generation(v->model))
    return;
  if (job->error || job->count < 0) {
    /* No count: the model keeps growing by pages */
    return;
  }
  lace_row_model_set_total(v->model, (gsize)job->count, TRUE, job->count_approximate);
}

static void start_count(LaceTableView *v) {
  if (v->count_pending)
    return;
  v->count_pending = TRUE;
  gboolean approx = v->prefs->approximate_counts && !v->where;
  LaceJob *job = lace_job_table_count(v->table, v->where, approx);
  job->tag = GSIZE_TO_POINTER(lace_row_model_get_generation(v->model));
  lace_connection_submit(v->conn, job, G_OBJECT(v), count_done, v);
}

static void page_done(LaceJob *job, gpointer data) {
  PageReq *req = data;
  LaceTableView *v = req->view;
  if (job->error) {
    lace_row_model_page_failed(v->model, req->gen, req->page);
    if (!job->cancelled && req->gen == lace_row_model_get_generation(v->model))
      show_error(v, lace_job_error(job));
  } else {
    gboolean first = req->page == 0 && req->gen == lace_row_model_get_generation(v->model) &&
                     lace_row_model_n_columns(v->model) == 0;
    lace_row_model_page_loaded(v->model, req->gen, req->page, lace_job_steal_result(job));
    if (req->gen == lace_row_model_get_generation(v->model)) {
      if (first)
        lace_data_grid_measure_columns(v->grid);
      if (!lace_row_model_total_known(v->model))
        start_count(v);
      if (first && v->want_focus && lace_row_model_get_total(v->model) > 0) {
        /* The grab retries while the cell is being realised and gives up
         * for a tab that is not shown */
        v->want_focus = FALSE;
        lace_data_grid_focus_cell(v->grid, 0, 0);
      }
    }
  }
  update_status(v);
  lace_trim_heap();
}

static void fetch_page(LaceRowModel *m, gsize page, gpointer data) {
  LaceTableView *v = data;
  gsize size = lace_row_model_get_page_size(m);
  char *sql = sql_table_page(lace_connection_get_driver(v->conn), v->table, v->where,
                             v->order_by, size, page * size);
  if (!sql)
    return;
  PageReq *req = g_new0(PageReq, 1);
  req->view = v;
  req->gen = lace_row_model_get_generation(m);
  req->page = page;
  LaceJob *job = lace_job_read(sql);
  free(sql);
  lace_connection_submit_full(v->conn, job, G_OBJECT(v), page_done, req, g_free);
}

static void rebuild_clauses(LaceTableView *v) {
  g_free(v->where);
  g_free(v->order_by);
  v->where = NULL;
  v->order_by = NULL;
  gsize nf = 0;
  const Filter *f = lace_filter_bar_get_filters(v->filters, &nf);
  char *w = sql_build_where(f, nf, lace_filter_bar_get_search(v->filters), v->schema,
                            lace_connection_get_driver(v->conn));
  v->where = w ? g_strdup(w) : NULL;
  free(w);
  /* A restored sort waits for the columns before the headers show it:
   * until then it is the order to load with */
  SortSpec *pending = g_object_get_data(G_OBJECT(v), "pending-sorts");
  gsize ns = 0;
  SortSpec *sorts = pending ? NULL : lace_data_grid_get_sorts(v->grid, &ns);
  if (pending)
    ns = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(v), "pending-sorts-n"));
  char *o = sql_build_order_by(pending ? pending : sorts, ns, lace_connection_get_driver(v->conn));
  v->order_by = o ? g_strdup(o) : NULL;
  free(o);
  for (gsize i = 0; sorts && i < ns; i++)
    g_free(sorts[i].column);
  g_free(sorts);
}

static void start_load(LaceTableView *v) {
  hide_error(v);
  rebuild_clauses(v);
  v->count_pending = FALSE;
  lace_row_model_reset(v->model);
  lace_row_model_request(v->model, 0);
  update_status(v);
}

static void schema_done(LaceJob *job, gpointer data) {
  LaceTableView *v = data;
  if (job->error) {
    show_error(v, lace_job_error(job));
    v->editable = FALSE;
    lace_data_grid_set_readonly_reason(v->grid, "the table's structure could not be read");
  } else {
    lace_schema_free(v->schema);
    v->schema = lace_job_steal_schema(job);
    v->edit.schema = v->schema;
    lace_data_grid_set_schema(v->grid, v->schema);
    lace_filter_bar_set_schema(v->filters, v->schema);
    gboolean has_pk = FALSE;
    for (size_t i = 0; v->schema && i < v->schema->num_columns; i++)
      if (v->schema->columns[i].primary_key)
        has_pk = TRUE;
    v->editable = has_pk;
    lace_data_grid_set_readonly_reason(v->grid, has_pk ? NULL : "the table has no primary key");
    lace_data_grid_set_row_numbers(v->grid, lace_prefs_row_numbers_for(v->prefs, v->schema));
  }
  lace_data_grid_set_editable(v->grid, v->editable);
  start_load(v);
}

static void load_schema(LaceTableView *v) {
  lace_connection_submit(v->conn, lace_job_schema(v->table), G_OBJECT(v), schema_done, v);
}

void lace_table_view_refresh(LaceTableView *v) {
  if (!v->schema)
    load_schema(v);
  else
    start_load(v);
}

static void on_sort_changed(LaceDataGrid *g, LaceTableView *v) {
  (void)g;
  if (!v->restoring) {
    touched(v);
    start_load(v);
  }
}

static void on_filters_changed(LaceFilterBar *b, LaceTableView *v) {
  (void)b;
  if (!v->restoring) {
    touched(v);
    start_load(v);
  }
}

/* ==========================================================================
 * Writes (through edit_ops: the one path shared with query results)
 * ========================================================================== */

static void on_edit_cell(LaceDataGrid *g, guint row, guint col, const char *text,
                         gboolean set_null, LaceTableView *v) {
  (void)g;
  touched(v);
  lace_edit_cell(&v->edit, row, col, text, set_null);
}

void lace_table_view_delete_selected(LaceTableView *v) {
  lace_edit_delete_selected(&v->edit);
}

static void on_delete_rows(LaceDataGrid *g, LaceTableView *v) {
  (void)g;
  lace_table_view_delete_selected(v);
}

static void after_delete(gsize deleted, gpointer data) {
  (void)deleted;
  update_status(LACE_TABLE_VIEW(data));
}

static void row_added(gboolean inserted, gpointer data) {
  LaceTableView *v = data;
  if (inserted) {
    lace_toast(GTK_WIDGET(v), "Row inserted");
    start_load(v);
  }
  g_object_unref(v);
}

void lace_table_view_add_row(LaceTableView *v) {
  if (!v->schema) {
    lace_toast(GTK_WIDGET(v), "The table's structure is not loaded");
    return;
  }
  lace_add_row_dialog_show(GTK_WIDGET(v), v->conn, v->table, v->schema, row_added, g_object_ref(v));
}

static void on_add_row(LaceDataGrid *g, LaceTableView *v) {
  (void)g;
  lace_table_view_add_row(v);
}

static void on_refresh(LaceDataGrid *g, LaceTableView *v) {
  (void)g;
  lace_table_view_refresh(v);
}

static void on_filter_column(LaceDataGrid *g, const char *column, const char *value, LaceTableView *v) {
  (void)g;
  gtk_revealer_set_reveal_child(GTK_REVEALER(v->filter_revealer), TRUE);
  if (value && *value)
    lace_filter_bar_add(v->filters, column, FILTER_EQ, value);
  else if (!value)
    lace_filter_bar_add(v->filters, column, FILTER_NULL, NULL);
  else
    lace_filter_bar_open_add(v->filters, column);
}

/* ==========================================================================
 * Public
 * ========================================================================== */

void lace_table_view_toggle_filters(LaceTableView *v) {
  gboolean shown = gtk_revealer_get_reveal_child(GTK_REVEALER(v->filter_revealer));
  if (shown && !gtk_widget_has_focus(GTK_WIDGET(v->filters)) &&
      !gtk_widget_is_ancestor(gtk_root_get_focus(gtk_widget_get_root(GTK_WIDGET(v))), GTK_WIDGET(v->filters))) {
    lace_filter_bar_focus_search(v->filters);
    return;
  }
  gtk_revealer_set_reveal_child(GTK_REVEALER(v->filter_revealer), !shown);
  if (!shown)
    lace_filter_bar_focus_search(v->filters);
  else
    lace_table_view_focus_grid(v);
}

static void goto_row_cb(const char *text, gpointer data) {
  LaceTableView *v = data;
  if (text) {
    char *end = NULL;
    long long n = strtoll(text, &end, 10);
    if (end && end != text && n >= 1) {
      gsize total = lace_row_model_get_total(v->model);
      gsize row = (gsize)n - 1;
      if (total > 0 && row >= total)
        row = total - 1;
      lace_data_grid_scroll_to_row(v->grid, row);
    }
  }
  g_object_unref(v);
}

void lace_table_view_goto_row(LaceTableView *v) {
  char *n = lace_format_count(lace_row_model_get_total(v->model));
  char *body = g_strdup_printf("Row number (1 to %s%s)", n,
                               lace_row_model_total_known(v->model) ? "" : "+");
  lace_prompt(GTK_WIDGET(v), "Go to row", body, NULL, "_Go", FALSE, goto_row_cb, g_object_ref(v));
  g_free(body);
  g_free(n);
}

void lace_table_view_show_structure(LaceTableView *v) {
  if (!v->schema) {
    lace_toast(GTK_WIDGET(v), "The table's structure is not loaded");
    return;
  }
  lace_schema_dialog_show(GTK_WIDGET(v), v->table, v->schema);
}

void lace_table_view_focus_grid(LaceTableView *v) {
  gsize row = 0, col = 0;
  if (lace_data_grid_get_focus_cell(v->grid, &row, &col) ||
      lace_row_model_get_total(v->model) > 0) {
    lace_data_grid_focus_cell(v->grid, row, col);
  } else {
    v->want_focus = TRUE;
    gtk_widget_grab_focus(GTK_WIDGET(lace_data_grid_get_column_view(v->grid)));
  }
}

const char *lace_table_view_get_table(LaceTableView *v) { return v->table; }
LaceConnection *lace_table_view_get_connection(LaceTableView *v) { return v->conn; }
LaceDataGrid *lace_table_view_get_grid(LaceTableView *v) { return v->grid; }

const Filter *lace_table_view_get_filters(LaceTableView *v, gsize *n) {
  return lace_filter_bar_get_filters(v->filters, n);
}

const char *lace_table_view_get_search(LaceTableView *v) {
  return lace_filter_bar_get_search(v->filters);
}

SortSpec *lace_table_view_get_sorts(LaceTableView *v, gsize *n) {
  return lace_data_grid_get_sorts(v->grid, n);
}

gboolean lace_table_view_filters_visible(LaceTableView *v) {
  return gtk_revealer_get_reveal_child(GTK_REVEALER(v->filter_revealer));
}

void lace_table_view_restore(LaceTableView *v, const Filter *filters, gsize n,
                             const char *search, const SortSpec *sorts,
                             gsize num_sorts, gboolean filters_visible) {
  v->restoring = TRUE;
  lace_filter_bar_set_filters(v->filters, filters, n, search);
  gtk_revealer_set_reveal_child(GTK_REVEALER(v->filter_revealer),
                                filters_visible || n > 0 || (search && *search));
  /* Sorts need the columns, which arrive with the first page: keep them
   * and apply once the grid has columns */
  if (num_sorts > 0) {
    SortSpec *copy = g_new0(SortSpec, num_sorts);
    for (gsize i = 0; i < num_sorts; i++) {
      copy[i].column = g_strdup(sorts[i].column);
      copy[i].desc = sorts[i].desc;
    }
    g_object_set_data_full(G_OBJECT(v), "pending-sorts", copy, g_free);
    g_object_set_data(G_OBJECT(v), "pending-sorts-n", GSIZE_TO_POINTER(num_sorts));
  }
  v->restoring = FALSE;
  /* The schema load in progress will start the load with these clauses;
   * if it already finished, load again */
  if (v->schema)
    start_load(v);
}

/* Once columns exist, reflect restored sorts in the headers */
static void on_columns_changed(LaceRowModel *m, LaceTableView *v) {
  if (lace_row_model_n_columns(m) == 0)
    return;
  SortSpec *pending = g_object_get_data(G_OBJECT(v), "pending-sorts");
  if (!pending)
    return;
  gsize n = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(v), "pending-sorts-n"));
  v->restoring = TRUE;
  lace_data_grid_set_sorts(v->grid, pending, n);
  v->restoring = FALSE;
  for (gsize i = 0; i < n; i++)
    g_free(pending[i].column);
  g_object_set_data(G_OBJECT(v), "pending-sorts", NULL);
}

/* ==========================================================================
 * Object
 * ========================================================================== */

static void on_busy_or_notice(LaceConnection *c, const char *msg, LaceTableView *v) {
  (void)c;
  lace_toast(GTK_WIDGET(v), "%s", msg);
}

static void lace_table_view_dispose(GObject *obj) {
  LaceTableView *v = LACE_TABLE_VIEW(obj);
  if (v->conn) {
    g_signal_handlers_disconnect_by_func(v->conn, on_busy_changed, v);
    g_signal_handlers_disconnect_by_func(v->conn, on_busy_or_notice, v);
  }
  if (v->model)
    g_signal_handlers_disconnect_by_func(v->model, on_columns_changed, v);
  if (v->grid) {
    lace_data_grid_set_schema(v->grid, NULL);
    lace_data_grid_set_model(v->grid, NULL);
  }
  v->edit.schema = NULL;
  if (v->filters)
    lace_filter_bar_set_schema(v->filters, NULL);
  g_clear_pointer(&v->box, gtk_widget_unparent);
  v->grid = NULL;
  v->filters = NULL;
  g_clear_object(&v->model);
  g_clear_object(&v->conn);
  g_clear_pointer(&v->schema, lace_schema_free);
  g_clear_pointer(&v->table, g_free);
  g_clear_pointer(&v->where, g_free);
  g_clear_pointer(&v->order_by, g_free);
  lace_edit_context_clear(&v->edit);
  G_OBJECT_CLASS(lace_table_view_parent_class)->dispose(obj);
}

static void lace_table_view_class_init(LaceTableViewClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = lace_table_view_dispose;
  signals[SIG_TOUCHED] = g_signal_new("touched", LACE_TYPE_TABLE_VIEW,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "lace-tableview");
}

static void lace_table_view_init(LaceTableView *v) {
  v->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(v->box, GTK_WIDGET(v));

  v->banner = adw_banner_new("");
  gtk_box_append(GTK_BOX(v->box), v->banner);

  v->filters = lace_filter_bar_new();
  v->filter_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(v->filter_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_child(GTK_REVEALER(v->filter_revealer), GTK_WIDGET(v->filters));
  gtk_box_append(GTK_BOX(v->box), v->filter_revealer);

  v->grid = lace_data_grid_new();
  gtk_widget_set_vexpand(GTK_WIDGET(v->grid), TRUE);
  gtk_box_append(GTK_BOX(v->box), GTK_WIDGET(v->grid));

  GtkWidget *bar = gtk_action_bar_new();
  gtk_widget_add_css_class(bar, "table-footer");
  v->status_label = gtk_label_new("");
  gtk_widget_add_css_class(v->status_label, "caption");
  gtk_label_set_ellipsize(GTK_LABEL(v->status_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_start(v->status_label, 6);
  gtk_action_bar_pack_start(GTK_ACTION_BAR(bar), v->status_label);
  v->spinner = adw_spinner_new();
  gtk_widget_set_size_request(v->spinner, 16, 16);
  gtk_widget_set_visible(v->spinner, FALSE);
  gtk_action_bar_pack_start(GTK_ACTION_BAR(bar), v->spinner);
  v->cell_label = gtk_label_new("");
  gtk_widget_add_css_class(v->cell_label, "caption");
  gtk_widget_add_css_class(v->cell_label, "dim-label");
  gtk_label_set_ellipsize(GTK_LABEL(v->cell_label), PANGO_ELLIPSIZE_END);
  gtk_action_bar_set_center_widget(GTK_ACTION_BAR(bar), v->cell_label);

  GtkWidget *b;
  b = lace_icon_button("document-properties-symbolic", "Table structure (Ctrl+I)", "view.structure");
  gtk_action_bar_pack_end(GTK_ACTION_BAR(bar), b);
  b = lace_icon_button("go-jump-symbolic", "Go to row (Ctrl+G)", "view.goto-row");
  gtk_action_bar_pack_end(GTK_ACTION_BAR(bar), b);
  v->filter_button = lace_icon_button("lace-filter-symbolic", "Filters (Ctrl+F)", "view.filter");
  gtk_action_bar_pack_end(GTK_ACTION_BAR(bar), v->filter_button);
  b = lace_icon_button("view-refresh-symbolic", "Refresh (F5)", "view.refresh");
  gtk_action_bar_pack_end(GTK_ACTION_BAR(bar), b);
  v->delete_button = lace_icon_button("user-trash-symbolic", "Delete selected rows (Delete)", "view.delete-rows");
  gtk_widget_set_sensitive(v->delete_button, FALSE);
  gtk_action_bar_pack_end(GTK_ACTION_BAR(bar), v->delete_button);
  b = lace_icon_button("list-add-symbolic", "Add row (Ctrl+Shift+N)", "view.add-row");
  gtk_action_bar_pack_end(GTK_ACTION_BAR(bar), b);
  gtk_box_append(GTK_BOX(v->box), bar);

  g_signal_connect(v->grid, "sort-changed", G_CALLBACK(on_sort_changed), v);
  g_signal_connect(v->grid, "edit-cell", G_CALLBACK(on_edit_cell), v);
  g_signal_connect(v->grid, "delete-rows", G_CALLBACK(on_delete_rows), v);
  g_signal_connect(v->grid, "cell-focused", G_CALLBACK(on_cell_focused), v);
  g_signal_connect(v->grid, "add-row", G_CALLBACK(on_add_row), v);
  g_signal_connect(v->grid, "refresh", G_CALLBACK(on_refresh), v);
  g_signal_connect(v->grid, "filter-column", G_CALLBACK(on_filter_column), v);
  g_signal_connect(v->filters, "changed", G_CALLBACK(on_filters_changed), v);
}

static void act_view(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)p;
  LaceTableView *v = data;
  const char *name = g_action_get_name(G_ACTION(a));
  if (g_str_equal(name, "structure")) lace_table_view_show_structure(v);
  else if (g_str_equal(name, "goto-row")) lace_table_view_goto_row(v);
  else if (g_str_equal(name, "filter")) lace_table_view_toggle_filters(v);
  else if (g_str_equal(name, "refresh")) lace_table_view_refresh(v);
  else if (g_str_equal(name, "delete-rows")) lace_table_view_delete_selected(v);
  else if (g_str_equal(name, "add-row")) lace_table_view_add_row(v);
}

LaceTableView *lace_table_view_new(LaceConnection *conn, const char *table,
                                   const LacePrefs *prefs) {
  LaceTableView *v = g_object_new(LACE_TYPE_TABLE_VIEW, NULL);
  v->conn = g_object_ref(conn);
  v->table = g_strdup(table);
  v->prefs = prefs;
  v->model = lace_row_model_new((gsize)prefs->page_size);
  lace_row_model_set_fetch(v->model, fetch_page, v);
  lace_data_grid_set_row_numbers(v->grid, prefs->row_numbers != LACE_ROW_NUMBERS_NEVER);
  lace_data_grid_set_model(v->grid, v->model);
  v->edit.owner = G_OBJECT(v);
  v->edit.widget = GTK_WIDGET(v);
  v->edit.conn = conn;
  v->edit.model = v->model;
  v->edit.grid = v->grid;
  v->edit.table = v->table;
  v->edit.unique_names = FALSE;
  v->edit.snapshot_all = FALSE;
  v->edit.confirm_delete = prefs->confirm_delete;
  v->edit.after_delete = after_delete;
  v->edit.user_data = v;
  g_signal_connect(v->model, "total-changed", G_CALLBACK(on_total_changed), v);
  g_signal_connect(v->model, "columns-changed", G_CALLBACK(on_columns_changed), v);
  g_signal_connect(gtk_column_view_get_model(lace_data_grid_get_column_view(v->grid)),
                   "selection-changed", G_CALLBACK(on_selection_changed), v);
  g_signal_connect(conn, "busy-changed", G_CALLBACK(on_busy_changed), v);
  g_signal_connect(conn, "notice", G_CALLBACK(on_busy_or_notice), v);

  static const GActionEntry entries[] = {
      {"structure", act_view, NULL, NULL, NULL, {0}},
      {"goto-row", act_view, NULL, NULL, NULL, {0}},
      {"filter", act_view, NULL, NULL, NULL, {0}},
      {"refresh", act_view, NULL, NULL, NULL, {0}},
      {"delete-rows", act_view, NULL, NULL, NULL, {0}},
      {"add-row", act_view, NULL, NULL, NULL, {0}},
  };
  GSimpleActionGroup *group = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(group), entries, G_N_ELEMENTS(entries), v);
  gtk_widget_insert_action_group(GTK_WIDGET(v), "view", G_ACTION_GROUP(group));
  g_object_unref(group);

  load_schema(v);
  return v;
}
