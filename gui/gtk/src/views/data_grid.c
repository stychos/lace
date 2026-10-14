/*
 * Lace GTK frontend
 * LaceDataGrid
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "data_grid.h"
#include "cell_editor.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

#define CELL_MAX_CHARS 200
#define MEASURE_ROWS 300
#define MIN_COL_CHARS 4
#define MAX_COL_CHARS 60
#define ROW_NUMBER_WIDTH 64

struct _LaceDataGrid {
  GtkWidget parent_instance;

  GtkWidget *scrolled;
  GtkWidget *view;     /* GtkColumnView */
  GtkWidget *sample;   /* Hidden label with the cell font, for measuring */
  LaceCellEditor *editor;
  GtkWidget *cell_menu;   /* GtkPopoverMenu */
  GSimpleActionGroup *actions;

  LaceRowModel *model;
  GtkMultiSelection *selection;
  const LaceSchema *schema;
  gboolean editable;
  char *readonly_reason;
  gboolean sortable;
  gboolean row_numbers;

  GPtrArray *columns;   /* GtkColumnViewColumn*, data column i at index i */
  GtkColumnViewColumn *number_column;
  gboolean *is_pk;
  gsize num_columns;

  GHashTable *bound;    /* (row << 12 | col) -> label widget, while bound */
  GtkWidget *current_label; /* The widget carrying the "current" class */
  guint focus_idle;
  int focus_tries;
  gsize focus_row, focus_col;
  gboolean has_focus_cell;
  gboolean editing_row_valid;
  gsize editing_row, editing_col;
  gboolean suppress_sort;
  gulong sorter_handler;
};

enum { SIG_SORT_CHANGED, SIG_EDIT_CELL, SIG_DELETE_ROWS, SIG_CELL_FOCUSED,
       SIG_ADD_ROW, SIG_REFRESH, SIG_FILTER_COLUMN, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(LaceDataGrid, lace_data_grid, GTK_TYPE_WIDGET)

static void rebuild_columns(LaceDataGrid *g);

/* ==========================================================================
 * Cells
 * ========================================================================== */

static const LaceColumn *model_column(LaceDataGrid *g, gsize col) {
  return g->model ? lace_row_model_get_column(g->model, col) : NULL;
}

/* A cell is one focusable GtkInscription (a GtkLabel refuses focus unless
 * selectable, and the list view keeps up to 200 rows of cells alive, so
 * every widget and controller per cell counts) */
static GtkWidget *cell_label(GtkWidget *cell) { return cell; }

static void cell_update(GtkWidget *cell) {
  LaceRowItem *item = g_object_get_data(G_OBJECT(cell), "item");
  LaceDataGrid *g = g_object_get_data(G_OBJECT(cell), "grid");
  gsize col = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(cell), "col"));
  GtkWidget *label = cell_label(cell);
  if (!item || !g) {
    gtk_inscription_set_text(GTK_INSCRIPTION(label), "");
    return;
  }
  if (col == (gsize)-1) { /* Row number */
    char *n = lace_format_count(lace_row_item_get_position(item) + 1);
    gtk_inscription_set_text(GTK_INSCRIPTION(label), n);
    g_free(n);
    return;
  }
  const LaceValue *v = lace_row_item_get_cell(item, col);
  gtk_widget_remove_css_class(cell, "null");
  gtk_widget_remove_css_class(cell, "placeholder");
  if (!lace_row_item_is_loaded(item)) {
    gtk_inscription_set_text(GTK_INSCRIPTION(label), "");
    gtk_widget_add_css_class(cell, "placeholder");
    return;
  }
  if (!v || v->is_null || v->type == LACE_TYPE_NULL) {
    gtk_inscription_set_text(GTK_INSCRIPTION(label), "NULL");
    gtk_widget_add_css_class(cell, "null");
    return;
  }
  char *text = sql_display_text(v, CELL_MAX_CHARS);
  gtk_inscription_set_text(GTK_INSCRIPTION(label), text);
  free(text);
}

static void on_item_changed(LaceRowItem *item, GtkWidget *label) {
  (void)item;
  cell_update(label);
}

static gpointer bound_key(gsize row, gsize col);

/* The "current" class marks the active cell's widget (the :focus state
 * alone is not enough: the row keeps its own focus ring and the cell must
 * stay marked while focus is elsewhere, e.g. in the filter bar) */
static void mark_current(LaceDataGrid *g) {
  GtkWidget *label = g->bound && g->has_focus_cell
                         ? g_hash_table_lookup(g->bound, bound_key(g->focus_row, g->focus_col))
                         : NULL;
  if (g->current_label == label)
    return;
  if (g->current_label)
    gtk_widget_remove_css_class(g->current_label, "current");
  g->current_label = label;
  if (label)
    gtk_widget_add_css_class(label, "current");
}

static void set_focus_cell(LaceDataGrid *g, gsize row, gsize col) {
  g->focus_row = row;
  g->focus_col = col;
  g->has_focus_cell = TRUE;
  mark_current(g);
  g_signal_emit(g, signals[SIG_CELL_FOCUSED], 0, (guint)row, (guint)col);
}

static void show_cell_menu(LaceDataGrid *g, double x, double y, GtkWidget *from);

/* A plain label takes no focus on click, and the row's own gesture claims
 * the press before a label gesture sees it. So the view watches presses in
 * the capture phase, finds the cell under the pointer, and focuses it once
 * the row has done its selection handling. */
static GtkColumnViewColumn *column_at(LaceDataGrid *g, gsize col);

typedef struct {
  LaceDataGrid *grid;
  gsize row, col;
} ClickFocus;

static gboolean focus_clicked_idle(gpointer data) {
  ClickFocus *cf = data;
  LaceDataGrid *g = cf->grid;
  if (g->view && !(g->editor && gtk_widget_get_visible(GTK_WIDGET(g->editor)))) {
    GtkWidget *cell = g->bound ? g_hash_table_lookup(g->bound, bound_key(cf->row, cf->col)) : NULL;
    if (cell && gtk_widget_get_mapped(cell))
      gtk_widget_grab_focus(cell);
  }
  g_object_unref(g);
  g_free(cf);
  return G_SOURCE_REMOVE;
}

