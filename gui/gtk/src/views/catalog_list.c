/*
 * Lace GTK frontend
 * LaceCatalogList
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "catalog_list.h"
#include <string.h>

/* One row as a list item: its cells, whether it spans, is a heading, its tag */
#define LACE_TYPE_CATALOG_ROW (lace_catalog_row_get_type())
G_DECLARE_FINAL_TYPE(LaceCatalogRow, lace_catalog_row, LACE, CATALOG_ROW, GObject)

struct _LaceCatalogRow {
  GObject parent_instance;
  char **cells;
  gsize ncells;
  gboolean span, heading;
  int tag;
};

G_DEFINE_TYPE(LaceCatalogRow, lace_catalog_row, G_TYPE_OBJECT)

static void lace_catalog_row_finalize(GObject *obj) {
  LaceCatalogRow *r = LACE_CATALOG_ROW(obj);
  g_strfreev(r->cells);
  G_OBJECT_CLASS(lace_catalog_row_parent_class)->finalize(obj);
}

static void lace_catalog_row_class_init(LaceCatalogRowClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = lace_catalog_row_finalize;
}

static void lace_catalog_row_init(LaceCatalogRow *r) { (void)r; }

struct _LaceCatalogList {
  GtkWidget parent_instance;
  GtkWidget *stack;
  GtkWidget *view;
  GtkWidget *scrolled;
  GtkWidget *message;
  GListStore *store;
  GtkSingleSelection *selection;
  gboolean monospace;
  gboolean has_head;
};

enum { SIG_ACTIVATE_ROW, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(LaceCatalogList, lace_catalog_list, GTK_TYPE_WIDGET)

static void cell_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data) {
  (void)f;
  LaceCatalogList *l = data;
  GtkWidget *label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_start(label, 6);
  gtk_widget_set_margin_end(label, 6);
  if (l->monospace)
    gtk_widget_add_css_class(label, "monospace");
  gtk_list_item_set_child(li, label);
}

static void cell_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data) {
  (void)data;
  gsize col = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(f), "col"));
  GtkWidget *label = gtk_list_item_get_child(li);
  LaceCatalogRow *r = gtk_list_item_get_item(li);
  const char *text = "";
  if (r->span) {
    text = col == 0 && r->ncells ? r->cells[0] : "";
  } else if (col < r->ncells) {
    text = r->cells[col];
  }
  gtk_label_set_text(GTK_LABEL(label), text);
  if (r->heading) {
    gtk_widget_add_css_class(label, "heading");
    gtk_widget_remove_css_class(label, "dim-label");
    if (r->span)
      gtk_widget_add_css_class(label, "accent");
  } else {
    gtk_widget_remove_css_class(label, "heading");
    gtk_widget_remove_css_class(label, "accent");
  }
  gtk_list_item_set_selectable(li, !r->heading);
  gtk_list_item_set_activatable(li, !r->heading);
}

static void on_activate(GtkColumnView *cv, guint pos, LaceCatalogList *l) {
  (void)cv;
  LaceCatalogRow *r = g_list_model_get_item(G_LIST_MODEL(l->store), pos);
  if (r) {
    if (!r->heading)
      g_signal_emit(l, signals[SIG_ACTIVATE_ROW], 0, r->tag);
    g_object_unref(r);
  }
}

