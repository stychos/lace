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
#include "catalog_list.h"
#include "core/catalog_rows.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

struct _LaceTableView {
  GtkWidget parent_instance;

  LaceConnection *conn;
  char *table;
  LaceObjectKind object;      /* what table names: a table, a database view, a routine, a trigger */
  char *object_table;         /* a trigger's table */
  char *object_detail;        /* a routine's kind, a materialized view's "materialized" */
  LaceSubView subview;
  GtkWidget *stack;           /* AdwViewStack: "data" and the lists */
  GtkWidget *switcher;
  LaceCatalogList *lists[LACE_SUBVIEW_COUNT];
  /* What the sub-views besides Data read when first shown; a refresh drops them */
  LaceTableInfo *info;
  LaceAutoInc *autoinc;
  char *info_error;
  gint64 info_count;          /* the exact count asked for from Information, -1 unknown */
  gboolean counting;
  LaceDependent *deps;
  size_t num_deps;
  gboolean deps_loaded;
  char *deps_error;
  char *definition;
  char *definition_error;
  gboolean catalog_loading;
  gboolean referenced_by_pending;
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

enum { SIG_TOUCHED, SIG_OPEN_FILTERED, SIG_OPEN_OBJECT, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(LaceTableView, lace_table_view, GTK_TYPE_WIDGET)

static void touched(LaceTableView *v) { g_signal_emit(v, signals[SIG_TOUCHED], 0); }

static void start_load(LaceTableView *v);
static void update_status(LaceTableView *v);
static void render_list(LaceTableView *v);

/* ==========================================================================
 * Sub-view names
 * ========================================================================== */

static const char *const SUBVIEW_KEYS[LACE_SUBVIEW_COUNT] = {"data", "structure", "indexes", "foreign_keys", "information"};
static const char *const OBJECT_KEYS[] = {"table", "view", "routine", "trigger"};

const char *lace_subview_key(LaceSubView sv, LaceObjectKind kind) {
  if (sv == LACE_SUBVIEW_STRUCTURE && kind != LACE_OBJ_TABLE)
    return "definition";
  return sv < LACE_SUBVIEW_COUNT ? SUBVIEW_KEYS[sv] : "data";
}

gboolean lace_subview_from_key(const char *key, LaceSubView *out) {
  if (!key)
    return FALSE;
  if (g_str_equal(key, "definition")) {
    *out = LACE_SUBVIEW_STRUCTURE;
    return TRUE;
  }
  for (int i = 0; i < LACE_SUBVIEW_COUNT; i++)
    if (g_str_equal(key, SUBVIEW_KEYS[i])) {
      *out = (LaceSubView)i;
      return TRUE;
    }
  return FALSE;
}

const char *lace_object_kind_key(LaceObjectKind kind) { return kind <= LACE_OBJ_TRIGGER ? OBJECT_KEYS[kind] : "table"; }

gboolean lace_object_kind_from_key(const char *key, LaceObjectKind *out) {
  for (int i = 0; key && i < 4; i++)
    if (g_str_equal(key, OBJECT_KEYS[i])) {
      *out = (LaceObjectKind)i;
      return TRUE;
    }
  return FALSE;
}

gboolean lace_object_has_subview(LaceObjectKind kind, LaceSubView sv) {
  switch (kind) {
  case LACE_OBJ_TABLE: return sv < LACE_SUBVIEW_COUNT;
  case LACE_OBJ_VIEW: return sv == LACE_SUBVIEW_DATA || sv == LACE_SUBVIEW_STRUCTURE || sv == LACE_SUBVIEW_INFORMATION;
  default: return sv == LACE_SUBVIEW_STRUCTURE || sv == LACE_SUBVIEW_INFORMATION;
  }
}

static const char *subview_label(LaceSubView sv, LaceObjectKind kind) {
  switch (sv) {
  case LACE_SUBVIEW_DATA: return "Data";
  case LACE_SUBVIEW_STRUCTURE: return kind == LACE_OBJ_TABLE ? "Structure" : "Definition";
  case LACE_SUBVIEW_INDEXES: return "Indexes";
  case LACE_SUBVIEW_FOREIGN_KEYS: return "Foreign keys";
  case LACE_SUBVIEW_INFORMATION: return "Information";
  default: return "";
  }
}

static const char *subview_icon(LaceSubView sv) {
  switch (sv) {
  case LACE_SUBVIEW_DATA: return "lace-table-symbolic";
  case LACE_SUBVIEW_STRUCTURE: return "document-properties-symbolic";
  case LACE_SUBVIEW_INDEXES: return "view-list-symbolic";
  case LACE_SUBVIEW_FOREIGN_KEYS: return "lace-key-symbolic";
  default: return "dialog-information-symbolic";
  }
}

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
  char *sql = lace_sql_table_page(lace_connection_get_driver(v->conn), v->table, v->where,
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
  const LaceFilter *f = lace_filter_bar_get_filters(v->filters, &nf);
  char *w = lace_filter_build_where(f, nf, lace_filter_bar_get_search(v->filters), v->schema,
                            lace_connection_get_driver(v->conn));
  v->where = w ? g_strdup(w) : NULL;
  free(w);
  /* A restored sort waits for the columns before the headers show it:
   * until then it is the order to load with */
  LaceSortSpec *pending = g_object_get_data(G_OBJECT(v), "pending-sorts");
  gsize ns = 0;
  LaceSortSpec *sorts = pending ? NULL : lace_data_grid_get_sorts(v->grid, &ns);
  if (pending)
    ns = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(v), "pending-sorts-n"));
  char *o = lace_sort_build_order_by(pending ? pending : sorts, ns, lace_connection_get_driver(v->conn));
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
    lace_data_grid_set_fk_columns(v->grid, v->schema);
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
  render_list(v);
}