static void on_view_pressed(GtkGestureClick *gesture, int n_press, double x,
                            double y, LaceDataGrid *g) {
  (void)n_press;
  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  GtkWidget *w = gtk_widget_pick(g->view, x, y, GTK_PICK_DEFAULT);
  while (w && w != g->view && g_object_get_data(G_OBJECT(w), "grid") != g)
    w = gtk_widget_get_parent(w);
  if (!w || w == g->view)
    return;
  gsize col = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(w), "col"));
  LaceRowItem *item = g_object_get_data(G_OBJECT(w), "item");
  if (col == (gsize)-1 || !item)
    return; /* The row number, or an unbound cell */
  gsize row = lace_row_item_get_position(item);
  if (button == GDK_BUTTON_SECONDARY) {
    /* The menu acts on the current cell: make it this one now */
    set_focus_cell(g, row, col);
    if (g->selection && !gtk_selection_model_is_selected(GTK_SELECTION_MODEL(g->selection), (guint)row))
      gtk_selection_model_select_item(GTK_SELECTION_MODEL(g->selection), (guint)row, TRUE);
    if (gtk_widget_get_mapped(w))
      gtk_widget_grab_focus(w);
    show_cell_menu(g, x, y, g->view);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    return;
  }
  if (button != GDK_BUTTON_PRIMARY)
    return;
  ClickFocus *cf = g_new0(ClickFocus, 1);
  cf->grid = g_object_ref(g);
  cf->row = row;
  cf->col = col;
  g_idle_add(focus_clicked_idle, cf);
}

static void cell_setup(GtkSignalListItemFactory *f, GtkListItem *li, LaceDataGrid *g) {
  (void)f;
  gsize col = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(f), "col"));
  GtkWidget *cell = gtk_inscription_new("");
  gtk_inscription_set_text_overflow(GTK_INSCRIPTION(cell), GTK_INSCRIPTION_OVERFLOW_ELLIPSIZE_END);
  gtk_inscription_set_min_lines(GTK_INSCRIPTION(cell), 1);
  gtk_inscription_set_nat_chars(GTK_INSCRIPTION(cell), 0);
  gtk_inscription_set_xalign(GTK_INSCRIPTION(cell), 0.0f);
  gtk_widget_add_css_class(cell, "cell");
  gtk_widget_set_focusable(cell, col != (gsize)-1);
  if (col == (gsize)-1) {
    gtk_widget_add_css_class(cell, "rownum");
    gtk_inscription_set_xalign(GTK_INSCRIPTION(cell), 1.0f);
  } else {
    const LaceColumn *c = model_column(g, col);
    if (c) {
      gtk_widget_add_css_class(cell, lace_type_css_class(c->type));
      if (sql_type_is_numeric(c->type))
        gtk_inscription_set_xalign(GTK_INSCRIPTION(cell), 1.0f);
    }
    if (g->is_pk && col < g->num_columns && g->is_pk[col])
      gtk_widget_add_css_class(cell, "pk");
  }
  g_object_set_data(G_OBJECT(cell), "grid", g);
  g_object_set_data(G_OBJECT(cell), "col", GSIZE_TO_POINTER(col));
  gtk_list_item_set_child(li, cell);
  gtk_list_item_set_focusable(li, FALSE);
}

static gpointer bound_key(gsize row, gsize col) {
  return GSIZE_TO_POINTER((row << 12) | (col & 0xfff));
}

/* Zebra stripes by row position: the list view recycles row widgets out of
 * order while scrolling, so CSS nth-child parity drifts */
static void stripe_row(GtkWidget *cell, gsize position) {
  GtkWidget *row = gtk_widget_get_parent(cell);
  while (row && !g_str_equal(gtk_widget_get_css_name(row), "row"))
    row = gtk_widget_get_parent(row);
  if (!row)
    return;
  if (position % 2)
    gtk_widget_add_css_class(row, "odd");
  else
    gtk_widget_remove_css_class(row, "odd");
}

static void cell_bind(GtkSignalListItemFactory *f, GtkListItem *li, LaceDataGrid *g) {
  (void)f;
  GtkWidget *label = gtk_list_item_get_child(li);
  LaceRowItem *item = gtk_list_item_get_item(li);
  g_object_set_data(G_OBJECT(label), "item", item);
  stripe_row(label, lace_row_item_get_position(item));
  gsize col = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(label), "col"));
  if (col != (gsize)-1 && g->bound) {
    g_hash_table_insert(g->bound, bound_key(lace_row_item_get_position(item), col), label);
    if (g->has_focus_cell && lace_row_item_get_position(item) == g->focus_row && col == g->focus_col) {
      if (g->current_label && g->current_label != label)
        gtk_widget_remove_css_class(g->current_label, "current");
      g->current_label = label;
      gtk_widget_add_css_class(label, "current");
    } else if (label == g->current_label) {
      gtk_widget_remove_css_class(label, "current");
      g->current_label = NULL;
    }
  }
  gulong id = g_signal_connect(item, "changed", G_CALLBACK(on_item_changed), label);
  g_object_set_data(G_OBJECT(label), "handler", GSIZE_TO_POINTER(id));
  cell_update(label);
}

static void cell_unbind(GtkSignalListItemFactory *f, GtkListItem *li, LaceDataGrid *g) {
  (void)f;
  GtkWidget *label = gtk_list_item_get_child(li);
  LaceRowItem *item = g_object_get_data(G_OBJECT(label), "item");
  gsize col = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(label), "col"));
  if (item && col != (gsize)-1 && g->bound &&
      g_hash_table_lookup(g->bound, bound_key(lace_row_item_get_position(item), col)) == label)
    g_hash_table_remove(g->bound, bound_key(lace_row_item_get_position(item), col));
  if (label == g->current_label) {
    gtk_widget_remove_css_class(label, "current");
    g->current_label = NULL;
  }
  gulong id = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(label), "handler"));
  if (item && id)
    g_signal_handler_disconnect(item, id);
  g_object_set_data(G_OBJECT(label), "item", NULL);
  g_object_set_data(G_OBJECT(label), "handler", NULL);
}

/* ==========================================================================
 * Columns and sorting
 * ========================================================================== */

static int noop_compare(gconstpointer a, gconstpointer b, gpointer data) {
  (void)a;
  (void)b;
  (void)data;
  return 0;
}

static void on_sorter_changed(GtkSorter *sorter, GtkSorterChange change, LaceDataGrid *g) {
  (void)sorter;
  (void)change;
  if (g->suppress_sort)
    return;
  g_signal_emit(g, signals[SIG_SORT_CHANGED], 0);
}