void lace_catalog_list_show(LaceCatalogList *l, const CatalogRows *rows, const char *message) {
  /* columns anew: the sub-views differ in their count */
  GListModel *cols = gtk_column_view_get_columns(GTK_COLUMN_VIEW(l->view));
  while (g_list_model_get_n_items(cols) > 0) {
    GtkColumnViewColumn *c = g_list_model_get_item(cols, 0);
    gtk_column_view_remove_column(GTK_COLUMN_VIEW(l->view), c);
    g_object_unref(c);
  }
  g_list_store_remove_all(l->store);
  if (!rows) {
    gtk_label_set_text(GTK_LABEL(l->message), message ? message : "");
    gtk_stack_set_visible_child_name(GTK_STACK(l->stack), "message");
    return;
  }
  gsize ncols = rows->ncols ? rows->ncols : 1;
  l->has_head = rows->head != NULL;
  for (gsize i = 0; i < ncols; i++) {
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_object_set_data(G_OBJECT(factory), "col", GSIZE_TO_POINTER(i));
    g_signal_connect(factory, "setup", G_CALLBACK(cell_setup), l);
    g_signal_connect(factory, "bind", G_CALLBACK(cell_bind), l);
    GtkColumnViewColumn *c = gtk_column_view_column_new(rows->head ? rows->head[i] : "", factory);
    gtk_column_view_column_set_resizable(c, TRUE);
    gtk_column_view_column_set_expand(c, i + 1 == ncols);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(l->view), c);
    g_object_unref(c);
  }
  gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(l->view), rows->head != NULL);
  GtkWidget *header = gtk_widget_get_first_child(l->view);
  if (header)
    gtk_widget_set_visible(header, rows->head != NULL);
  for (gsize r = 0; r < rows->nrows; r++) {
    LaceCatalogRow *row = g_object_new(LACE_TYPE_CATALOG_ROW, NULL);
    row->cells = g_new0(char *, ncols + 1);
    for (gsize i = 0; i < ncols; i++)
      row->cells[i] = g_strdup(i < rows->ncols ? rows->cells[r][i] : "");
    row->ncells = ncols;
    row->span = rows->span[r];
    row->heading = rows->heading[r];
    row->tag = rows->tag[r];
    g_list_store_append(l->store, row);
    g_object_unref(row);
  }
  gtk_stack_set_visible_child_name(GTK_STACK(l->stack), "list");
}

char **lace_catalog_list_get_texts(LaceCatalogList *l) {
  guint n = g_list_model_get_n_items(G_LIST_MODEL(l->store));
  char **out = g_new0(char *, n + 1);
  for (guint i = 0; i < n; i++) {
    LaceCatalogRow *r = g_list_model_get_item(G_LIST_MODEL(l->store), i);
    out[i] = r->span ? g_strdup(r->ncells ? r->cells[0] : "") : g_strjoinv("  ", r->cells);
    g_object_unref(r);
  }
  return out;
}

void lace_catalog_list_focus(LaceCatalogList *l) { gtk_widget_grab_focus(l->view); }

static void lace_catalog_list_dispose(GObject *obj) {
  LaceCatalogList *l = LACE_CATALOG_LIST(obj);
  g_clear_pointer(&l->stack, gtk_widget_unparent);
  g_clear_object(&l->store);
  g_clear_object(&l->selection);
  G_OBJECT_CLASS(lace_catalog_list_parent_class)->dispose(obj);
}

static void lace_catalog_list_class_init(LaceCatalogListClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = lace_catalog_list_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "lace-cataloglist");
  signals[SIG_ACTIVATE_ROW] = g_signal_new("activate-row", LACE_TYPE_CATALOG_LIST,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void lace_catalog_list_init(LaceCatalogList *l) { (void)l; }

LaceCatalogList *lace_catalog_list_new(gboolean monospace) {
  LaceCatalogList *l = g_object_new(LACE_TYPE_CATALOG_LIST, NULL);
  l->monospace = monospace;
  l->store = g_list_store_new(LACE_TYPE_CATALOG_ROW);
  l->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(l->store)));
  gtk_single_selection_set_autoselect(l->selection, FALSE);
  gtk_single_selection_set_can_unselect(l->selection, TRUE);
  l->view = gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(l->selection)));
  gtk_column_view_set_single_click_activate(GTK_COLUMN_VIEW(l->view), FALSE);
  gtk_widget_add_css_class(l->view, "data-table");
  g_signal_connect(l->view, "activate", G_CALLBACK(on_activate), l);
  l->scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(l->scrolled), l->view);
  gtk_widget_set_vexpand(l->scrolled, TRUE);
  l->message = gtk_label_new("");
  gtk_widget_add_css_class(l->message, "dim-label");
  gtk_label_set_wrap(GTK_LABEL(l->message), TRUE);
  gtk_widget_set_halign(l->message, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(l->message, GTK_ALIGN_CENTER);
  l->stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(l->stack), l->scrolled, "list");
  gtk_stack_add_named(GTK_STACK(l->stack), l->message, "message");
  gtk_widget_set_parent(l->stack, GTK_WIDGET(l));
  return l;
}