static void load_schema(LaceTableView *v) {
  lace_connection_submit(v->conn, lace_job_schema(v->table), G_OBJECT(v), schema_done, v);
}

static void drop_catalog(LaceTableView *v) {
  g_clear_pointer(&v->info, lace_table_info_free);
  g_clear_pointer(&v->autoinc, lace_autoinc_free);
  g_clear_pointer(&v->info_error, g_free);
  v->info_count = -1;
  lace_dependents_free(v->deps, v->num_deps);
  v->deps = NULL;
  v->num_deps = 0;
  v->deps_loaded = FALSE;
  g_clear_pointer(&v->deps_error, g_free);
  g_clear_pointer(&v->definition, g_free);
  g_clear_pointer(&v->definition_error, g_free);
}

void lace_table_view_refresh(LaceTableView *v) {
  drop_catalog(v);
  if (!lace_object_has_subview(v->object, LACE_SUBVIEW_DATA))
    render_list(v);
  else if (!v->schema || v->subview != LACE_SUBVIEW_DATA)
    load_schema(v); /* the columns, keys and indexes again */
  else
    start_load(v);
}

/* ==========================================================================
 * The sub-views' reads
 * ========================================================================== */

static void info_done(LaceJob *job, gpointer data) {
  LaceTableView *v = data;
  v->catalog_loading = FALSE;
  if (job->error) {
    g_free(v->info_error);
    v->info_error = g_strdup(lace_job_error(job));
  } else {
    g_clear_pointer(&v->info, lace_table_info_free);
    g_clear_pointer(&v->autoinc, lace_autoinc_free);
    v->info = job->info;
    v->autoinc = job->autoinc;
    job->info = NULL;
    job->autoinc = NULL;
  }
  render_list(v);
}

static void deps_done(LaceJob *job, gpointer data) {
  LaceTableView *v = data;
  v->catalog_loading = FALSE;
  if (job->error) {
    g_free(v->deps_error);
    v->deps_error = g_strdup(lace_job_error(job));
    v->referenced_by_pending = FALSE;
  } else {
    lace_dependents_free(v->deps, v->num_deps);
    v->deps = job->deps;
    v->num_deps = job->num_deps;
    job->deps = NULL;
    job->num_deps = 0;
    v->deps_loaded = TRUE;
    if (v->referenced_by_pending) {
      v->referenced_by_pending = FALSE;
      lace_table_view_referenced_by(v); /* the menu that waited for the list */
    }
  }
  render_list(v);
}

static void definition_done(LaceJob *job, gpointer data) {
  LaceTableView *v = data;
  v->catalog_loading = FALSE;
  if (job->error) {
    g_free(v->definition_error);
    v->definition_error = g_strdup(lace_job_error(job));
  } else {
    g_free(v->definition);
    v->definition = g_strdup(job->definition ? job->definition : "");
  }
  render_list(v);
}