static GMenuModel *header_menu(gsize col) {
  GMenu *menu = g_menu_new();
  GMenuItem *it;
  it = g_menu_item_new("Sort ascending", NULL);
  g_menu_item_set_action_and_target(it, "grid.sort-asc", "t", (guint64)col);
  g_menu_append_item(menu, it);
  g_object_unref(it);
  it = g_menu_item_new("Sort descending", NULL);
  g_menu_item_set_action_and_target(it, "grid.sort-desc", "t", (guint64)col);
  g_menu_append_item(menu, it);
  g_object_unref(it);
  g_menu_append(menu, "Clear sorting", "grid.sort-clear");
  GMenu *more = g_menu_new();
  it = g_menu_item_new("Filter by this column…", NULL);
  g_menu_item_set_action_and_target(it, "grid.filter-column", "t", (guint64)col);
  g_menu_append_item(more, it);
  g_object_unref(it);
  it = g_menu_item_new("Copy column name", NULL);
  g_menu_item_set_action_and_target(it, "grid.copy-column-name", "t", (guint64)col);
  g_menu_append_item(more, it);
  g_object_unref(it);
  it = g_menu_item_new("Fit column width", NULL);
  g_menu_item_set_action_and_target(it, "grid.fit-column", "t", (guint64)col);
  g_menu_append_item(more, it);
  g_object_unref(it);
  g_menu_append_section(menu, NULL, G_MENU_MODEL(more));
  g_object_unref(more);
  return G_MENU_MODEL(menu);
}

static void clear_columns(LaceDataGrid *g) {
  if (g->number_column) {
    gtk_column_view_remove_column(GTK_COLUMN_VIEW(g->view), g->number_column);
    g->number_column = NULL;
  }
  for (guint i = 0; i < g->columns->len; i++)
    gtk_column_view_remove_column(GTK_COLUMN_VIEW(g->view), g_ptr_array_index(g->columns, i));
  g_ptr_array_set_size(g->columns, 0);
  if (g->bound)
    g_hash_table_remove_all(g->bound);
  g->current_label = NULL;
  g_free(g->is_pk);
  g->is_pk = NULL;
  g->num_columns = 0;
  g->has_focus_cell = FALSE;
}

static void compute_pk(LaceDataGrid *g) {
  g_free(g->is_pk);
  g->is_pk = g_new0(gboolean, g->num_columns ? g->num_columns : 1);
  if (!g->schema || !g->model)
    return;
  for (gsize i = 0; i < g->num_columns; i++) {
    const LaceColumn *c = lace_row_model_get_column(g->model, i);
    for (size_t k = 0; c && k < g->schema->num_columns; k++) {
      if (g->schema->columns[k].primary_key && g->schema->columns[k].name &&
          c->name && strcmp(g->schema->columns[k].name, c->name) == 0)
        g->is_pk[i] = TRUE;
    }
  }
}

static GtkColumnViewColumn *make_column(LaceDataGrid *g, gsize col, const char *title) {
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_object_set_data(G_OBJECT(factory), "col", GSIZE_TO_POINTER(col));
  g_signal_connect(factory, "setup", G_CALLBACK(cell_setup), g);
  g_signal_connect(factory, "bind", G_CALLBACK(cell_bind), g);
  g_signal_connect(factory, "unbind", G_CALLBACK(cell_unbind), g);
  GtkColumnViewColumn *column = gtk_column_view_column_new(title, factory);
  gtk_column_view_column_set_resizable(column, col != (gsize)-1);
  gtk_column_view_column_set_expand(column, FALSE);
  g_object_set_data(G_OBJECT(column), "col", GSIZE_TO_POINTER(col));
  return column;
}

static void rebuild_columns(LaceDataGrid *g) {
  clear_columns(g);
  if (!g->model)
    return;
  g->num_columns = lace_row_model_n_columns(g->model);
  compute_pk(g);
  g->suppress_sort = TRUE;
  if (g->row_numbers) {
    g->number_column = make_column(g, (gsize)-1, "#");
    gtk_column_view_column_set_fixed_width(g->number_column, ROW_NUMBER_WIDTH);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(g->view), g->number_column);
  }
  for (gsize i = 0; i < g->num_columns; i++) {
    const LaceColumn *c = lace_row_model_get_column(g->model, i);
    char *title = g_strdup_printf("%s%s", c->name ? c->name : "", g->is_pk[i] ? "  ⚿" : "");
    GtkColumnViewColumn *column = make_column(g, i, title);
    g_free(title);
    g_object_set_data_full(G_OBJECT(column), "name", g_strdup(c->name ? c->name : ""), g_free);
    g_object_set_data(G_OBJECT(column), "type", GINT_TO_POINTER((int)c->type));
    gtk_column_view_column_set_fixed_width(column, 140);
    if (g->sortable) {
      GtkSorter *s = GTK_SORTER(gtk_custom_sorter_new(noop_compare, NULL, NULL));
      gtk_column_view_column_set_sorter(column, s);
      g_object_unref(s);
    }
    GMenuModel *menu = header_menu(i);
    gtk_column_view_column_set_header_menu(column, menu);
    g_object_unref(menu);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(g->view), column);
    g_ptr_array_add(g->columns, column);
  }
  g->suppress_sort = FALSE;
}

/* Keep the columns (their widths, order and sort state) when a reload
 * brings back the same column set; rebuild only when it differs. A reset
 * leaves the model without columns until its first page: nothing to do. */
static gboolean same_columns(LaceDataGrid *g) {
  gsize n = lace_row_model_n_columns(g->model);
  if (n != g->num_columns || n == 0)
    return FALSE;
  for (gsize i = 0; i < n; i++) {
    const LaceColumn *c = lace_row_model_get_column(g->model, i);
    GtkColumnViewColumn *column = g_ptr_array_index(g->columns, i);
    const char *name = g_object_get_data(G_OBJECT(column), "name");
    LaceValueType type = (LaceValueType)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "type"));
    if (g_strcmp0(name, c->name) != 0 || type != c->type)
      return FALSE;
  }
  return TRUE;
}

static int chars_to_px(LaceDataGrid *g, int chars);

/* The row number column fits the largest number it will show */
static void on_total_changed(LaceRowModel *m, LaceDataGrid *g) {
  if (!g->number_column)
    return;
  char *n = lace_format_count(lace_row_model_get_total(m));
  int chars = (int)strlen(n) + 1;
  g_free(n);
  if (chars < 4)
    chars = 4;
  int px = chars_to_px(g, chars);
  if (px > gtk_column_view_column_get_fixed_width(g->number_column))
    gtk_column_view_column_set_fixed_width(g->number_column, px);
}

static void on_columns_changed(LaceRowModel *m, LaceDataGrid *g) {
  if (lace_row_model_n_columns(m) == 0)
    return;
  if (same_columns(g)) {
    compute_pk(g);
    return;
  }
  rebuild_columns(g);
}