static void load_info(LaceTableView *v) {
  if (v->info || v->info_error || v->catalog_loading)
    return;
  if (v->object != LACE_OBJ_TABLE && v->object != LACE_OBJ_VIEW)
    return;
  v->catalog_loading = TRUE;
  lace_connection_submit(v->conn, lace_job_table_info(v->table), G_OBJECT(v), info_done, v);
}

static void load_deps(LaceTableView *v) {
  if (v->deps_loaded || v->deps_error || v->catalog_loading)
    return;
  v->catalog_loading = TRUE;
  lace_connection_submit(v->conn, lace_job_dependents(v->table), G_OBJECT(v), deps_done, v);
}

static void load_definition(LaceTableView *v) {
  if (v->definition || v->definition_error || v->catalog_loading)
    return;
  /* a trigger restored without its table: the object list knows it */
  if (v->object == LACE_OBJ_TRIGGER && !v->object_table) {
    size_t n = 0;
    const LaceObject *objs = lace_connection_get_objects(v->conn, &n);
    for (size_t i = 0; i < n; i++)
      if (objs[i].kind == LACE_OBJ_TRIGGER && objs[i].name && g_str_equal(objs[i].name, v->table) && objs[i].table) {
        v->object_table = g_strdup(objs[i].table);
        break;
      }
    if (!v->object_table && !lace_connection_tables_loaded(v->conn))
      return; /* asked again when the list is in */
  }
  v->catalog_loading = TRUE;
  lace_connection_submit(v->conn, lace_job_definition(v->object, v->table, v->object_table, v->object_detail),
                         G_OBJECT(v), definition_done, v);
}

/* The current list from what is loaded; asks for what is not */
static void render_list(LaceTableView *v) {
  if (v->subview == LACE_SUBVIEW_DATA)
    return;
  LaceCatalogList *l = v->lists[v->subview];
  if (!l)
    return;
  gboolean data_like = lace_object_has_subview(v->object, LACE_SUBVIEW_DATA);
  if (data_like && !v->schema) {
    lace_catalog_list_show(l, NULL, v->error_shown ? "The structure could not be read" : "Loading…");
    return;
  }
  CatalogRows rows;
  catalog_rows_init(&rows, 0, NULL);
  switch (v->subview) {
  case LACE_SUBVIEW_STRUCTURE:
    if (v->object == LACE_OBJ_TABLE) {
      catalog_rows_structure(&rows, v->schema);
      lace_catalog_list_show(l, &rows, NULL);
    } else {
      load_definition(v);
      if (v->definition) {
        catalog_rows_definition(&rows, v->definition, 110, v->object == LACE_OBJ_VIEW ? v->schema : NULL);
        lace_catalog_list_show(l, &rows, NULL);
      } else {
        lace_catalog_list_show(l, NULL, v->definition_error ? v->definition_error : "Loading…");
      }
    }
    break;
  case LACE_SUBVIEW_INDEXES:
    if (catalog_rows_indexes(&rows, v->schema))
      lace_catalog_list_show(l, &rows, NULL);
    else
      lace_catalog_list_show(l, NULL, "No indexes or constraints");
    break;
  case LACE_SUBVIEW_FOREIGN_KEYS:
    load_deps(v);
    catalog_rows_fks(&rows, v->schema, v->table, v->deps, v->num_deps, v->deps_loaded, v->deps_error);
    lace_catalog_list_show(l, &rows, NULL);
    break;
  case LACE_SUBVIEW_INFORMATION: {
    load_info(v);
    size_t n = 0;
    const LaceObject *objs = lace_connection_get_objects(v->conn, &n);
    /* the exact count: asked for here, or Data's when it was counted and unfiltered */
    gint64 exact = v->info_count >= 0 ? v->info_count
                   : data_like && lace_row_model_total_known(v->model) &&
                           !lace_row_model_total_approximate(v->model) && lace_filter_bar_is_empty(v->filters)
                       ? (gint64)lace_row_model_get_total(v->model)
                       : -1;
    catalog_rows_info(&rows, v->object, v->table, v->object_table, v->object_detail, v->info, v->autoinc,
                      v->info_error, exact, v->counting, objs, n);
    lace_catalog_list_show(l, &rows, NULL);
    break;
  }
  default:
    break;
  }
  catalog_rows_free(&rows);
}