/* Width in pixels of n characters of the cell font */
static int chars_to_px(LaceDataGrid *g, int chars) {
  PangoLayout *layout = gtk_widget_create_pango_layout(g->sample, "0");
  int w = 8, h;
  pango_layout_get_pixel_size(layout, &w, &h);
  g_object_unref(layout);
  if (w < 4)
    w = 8;
  return chars * w + 22;
}

static int utf8_chars(const char *s) {
  return s ? (int)g_utf8_strlen(s, -1) : 0;
}

static void fit_column(LaceDataGrid *g, gsize col) {
  if (!g->model || col >= g->columns->len)
    return;
  const LaceColumn *c = lace_row_model_get_column(g->model, col);
  int chars = utf8_chars(c && c->name ? c->name : "") + 2;
  gsize n = lace_row_model_get_total(g->model);
  gsize checked = 0;
  for (gsize r = 0; r < n && checked < MEASURE_ROWS; r++) {
    if (!lace_row_model_row_loaded(g->model, r)) {
      /* Skip to the next loaded page */
      gsize ps = lace_row_model_get_page_size(g->model);
      r = (r / ps + 1) * ps - 1;
      continue;
    }
    checked++;
    const LaceValue *v = lace_row_model_get_cell(g->model, r, col);
    char *text = sql_display_text(v, MAX_COL_CHARS + 1);
    int len = utf8_chars(text);
    free(text);
    if (len > chars)
      chars = len;
  }
  if (chars < MIN_COL_CHARS)
    chars = MIN_COL_CHARS;
  if (chars > MAX_COL_CHARS)
    chars = MAX_COL_CHARS;
  gtk_column_view_column_set_fixed_width(g_ptr_array_index(g->columns, col),
                                         chars_to_px(g, chars));
}

void lace_data_grid_measure_columns(LaceDataGrid *g) {
  for (gsize i = 0; i < g->columns->len; i++)
    fit_column(g, i);
}

/* ==========================================================================
 * Focus, keyboard, editing
 * ========================================================================== */

static GtkColumnViewColumn *column_at(LaceDataGrid *g, gsize col) {
  return col < g->columns->len ? g_ptr_array_index(g->columns, col) : NULL;
}

/* Keyboard focus to the widget of the focused cell, once it is bound (the
 * scroll realises it on the next frame) */
static gboolean grab_focus_idle(gpointer data) {
  LaceDataGrid *g = data;
  g->focus_idle = 0;
  if (!g->has_focus_cell || !g->bound)
    return G_SOURCE_REMOVE;
  GtkWidget *label = g_hash_table_lookup(g->bound, bound_key(g->focus_row, g->focus_col));
  if (label && gtk_widget_get_mapped(label)) {
    gtk_widget_grab_focus(label);
    g->focus_tries = 0;
  } else if (g->focus_tries++ < 20) {
    /* The scroll realises the cell over the next frames */
    g->focus_idle = g_timeout_add(40, grab_focus_idle, g);
  } else {
    g->focus_tries = 0;
  }
  return G_SOURCE_REMOVE;
}

void lace_data_grid_focus_cell(LaceDataGrid *g, gsize row, gsize col) {
  if (!g->model)
    return;
  gsize n = g_list_model_get_n_items(G_LIST_MODEL(g->model));
  if (n == 0)
    return;
  if (row >= n)
    row = n - 1;
  if (col >= g->columns->len)
    col = g->columns->len ? g->columns->len - 1 : 0;
  gtk_column_view_scroll_to(GTK_COLUMN_VIEW(g->view), (guint)row, column_at(g, col),
                            GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT, NULL);
  set_focus_cell(g, row, col);
  GtkWidget *label = g_hash_table_lookup(g->bound, bound_key(row, col));
  if (label && gtk_widget_get_mapped(label))
    gtk_widget_grab_focus(label);
  else if (!g->focus_idle)
    g->focus_idle = g_idle_add(grab_focus_idle, g);
}

void lace_data_grid_scroll_to_row(LaceDataGrid *g, gsize row) {
  gsize col = g->has_focus_cell ? g->focus_col : 0;
  lace_data_grid_focus_cell(g, row, col);
}

gboolean lace_data_grid_get_focus_cell(LaceDataGrid *g, gsize *row, gsize *col) {
  if (!g->has_focus_cell)
    return FALSE;
  if (row)
    *row = g->focus_row;
  if (col)
    *col = g->focus_col;
  return TRUE;
}

/* The label widget showing the focused cell, if bound */
static GtkWidget *focused_cell_widget(LaceDataGrid *g) {
  if (!g->has_focus_cell || !g->bound)
    return NULL;
  GtkWidget *label = g_hash_table_lookup(g->bound, bound_key(g->focus_row, g->focus_col));
  return label && gtk_widget_get_mapped(label) ? label : NULL;
}

static gsize visible_rows(LaceDataGrid *g) {
  int h = gtk_widget_get_height(g->scrolled);
  int row_h = 30;
  GtkWidget *cell = focused_cell_widget(g);
  if (cell) {
    GtkWidget *p = gtk_widget_get_parent(cell);
    if (p && gtk_widget_get_height(p) > 8)
      row_h = gtk_widget_get_height(p);
  }
  gsize n = h > 0 ? (gsize)(h / row_h) : 20;
  return n > 1 ? n - 1 : 1;
}

static gboolean grab_focus_idle(gpointer data);

static void open_editor_at(LaceDataGrid *g, gsize row, gsize col) {
  if (!g->model || !lace_row_model_row_loaded(g->model, row))
    return;
  GtkWidget *cell = focused_cell_widget(g);
  GdkRectangle rect = {0, 0, 1, 1};
  if (cell) {
    graphene_rect_t b;
    if (gtk_widget_compute_bounds(cell, GTK_WIDGET(g), &b)) {
      rect.x = (int)b.origin.x;
      rect.y = (int)b.origin.y;
      rect.width = (int)b.size.width;
      rect.height = (int)b.size.height;
    }
  }
  const LaceColumn *c = model_column(g, col);
  const LaceValue *v = lace_row_model_get_cell(g->model, row, col);
  gboolean editable = g->editable && !(v && v->omitted);
  g->editing_row = row;
  g->editing_col = col;
  g->editing_row_valid = TRUE;
  lace_cell_editor_open(g->editor, &rect, v, c ? c->name : "", c ? c->type : LACE_TYPE_TEXT,
                        c ? c->type_name : NULL, editable);
}

void lace_data_grid_edit_focused(LaceDataGrid *g) {
  if (g->has_focus_cell)
    open_editor_at(g, g->focus_row, g->focus_col);
}