static void exact_count_done(LaceJob *job, gpointer data) {
  LaceTableView *v = data;
  v->counting = FALSE;
  if (!job->error && job->count >= 0)
    v->info_count = job->count;
  else
    lace_toast(GTK_WIDGET(v), "The count failed");
  render_list(v);
}

/* Return or a double click on a list row */
static void on_list_activate(LaceCatalogList *l, int tag, LaceTableView *v) {
  (void)l;
  if (tag == CATALOG_TAG_ROWS && v->subview == LACE_SUBVIEW_INFORMATION) {
    if (v->counting)
      return;
    v->counting = TRUE;
    render_list(v);
    lace_connection_submit(v->conn, lace_job_table_count(v->table, NULL, FALSE), G_OBJECT(v), exact_count_done, v);
    return;
  }
  if (tag >= CATALOG_TAG_TRIGGER) {
    size_t n = 0;
    const LaceObject *objs = lace_connection_get_objects(v->conn, &n);
    size_t i = (size_t)(tag - CATALOG_TAG_TRIGGER);
    if (i < n && objs[i].kind == LACE_OBJ_TRIGGER)
      g_signal_emit(v, signals[SIG_OPEN_OBJECT], 0, (int)LACE_OBJ_TRIGGER, objs[i].name, objs[i].table,
                    (int)LACE_SUBVIEW_STRUCTURE);
    return;
  }
  if (tag >= CATALOG_TAG_DEP && tag < CATALOG_TAG_TRIGGER) {
    size_t i = (size_t)(tag - CATALOG_TAG_DEP);
    if (i < v->num_deps && v->deps[i].table)
      g_signal_emit(v, signals[SIG_OPEN_OBJECT], 0, (int)LACE_OBJ_TABLE, v->deps[i].table, NULL,
                    (int)LACE_SUBVIEW_FOREIGN_KEYS);
  }
}

static void on_stack_page_changed(GObject *stack, GParamSpec *ps, LaceTableView *v) {
  (void)ps;
  const char *name = adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(stack));
  LaceSubView sv;
  if (name && lace_subview_from_key(name, &sv) && sv != v->subview) {
    v->subview = sv;
    render_list(v);
  }
}

LaceSubView lace_table_view_get_subview(LaceTableView *v) { return v->subview; }

void lace_table_view_set_subview(LaceTableView *v, LaceSubView sv) {
  if (!lace_object_has_subview(v->object, sv)) {
    lace_toast(GTK_WIDGET(v), "A %s has no %s", catalog_object_word(v->object), subview_label(sv, v->object));
    return;
  }
  v->subview = sv;
  adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(v->stack), lace_subview_key(sv, v->object));
  render_list(v);
}

char **lace_table_view_get_subview_texts(LaceTableView *v) {
  if (v->subview == LACE_SUBVIEW_DATA || !v->lists[v->subview])
    return NULL;
  return lace_catalog_list_get_texts(v->lists[v->subview]);
}

LaceObjectKind lace_table_view_get_object_kind(LaceTableView *v) { return v->object; }
const char *lace_table_view_get_object_table(LaceTableView *v) { return v->object_table; }
const char *lace_table_view_get_object_detail(LaceTableView *v) { return v->object_detail; }

/* ==========================================================================
 * Following foreign keys
 * ========================================================================== */

/* The focused row's values of the named columns as edit text (g_strfreev);
 * NULL with a toast when one is missing, NULL or not loaded */