static void on_editor_committed(LaceCellEditor *ed, const char *text, gboolean set_null, LaceDataGrid *g) {
  (void)ed;
  if (!g->editing_row_valid)
    return;
  g_signal_emit(g, signals[SIG_EDIT_CELL], 0, (guint)g->editing_row, (guint)g->editing_col,
                set_null ? NULL : text, set_null);
}

static void on_editor_closed(LaceCellEditor *ed, LaceDataGrid *g) {
  (void)ed;
  /* Focus goes back to the cell, after the popover's own focus restore */
  if (g->has_focus_cell && !g->focus_idle)
    g->focus_idle = g_timeout_add(60, grab_focus_idle, g);
}

static void on_row_activated(GtkColumnView *view, guint position, LaceDataGrid *g) {
  (void)view;
  gsize col = g->has_focus_cell ? g->focus_col : 0;
  g->focus_row = position;
  open_editor_at(g, position, col);
}

void lace_data_grid_copy_focused(LaceDataGrid *g, gboolean whole_row) {
  if (!g->has_focus_cell || !g->model)
    return;
  if (!whole_row) {
    const LaceValue *v = lace_row_model_get_cell(g->model, g->focus_row, g->focus_col);
    char *t = v ? lace_value_to_edit_text(v) : NULL;
    lace_copy_text(GTK_WIDGET(g), t ? t : "");
    free(t);
    return;
  }
  GString *s = g_string_new(NULL);
  gsize n;
  gsize *rows = lace_data_grid_get_selected_rows(g, &n);
  if (n == 0) {
    g_free(rows);
    rows = g_new(gsize, 1);
    rows[0] = g->focus_row;
    n = 1;
  }
  for (gsize r = 0; r < n; r++) {
    for (gsize c = 0; c < g->num_columns; c++) {
      const LaceValue *v = lace_row_model_get_cell(g->model, rows[r], c);
      if (c > 0)
        g_string_append_c(s, '\t');
      if (v && !v->is_null && v->type != LACE_TYPE_NULL) {
        char *t = lace_value_to_edit_text(v);
        g_string_append(s, t ? t : "");
        free(t);
      }
    }
    g_string_append_c(s, '\n');
  }
  g_free(rows);
  lace_copy_text(GTK_WIDGET(g), s->str);
  g_string_free(s, TRUE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode,
                       GdkModifierType state, LaceDataGrid *g) {
  (void)c;
  (void)keycode;
  gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
  gboolean shift = (state & GDK_SHIFT_MASK) != 0;
  if (!g->model || !g->has_focus_cell)
    return FALSE;
  gsize n = g_list_model_get_n_items(G_LIST_MODEL(g->model));
  if (n == 0)
    return FALSE;
  gsize row = g->focus_row, col = g->focus_col;
  gsize page = visible_rows(g);
  gboolean move = TRUE;
  switch (keyval) {
  case GDK_KEY_Up: case GDK_KEY_KP_Up:
    row = ctrl ? 0 : (row > 0 ? row - 1 : 0);
    break;
  case GDK_KEY_Down: case GDK_KEY_KP_Down:
    row = ctrl ? n - 1 : (row + 1 < n ? row + 1 : n - 1);
    break;
  case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up:
    row = row > page ? row - page : 0;
    break;
  case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down:
    row = row + page < n ? row + page : n - 1;
    break;
  case GDK_KEY_Home: case GDK_KEY_KP_Home:
    if (ctrl) row = 0; else col = 0;
    break;
  case GDK_KEY_End: case GDK_KEY_KP_End:
    if (ctrl) row = n - 1; else col = g->columns->len ? g->columns->len - 1 : 0;
    break;
  case GDK_KEY_Left: case GDK_KEY_KP_Left:
    col = col > 0 ? col - 1 : 0;
    break;
  case GDK_KEY_Right: case GDK_KEY_KP_Right:
    col = col + 1 < g->columns->len ? col + 1 : col;
    break;
  case GDK_KEY_Tab:
    if (shift) col = col > 0 ? col - 1 : 0; else col = col + 1 < g->columns->len ? col + 1 : col;
    break;
  default:
    move = FALSE;
  }
  if (move) {
    GtkListScrollFlags flags = GTK_LIST_SCROLL_FOCUS;
    if (!ctrl && !shift)
      flags |= GTK_LIST_SCROLL_SELECT;
    gtk_column_view_scroll_to(GTK_COLUMN_VIEW(g->view), (guint)row, column_at(g, col), flags, NULL);
    GtkWidget *label = g_hash_table_lookup(g->bound, bound_key(row, col));
    if (label && gtk_widget_get_mapped(label))
      gtk_widget_grab_focus(label);
    else if (!g->focus_idle)
      g->focus_idle = g_idle_add(grab_focus_idle, g);
    if (shift && g->selection) {
      guint lo = (guint)MIN(row, g->focus_row), hi = (guint)MAX(row, g->focus_row);
      gtk_selection_model_select_range(GTK_SELECTION_MODEL(g->selection), lo, hi - lo + 1, FALSE);
    }
    set_focus_cell(g, row, col);
    return TRUE;
  }
  switch (keyval) {
  case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_F2:
    open_editor_at(g, row, col);
    return TRUE;
  case GDK_KEY_Delete: case GDK_KEY_KP_Delete:
    if (!ctrl && !shift) {
      g_signal_emit(g, signals[SIG_DELETE_ROWS], 0);
      return TRUE;
    }
    break;
  case GDK_KEY_c: case GDK_KEY_C:
    if (ctrl) {
      lace_data_grid_copy_focused(g, shift);
      return TRUE;
    }
    break;
  case GDK_KEY_Escape:
    if (g->selection)
      gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(g->selection));
    return TRUE;
  default:
    break;
  }
  /* A printable character starts an edit with that character, as in a
   * spreadsheet */
  gunichar ch = gdk_keyval_to_unicode(keyval);
  if (ch && g_unichar_isprint(ch) && !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) &&
      g->editable && lace_row_model_row_loaded(g->model, row)) {
    char utf8[8] = {0};
    g_unichar_to_utf8(ch, utf8);
    open_editor_at(g, row, col);
    lace_cell_editor_set_text(g->editor, utf8);
    return TRUE;
  }
  return FALSE;
}

/* ==========================================================================
 * Context menu and actions
 * ========================================================================== */

static void show_cell_menu(LaceDataGrid *g, double x, double y, GtkWidget *from) {
  graphene_point_t p = {(float)x, (float)y}, out;
  if (!gtk_widget_compute_point(from, GTK_WIDGET(g), &p, &out))
    out = p;
  GdkRectangle rect = {(int)out.x, (int)out.y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(g->cell_menu), &rect);
  gtk_popover_popup(GTK_POPOVER(g->cell_menu));
}

static void act_edit(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  lace_data_grid_edit_focused(LACE_DATA_GRID(data));
}

static void act_set_null(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceDataGrid *g = data;
  if (g->has_focus_cell && g->editable)
    g_signal_emit(g, signals[SIG_EDIT_CELL], 0, (guint)g->focus_row, (guint)g->focus_col, NULL, TRUE);
}

static void act_set_empty(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceDataGrid *g = data;
  if (g->has_focus_cell && g->editable)
    g_signal_emit(g, signals[SIG_EDIT_CELL], 0, (guint)g->focus_row, (guint)g->focus_col, "", FALSE);
}

static void act_copy_cell(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  lace_data_grid_copy_focused(LACE_DATA_GRID(data), FALSE);
}

static void act_copy_row(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  lace_data_grid_copy_focused(LACE_DATA_GRID(data), TRUE);
}

static void act_copy_column_name(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a;
  LaceDataGrid *g = data;
  gsize col = p ? (gsize)g_variant_get_uint64(p) : g->focus_col;
  const LaceColumn *c = model_column(g, col);
  if (c)
    lace_copy_text(GTK_WIDGET(g), c->name);
}

static void act_filter_value(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceDataGrid *g = data;
  if (!g->has_focus_cell || !g->model)
    return;
  const LaceColumn *c = model_column(g, g->focus_col);
  const LaceValue *v = lace_row_model_get_cell(g->model, g->focus_row, g->focus_col);
  char *t = v ? lace_value_to_edit_text(v) : NULL;
  gboolean is_null = !v || v->is_null || v->type == LACE_TYPE_NULL;
  g_signal_emit(g, signals[SIG_FILTER_COLUMN], 0, c ? c->name : "", is_null ? NULL : t);
  free(t);
}

static void act_filter_column(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a;
  LaceDataGrid *g = data;
  gsize col = p ? (gsize)g_variant_get_uint64(p) : g->focus_col;
  const LaceColumn *c = model_column(g, col);
  g_signal_emit(g, signals[SIG_FILTER_COLUMN], 0, c ? c->name : "", "");
}

static void act_sort(LaceDataGrid *g, GVariant *p, GtkSortType dir) {
  gsize col = (gsize)g_variant_get_uint64(p);
  GtkColumnViewColumn *column = column_at(g, col);
  if (column && g->sortable)
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(g->view), column, dir);
}

static void act_sort_asc(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a;
  act_sort(LACE_DATA_GRID(data), p, GTK_SORT_ASCENDING);
}

static void act_sort_desc(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a;
  act_sort(LACE_DATA_GRID(data), p, GTK_SORT_DESCENDING);
}

static void act_sort_clear(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceDataGrid *g = data;
  if (g->sortable)
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(g->view), NULL, GTK_SORT_ASCENDING);
}

static void act_fit_column(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a;
  fit_column(LACE_DATA_GRID(data), (gsize)g_variant_get_uint64(p));
}

static void act_delete(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  g_signal_emit(data, signals[SIG_DELETE_ROWS], 0);
}

static void act_add_row(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  g_signal_emit(data, signals[SIG_ADD_ROW], 0);
}

static void act_refresh(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  g_signal_emit(data, signals[SIG_REFRESH], 0);
}

static void act_select_all(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceDataGrid *g = data;
  if (g->selection)
    gtk_selection_model_select_all(GTK_SELECTION_MODEL(g->selection));
}

static GMenuModel *cell_menu_model(void) {
  GMenu *menu = g_menu_new();
  GMenu *s1 = g_menu_new();
  g_menu_append(s1, "Edit cell…", "grid.edit");
  g_menu_append(s1, "Set NULL", "grid.set-null");
  g_menu_append(s1, "Set empty", "grid.set-empty");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(s1));
  GMenu *s2 = g_menu_new();
  g_menu_append(s2, "Copy cell", "grid.copy-cell");
  g_menu_append(s2, "Copy row(s) as TSV", "grid.copy-row");
  g_menu_append(s2, "Copy column name", "grid.copy-column-name-focused");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(s2));
  GMenu *s3 = g_menu_new();
  g_menu_append(s3, "Filter by this value", "grid.filter-value");
  g_menu_append(s3, "Select all", "grid.select-all");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(s3));
  GMenu *s4 = g_menu_new();
  g_menu_append(s4, "Add row…", "grid.add-row");
  g_menu_append(s4, "Delete selected rows", "grid.delete-rows");
  g_menu_append(s4, "Refresh", "grid.refresh");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(s4));
  g_object_unref(s1);
  g_object_unref(s2);
  g_object_unref(s3);
  g_object_unref(s4);
  return G_MENU_MODEL(menu);
}

static void update_action_states(LaceDataGrid *g) {
  const char *write_actions[] = {"edit", "set-null", "set-empty", "add-row", "delete-rows", NULL};
  for (int i = 0; write_actions[i]; i++) {
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(g->actions), write_actions[i]);
    if (a)
      g_simple_action_set_enabled(G_SIMPLE_ACTION(a), g->editable);
  }
  /* Editing a cell is always allowed as a viewer */
  GAction *a = g_action_map_lookup_action(G_ACTION_MAP(g->actions), "edit");
  if (a)
    g_simple_action_set_enabled(G_SIMPLE_ACTION(a), TRUE);
}

/* ==========================================================================
 * Model and selection
 * ========================================================================== */

void lace_data_grid_set_model(LaceDataGrid *g, LaceRowModel *model) {
  if (g->model == model)
    return;
  if (g->model) {
    g_signal_handlers_disconnect_by_func(g->model, on_columns_changed, g);
    g_signal_handlers_disconnect_by_func(g->model, on_total_changed, g);
    clear_columns(g);
  }
  g_clear_object(&g->selection);
  g_set_object(&g->model, model);
  if (model) {
    g->selection = gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(model)));
    gtk_column_view_set_model(GTK_COLUMN_VIEW(g->view), GTK_SELECTION_MODEL(g->selection));
    g_signal_connect(model, "columns-changed", G_CALLBACK(on_columns_changed), g);
    g_signal_connect(model, "total-changed", G_CALLBACK(on_total_changed), g);
    rebuild_columns(g);
  } else {
    gtk_column_view_set_model(GTK_COLUMN_VIEW(g->view), NULL);
  }
}

LaceRowModel *lace_data_grid_get_model(LaceDataGrid *g) { return g->model; }