static char **row_values(LaceTableView *v, char *const *columns, size_t n) {
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

static void emit_filtered(LaceTableView *v, const char *table, char *const *columns, char **values, size_t n) {
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

static void follow_key(LaceTableView *v, const LaceForeignKey *fk) {
  if (!fk->ref_table || fk->num_ref_columns != fk->num_columns) {
    lace_toast(GTK_WIDGET(v), "The foreign key %s is incomplete", fk->name ? fk->name : "");
    return;
  }
  char **vals = row_values(v, fk->columns, fk->num_columns);
  if (!vals)
    return;
  emit_filtered(v, fk->ref_table, fk->ref_columns, vals, fk->num_columns);
  g_strfreev(vals);
}

static void act_follow_key(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a;
  LaceTableView *v = data;
  guint64 i = g_variant_get_uint64(p);
  if (v->schema && i < v->schema->num_foreign_keys)
    follow_key(v, &v->schema->foreign_keys[i]);
}

static void act_follow_dep(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a;
  LaceTableView *v = data;
  guint64 i = g_variant_get_uint64(p);
  if (i >= v->num_deps)
    return;
  const LaceDependent *d = &v->deps[i];
  char **vals = row_values(v, d->ref_columns, d->num_columns);
  if (!vals)
    return;
  emit_filtered(v, d->table, d->columns, vals, d->num_columns);
  g_strfreev(vals);
}

/* A menu of choices at the focused cell */
static void popup_choices(LaceTableView *v, GMenu *menu) {
  GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  gtk_widget_set_parent(pop, GTK_WIDGET(v->grid));
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
  g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
  gtk_popover_popup(GTK_POPOVER(pop));
}

void lace_table_view_follow_fk(LaceTableView *v) {
  if (v->subview != LACE_SUBVIEW_DATA || !lace_object_has_subview(v->object, LACE_SUBVIEW_DATA))
    return;
  if (!v->schema) {
    lace_toast(GTK_WIDGET(v), "The table's structure is not loaded");
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
    char *title = g_strdup_printf("%s (%s)%s%s", fk->ref_table ? fk->ref_table : "?", cols, fk->name ? "  " : "",
                                  fk->name ? fk->name : "");
    GMenuItem *it = g_menu_item_new(title, NULL);
    g_menu_item_set_action_and_target(it, "view.follow-key", "t", (guint64)matches[k]);
    g_menu_append_item(menu, it);
    g_object_unref(it);
    g_free(title);
    free(cols);
  }
  popup_choices(v, menu);
  g_object_unref(menu);
}

void lace_table_view_referenced_by(LaceTableView *v) {
  if (v->subview != LACE_SUBVIEW_DATA || !lace_object_has_subview(v->object, LACE_SUBVIEW_DATA))
    return;
  gsize row = 0, col = 0;
  if (!lace_data_grid_get_focus_cell(v->grid, &row, &col)) {
    lace_toast(GTK_WIDGET(v), "No row to follow");
    return;
  }
  if (!v->deps_loaded) {
    if (v->deps_error) {
      lace_toast(GTK_WIDGET(v), "%s", v->deps_error);
      g_clear_pointer(&v->deps_error, g_free);
      return;
    }
    v->referenced_by_pending = TRUE;
    load_deps(v);
    return;
  }
  if (!v->num_deps) {
    lace_toast(GTK_WIDGET(v), "No table references %s", v->table);
    return;
  }
  GMenu *menu = g_menu_new();
  for (size_t i = 0; i < v->num_deps; i++) {
    const LaceDependent *d = &v->deps[i];
    char *cols = catalog_join(d->columns, d->num_columns);
    char *title = g_strdup_printf("%s (%s)%s%s", d->table ? d->table : "?", cols, d->on_delete ? "  ON DELETE " : "",
                                  d->on_delete ? d->on_delete : "");
    GMenuItem *it = g_menu_item_new(title, NULL);
    g_menu_item_set_action_and_target(it, "view.follow-dep", "t", (guint64)i);
    g_menu_append_item(menu, it);
    g_object_unref(it);
    g_free(title);
    free(cols);
  }
  popup_choices(v, menu);
  g_object_unref(menu);
}

static void on_follow_fk(LaceDataGrid *g, LaceTableView *v) {
  (void)g;
  lace_table_view_follow_fk(v);
}

static void on_referenced_by(LaceDataGrid *g, LaceTableView *v) {
  (void)g;
  lace_table_view_referenced_by(v);
}

void lace_table_view_show_filtered(LaceTableView *v, const LaceFilter *filters, gsize n) {
  v->restoring = TRUE;
  lace_filter_bar_set_filters(v->filters, filters, n, NULL);
  gtk_revealer_set_reveal_child(GTK_REVEALER(v->filter_revealer), TRUE);
  v->restoring = FALSE;
  if (v->subview != LACE_SUBVIEW_DATA)
    lace_table_view_set_subview(v, LACE_SUBVIEW_DATA);
  touched(v);
  if (v->schema)
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
    lace_filter_bar_add(v->filters, column, LACE_FILTER_EQ, value);
  else if (!value)
    lace_filter_bar_add(v->filters, column, LACE_FILTER_NULL, NULL);
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
  lace_table_view_set_subview(v, LACE_SUBVIEW_STRUCTURE);
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

const LaceFilter *lace_table_view_get_filters(LaceTableView *v, gsize *n) {
  return lace_filter_bar_get_filters(v->filters, n);
}

const char *lace_table_view_get_search(LaceTableView *v) {
  return lace_filter_bar_get_search(v->filters);
}

LaceSortSpec *lace_table_view_get_sorts(LaceTableView *v, gsize *n) {
  return lace_data_grid_get_sorts(v->grid, n);
}

gboolean lace_table_view_filters_visible(LaceTableView *v) {
  return gtk_revealer_get_reveal_child(GTK_REVEALER(v->filter_revealer));
}

void lace_table_view_restore(LaceTableView *v, const LaceFilter *filters, gsize n,
                             const char *search, const LaceSortSpec *sorts,
                             gsize num_sorts, gboolean filters_visible) {
  v->restoring = TRUE;
  lace_filter_bar_set_filters(v->filters, filters, n, search);
  gtk_revealer_set_reveal_child(GTK_REVEALER(v->filter_revealer),
                                filters_visible || n > 0 || (search && *search));
  /* Sorts need the columns, which arrive with the first page: keep them
   * and apply once the grid has columns */
  if (num_sorts > 0) {
    LaceSortSpec *copy = g_new0(LaceSortSpec, num_sorts);
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
  LaceSortSpec *pending = g_object_get_data(G_OBJECT(v), "pending-sorts");
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
  drop_catalog(v);
  g_clear_pointer(&v->table, g_free);
  g_clear_pointer(&v->object_table, g_free);
  g_clear_pointer(&v->object_detail, g_free);
  g_clear_pointer(&v->where, g_free);
  g_clear_pointer(&v->order_by, g_free);
  lace_edit_context_clear(&v->edit);
  G_OBJECT_CLASS(lace_table_view_parent_class)->dispose(obj);
}

static void lace_table_view_class_init(LaceTableViewClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = lace_table_view_dispose;
  signals[SIG_TOUCHED] = g_signal_new("touched", LACE_TYPE_TABLE_VIEW,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_OPEN_FILTERED] = g_signal_new("open-filtered", LACE_TYPE_TABLE_VIEW,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_UINT);
  signals[SIG_OPEN_OBJECT] = g_signal_new("open-object", LACE_TYPE_TABLE_VIEW,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 4, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "lace-tableview");
}

static void lace_table_view_init(LaceTableView *v) {
  v->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(v->box, GTK_WIDGET(v));

  /* The sub-view strip: an AdwViewSwitcher over the stack of Data and the lists */
  v->stack = adw_view_stack_new();
  gtk_widget_set_vexpand(v->stack, TRUE);
  v->switcher = adw_view_switcher_new();
  adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(v->switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
  adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(v->switcher), ADW_VIEW_STACK(v->stack));
  gtk_widget_set_halign(v->switcher, GTK_ALIGN_START);
  gtk_widget_set_margin_start(v->switcher, 6);
  gtk_widget_set_margin_top(v->switcher, 4);
  gtk_widget_set_margin_bottom(v->switcher, 4);
  gtk_widget_add_css_class(v->switcher, "subview-strip");
  gtk_box_append(GTK_BOX(v->box), v->switcher);

  v->banner = adw_banner_new("");
  gtk_box_append(GTK_BOX(v->box), v->banner);

  GtkWidget *data = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  v->filters = lace_filter_bar_new();
  v->filter_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(v->filter_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_child(GTK_REVEALER(v->filter_revealer), GTK_WIDGET(v->filters));
  gtk_box_append(GTK_BOX(data), v->filter_revealer);

  v->grid = lace_data_grid_new();
  gtk_widget_set_vexpand(GTK_WIDGET(v->grid), TRUE);
  gtk_box_append(GTK_BOX(data), GTK_WIDGET(v->grid));
  g_object_set_data(G_OBJECT(v), "data-page", data);
  gtk_box_append(GTK_BOX(v->box), v->stack);

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
  g_signal_connect(v->grid, "follow-fk", G_CALLBACK(on_follow_fk), v);
  g_signal_connect(v->grid, "referenced-by", G_CALLBACK(on_referenced_by), v);
  g_signal_connect(v->filters, "changed", G_CALLBACK(on_filters_changed), v);
}

/* The stack's pages for the object: Data (the grid page made in init) and a list per other sub-view */
static void add_pages(LaceTableView *v) {
  GtkWidget *data = g_object_get_data(G_OBJECT(v), "data-page");
  for (int i = 0; i < LACE_SUBVIEW_COUNT; i++) {
    LaceSubView sv = (LaceSubView)i;
    if (!lace_object_has_subview(v->object, sv))
      continue;
    GtkWidget *child;
    if (sv == LACE_SUBVIEW_DATA) {
      child = data;
    } else {
      v->lists[i] = lace_catalog_list_new(sv == LACE_SUBVIEW_STRUCTURE && v->object != LACE_OBJ_TABLE);
      g_signal_connect(v->lists[i], "activate-row", G_CALLBACK(on_list_activate), v);
      child = GTK_WIDGET(v->lists[i]);
    }
    AdwViewStackPage *page = adw_view_stack_add_titled_with_icon(
        ADW_VIEW_STACK(v->stack), child, lace_subview_key(sv, v->object), subview_label(sv, v->object),
        subview_icon(sv));
    (void)page;
  }
  g_signal_connect(v->stack, "notify::visible-child-name", G_CALLBACK(on_stack_page_changed), v);
}

static void act_view(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)p;
  LaceTableView *v = data;
  const char *name = g_action_get_name(G_ACTION(a));
  if (g_str_equal(name, "structure")) lace_table_view_show_structure(v);
  else if (g_str_equal(name, "follow-fk")) lace_table_view_follow_fk(v);
  else if (g_str_equal(name, "referenced-by")) lace_table_view_referenced_by(v);
  else if (g_str_equal(name, "goto-row")) lace_table_view_goto_row(v);
  else if (g_str_equal(name, "filter")) lace_table_view_toggle_filters(v);
  else if (g_str_equal(name, "refresh")) lace_table_view_refresh(v);
  else if (g_str_equal(name, "delete-rows")) lace_table_view_delete_selected(v);
  else if (g_str_equal(name, "add-row")) lace_table_view_add_row(v);
}

LaceTableView *lace_table_view_new(LaceConnection *conn, const char *table,
                                   const LacePrefs *prefs) {
  return lace_table_view_new_object(conn, LACE_OBJ_TABLE, table, NULL, NULL, prefs);
}

LaceTableView *lace_table_view_new_object(LaceConnection *conn, LaceObjectKind kind, const char *name,
                                          const char *table, const char *detail, const LacePrefs *prefs) {
  LaceTableView *v = g_object_new(LACE_TYPE_TABLE_VIEW, NULL);
  v->conn = g_object_ref(conn);
  v->table = g_strdup(name);
  v->object = kind;
  v->object_table = table ? g_strdup(table) : NULL;
  v->object_detail = detail ? g_strdup(detail) : NULL;
  v->info_count = -1;
  v->prefs = prefs;
  add_pages(v);
  v->subview = lace_object_has_subview(kind, LACE_SUBVIEW_DATA) ? LACE_SUBVIEW_DATA : LACE_SUBVIEW_STRUCTURE;
  adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(v->stack), lace_subview_key(v->subview, kind));
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
      {"follow-fk", act_view, NULL, NULL, NULL, {0}},
      {"referenced-by", act_view, NULL, NULL, NULL, {0}},
      {"follow-key", act_follow_key, "t", NULL, NULL, {0}},
      {"follow-dep", act_follow_dep, "t", NULL, NULL, {0}},
  };
  GSimpleActionGroup *group = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(group), entries, G_N_ELEMENTS(entries), v);
  gtk_widget_insert_action_group(GTK_WIDGET(v), "view", G_ACTION_GROUP(group));
  g_object_unref(group);

  if (lace_object_has_subview(kind, LACE_SUBVIEW_DATA))
    load_schema(v);
  else
    render_list(v);
  return v;
}