void lace_data_grid_set_schema(LaceDataGrid *g, const LaceSchema *schema) {
  g->schema = schema;
  if (g->model && g->num_columns > 0) {
    compute_pk(g);
    /* Retitle for the key marks */
    for (gsize i = 0; i < g->columns->len; i++) {
      const LaceColumn *c = lace_row_model_get_column(g->model, i);
      char *title = g_strdup_printf("%s%s", c && c->name ? c->name : "", g->is_pk[i] ? "  ⚿" : "");
      gtk_column_view_column_set_title(g_ptr_array_index(g->columns, i), title);
      g_free(title);
    }
  }
}

void lace_data_grid_set_editable(LaceDataGrid *g, gboolean editable) {
  g->editable = editable;
  update_action_states(g);
}

gboolean lace_data_grid_get_editable(LaceDataGrid *g) { return g->editable; }

void lace_data_grid_set_readonly_reason(LaceDataGrid *g, const char *reason) {
  g_free(g->readonly_reason);
  g->readonly_reason = g_strdup(reason);
}

const char *lace_data_grid_get_readonly_reason(LaceDataGrid *g) { return g->readonly_reason; }

void lace_data_grid_set_sortable(LaceDataGrid *g, gboolean sortable) {
  g->sortable = sortable;
}

void lace_data_grid_set_row_numbers(LaceDataGrid *g, gboolean show) {
  if (g->row_numbers == show)
    return;
  g->row_numbers = show;
  if (g->model)
    rebuild_columns(g);
}

SortSpec *lace_data_grid_get_sorts(LaceDataGrid *g, gsize *n) {
  *n = 0;
  GtkSorter *sorter = gtk_column_view_get_sorter(GTK_COLUMN_VIEW(g->view));
  if (!GTK_IS_COLUMN_VIEW_SORTER(sorter))
    return NULL;
  GtkColumnViewSorter *cs = GTK_COLUMN_VIEW_SORTER(sorter);
  guint count = gtk_column_view_sorter_get_n_sort_columns(cs);
  SortSpec *out = g_new0(SortSpec, count + 1);
  for (guint i = 0; i < count; i++) {
    GtkSortType order;
    GtkColumnViewColumn *column = gtk_column_view_sorter_get_nth_sort_column(cs, i, &order);
    if (!column)
      continue;
    gsize col = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(column), "col"));
    const LaceColumn *c = model_column(g, col);
    if (!c || !c->name)
      continue;
    out[*n].column = g_strdup(c->name);
    out[*n].desc = order == GTK_SORT_DESCENDING;
    (*n)++;
  }
  return out;
}

void lace_data_grid_set_sorts(LaceDataGrid *g, const SortSpec *sorts, gsize n) {
  if (!g->sortable)
    return;
  g->suppress_sort = TRUE;
  gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(g->view), NULL, GTK_SORT_ASCENDING);
  /* The last call becomes the primary sort: apply in reverse */
  for (gsize i = n; i > 0; i--) {
    const SortSpec *s = &sorts[i - 1];
    for (gsize c = 0; c < g->columns->len; c++) {
      const LaceColumn *col = model_column(g, c);
      if (col && col->name && s->column && strcmp(col->name, s->column) == 0) {
        gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(g->view), column_at(g, c),
                                       s->desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
        break;
      }
    }
  }
  g->suppress_sort = FALSE;
}

gsize *lace_data_grid_get_selected_rows(LaceDataGrid *g, gsize *n) {
  *n = 0;
  if (!g->selection)
    return NULL;
  GtkBitset *set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(g->selection));
  guint64 size = gtk_bitset_get_size(set);
  gsize *out = g_new(gsize, (gsize)size + 1);
  GtkBitsetIter it;
  guint value;
  if (gtk_bitset_iter_init_first(&it, set, &value)) {
    do {
      out[(*n)++] = value;
    } while (gtk_bitset_iter_next(&it, &value));
  }
  gtk_bitset_unref(set);
  return out;
}

gsize lace_data_grid_get_selected_count(LaceDataGrid *g) {
  if (!g->selection)
    return 0;
  GtkBitset *set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(g->selection));
  gsize n = (gsize)gtk_bitset_get_size(set);
  gtk_bitset_unref(set);
  return n;
}

void lace_data_grid_unselect_all(LaceDataGrid *g) {
  if (g->selection)
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(g->selection));
}

GtkColumnView *lace_data_grid_get_column_view(LaceDataGrid *g) {
  return GTK_COLUMN_VIEW(g->view);
}

/* ==========================================================================
 * Object
 * ========================================================================== */

static void lace_data_grid_dispose(GObject *obj) {
  LaceDataGrid *g = LACE_DATA_GRID(obj);
  if (g->model) {
    g_signal_handlers_disconnect_by_func(g->model, on_columns_changed, g);
    g_signal_handlers_disconnect_by_func(g->model, on_total_changed, g);
    gtk_column_view_set_model(GTK_COLUMN_VIEW(g->view), NULL);
  }
  g_clear_object(&g->selection);
  g_clear_object(&g->model);
  if (g->editor) {
    gtk_widget_unparent(GTK_WIDGET(g->editor));
    g->editor = NULL;
  }
  g_clear_pointer(&g->cell_menu, gtk_widget_unparent);
  g_clear_pointer(&g->sample, gtk_widget_unparent);
  g_clear_pointer(&g->scrolled, gtk_widget_unparent);
  if (g->focus_idle) {
    g_source_remove(g->focus_idle);
    g->focus_idle = 0;
  }
  g_clear_pointer(&g->bound, g_hash_table_unref);
  g_clear_object(&g->actions);
  g_clear_pointer(&g->columns, g_ptr_array_unref);
  g_free(g->is_pk);
  g->is_pk = NULL;
  g_clear_pointer(&g->readonly_reason, g_free);
  G_OBJECT_CLASS(lace_data_grid_parent_class)->dispose(obj);
}

/* One watcher on the window's focus instead of a controller per cell */
static void on_root_focus(GObject *root, GParamSpec *p, LaceDataGrid *g) {
  (void)p;
  GtkWidget *f = gtk_root_get_focus(GTK_ROOT(root));
  if (!f || g_object_get_data(G_OBJECT(f), "grid") != g)
    return;
  LaceRowItem *item = g_object_get_data(G_OBJECT(f), "item");
  gsize col = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(f), "col"));
  if (item && col != (gsize)-1)
    set_focus_cell(g, lace_row_item_get_position(item), col);
}

static void lace_data_grid_root(GtkWidget *w) {
  GTK_WIDGET_CLASS(lace_data_grid_parent_class)->root(w);
  GtkRoot *root = gtk_widget_get_root(w);
  if (root)
    g_signal_connect(root, "notify::focus-widget", G_CALLBACK(on_root_focus), w);
}

static void lace_data_grid_unroot(GtkWidget *w) {
  GtkRoot *root = gtk_widget_get_root(w);
  if (root)
    g_signal_handlers_disconnect_by_func(root, on_root_focus, w);
  GTK_WIDGET_CLASS(lace_data_grid_parent_class)->unroot(w);
}

static void lace_data_grid_class_init(LaceDataGridClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  oc->dispose = lace_data_grid_dispose;
  wc->root = lace_data_grid_root;
  wc->unroot = lace_data_grid_unroot;
  gtk_widget_class_set_layout_manager_type(wc, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(wc, "lace-grid");

  signals[SIG_SORT_CHANGED] = g_signal_new("sort-changed", LACE_TYPE_DATA_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_EDIT_CELL] = g_signal_new("edit-cell", LACE_TYPE_DATA_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 4, G_TYPE_UINT, G_TYPE_UINT,
      G_TYPE_STRING, G_TYPE_BOOLEAN);
  signals[SIG_DELETE_ROWS] = g_signal_new("delete-rows", LACE_TYPE_DATA_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_CELL_FOCUSED] = g_signal_new("cell-focused", LACE_TYPE_DATA_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);
  signals[SIG_ADD_ROW] = g_signal_new("add-row", LACE_TYPE_DATA_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_REFRESH] = g_signal_new("refresh", LACE_TYPE_DATA_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_FILTER_COLUMN] = g_signal_new("filter-column", LACE_TYPE_DATA_GRID,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}

static void lace_data_grid_init(LaceDataGrid *g) {
  g->columns = g_ptr_array_new();
  g->bound = g_hash_table_new(g_direct_hash, g_direct_equal);
  g->sortable = TRUE;
  g->row_numbers = TRUE;

  g->scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g->scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(g->scrolled, TRUE);
  gtk_widget_set_vexpand(g->scrolled, TRUE);
  gtk_widget_set_parent(g->scrolled, GTK_WIDGET(g));

  g->view = gtk_column_view_new(NULL);
  gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(g->view), TRUE);
  gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(g->view), FALSE);
  gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(g->view), TRUE);
  gtk_column_view_set_single_click_activate(GTK_COLUMN_VIEW(g->view), FALSE);
  gtk_column_view_set_tab_behavior(GTK_COLUMN_VIEW(g->view), GTK_LIST_TAB_ITEM);
  gtk_widget_add_css_class(g->view, "data-table");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(g->scrolled), g->view);
  g_signal_connect(g->view, "activate", G_CALLBACK(on_row_activated), g);

  GtkSorter *sorter = gtk_column_view_get_sorter(GTK_COLUMN_VIEW(g->view));
  if (sorter)
    g->sorter_handler = g_signal_connect(sorter, "changed", G_CALLBACK(on_sorter_changed), g);

  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), g);
  gtk_widget_add_controller(g->view, keys);
  GtkGesture *press = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), 0);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(press), GTK_PHASE_CAPTURE);
  g_signal_connect(press, "pressed", G_CALLBACK(on_view_pressed), g);
  gtk_widget_add_controller(g->view, GTK_EVENT_CONTROLLER(press));

  g->sample = gtk_label_new("0");
  gtk_widget_add_css_class(g->sample, "cell");
  gtk_widget_set_visible(g->sample, FALSE);
  gtk_widget_set_parent(g->sample, GTK_WIDGET(g));

  g->editor = lace_cell_editor_new();
  gtk_widget_set_parent(GTK_WIDGET(g->editor), GTK_WIDGET(g));
  g_signal_connect(g->editor, "committed", G_CALLBACK(on_editor_committed), g);
  g_signal_connect(g->editor, "closed", G_CALLBACK(on_editor_closed), g);

  GMenuModel *menu = cell_menu_model();
  g->cell_menu = gtk_popover_menu_new_from_model(menu);
  g_object_unref(menu);
  gtk_popover_set_has_arrow(GTK_POPOVER(g->cell_menu), FALSE);
  gtk_widget_set_halign(g->cell_menu, GTK_ALIGN_START);
  gtk_widget_set_parent(g->cell_menu, GTK_WIDGET(g));

  static const GActionEntry entries[] = {
      {"edit", act_edit, NULL, NULL, NULL, {0}},
      {"set-null", act_set_null, NULL, NULL, NULL, {0}},
      {"set-empty", act_set_empty, NULL, NULL, NULL, {0}},
      {"copy-cell", act_copy_cell, NULL, NULL, NULL, {0}},
      {"copy-row", act_copy_row, NULL, NULL, NULL, {0}},
      {"copy-column-name", act_copy_column_name, "t", NULL, NULL, {0}},
      {"copy-column-name-focused", act_copy_column_name, NULL, NULL, NULL, {0}},
      {"filter-value", act_filter_value, NULL, NULL, NULL, {0}},
      {"filter-column", act_filter_column, "t", NULL, NULL, {0}},
      {"sort-asc", act_sort_asc, "t", NULL, NULL, {0}},
      {"sort-desc", act_sort_desc, "t", NULL, NULL, {0}},
      {"sort-clear", act_sort_clear, NULL, NULL, NULL, {0}},
      {"fit-column", act_fit_column, "t", NULL, NULL, {0}},
      {"delete-rows", act_delete, NULL, NULL, NULL, {0}},
      {"add-row", act_add_row, NULL, NULL, NULL, {0}},
      {"refresh", act_refresh, NULL, NULL, NULL, {0}},
      {"select-all", act_select_all, NULL, NULL, NULL, {0}},
  };
  g->actions = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(g->actions), entries, G_N_ELEMENTS(entries), g);
  gtk_widget_insert_action_group(GTK_WIDGET(g), "grid", G_ACTION_GROUP(g->actions));
  update_action_states(g);
}

LaceDataGrid *lace_data_grid_new(void) {
  return g_object_new(LACE_TYPE_DATA_GRID, NULL);
}

void lace_data_grid_debug_counts(LaceDataGrid *g, guint *bound_cells, guint *row_widgets) {
  *bound_cells = g->bound ? g_hash_table_size(g->bound) : 0;
  *row_widgets = 0;
  /* columnview > listview > rows */
  GtkWidget *lv = gtk_widget_get_first_child(g->view);
  while (lv && !g_str_equal(gtk_widget_get_css_name(lv), "listview"))
    lv = gtk_widget_get_next_sibling(lv);
  for (GtkWidget *r = lv ? gtk_widget_get_first_child(lv) : NULL; r; r = gtk_widget_get_next_sibling(r))
    (*row_widgets)++;
}
