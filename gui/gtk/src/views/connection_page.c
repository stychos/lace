/*
 * Lace GTK frontend
 * LaceConnectionPage
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connection_page.h"
#include "sidebar.h"
#include "util.h"

struct _LaceConnectionPage {
  GtkWidget parent_instance;
  LaceConnection *conn;
  const LacePrefs *prefs;
  GtkWidget *split;
  LaceSidebar *sidebar;
  GtkWidget *stack;
  GtkWidget *tab_bar;
  GtkWidget *tab_view;
  guint query_counter;
  guint style_idle;
};

G_DEFINE_TYPE(LaceConnectionPage, lace_connection_page, GTK_TYPE_WIDGET)

static void schedule_style_sync(LaceConnectionPage *p);

/* The view a page shows: a preview page wraps its view in an AdwBin so the
 * content can be swapped without replacing the tab */
static GtkWidget *page_view(AdwTabPage *page) {
  GtkWidget *child = adw_tab_page_get_child(page);
  return ADW_IS_BIN(child) ? adw_bin_get_child(ADW_BIN(child)) : child;
}

static void update_empty(LaceConnectionPage *p) {
  int n = adw_tab_view_get_n_pages(ADW_TAB_VIEW(p->tab_view));
  gtk_stack_set_visible_child_name(GTK_STACK(p->stack), n > 0 ? "tabs" : "empty");
}

static void on_pages_changed(GObject *o, GParamSpec *ps, LaceConnectionPage *p) {
  (void)o; (void)ps;
  update_empty(p);
  schedule_style_sync(p);
}

static void on_page_reordered(AdwTabView *v, AdwTabPage *page, int pos, LaceConnectionPage *p) {
  (void)v; (void)page; (void)pos;
  schedule_style_sync(p);
}

static gboolean on_close_page(AdwTabView *view, AdwTabPage *page, LaceConnectionPage *p) {
  (void)p;
  adw_tab_view_close_page_finish(view, page, TRUE);
  return GDK_EVENT_STOP;
}

/* Tables and database views share a namespace; routines and triggers have their own */
static gboolean same_object(LaceObjectKind a, LaceObjectKind b) {
  gboolean ra = a == LACE_OBJ_TABLE || a == LACE_OBJ_VIEW, rb = b == LACE_OBJ_TABLE || b == LACE_OBJ_VIEW;
  return ra ? rb : a == b;
}

static AdwTabPage *find_object_page(LaceConnectionPage *p, LaceObjectKind kind, const char *name) {
  int n = adw_tab_view_get_n_pages(ADW_TAB_VIEW(p->tab_view));
  for (int i = 0; i < n; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(ADW_TAB_VIEW(p->tab_view), i);
    GtkWidget *child = page_view(page);
    if (LACE_IS_TABLE_VIEW(child) && same_object(lace_table_view_get_object_kind(LACE_TABLE_VIEW(child)), kind) &&
        g_strcmp0(lace_table_view_get_table(LACE_TABLE_VIEW(child)), name) == 0)
      return page;
  }
  return NULL;
}

static AdwTabPage *preview_page(LaceConnectionPage *p) {
  int n = adw_tab_view_get_n_pages(ADW_TAB_VIEW(p->tab_view));
  for (int i = 0; i < n; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(ADW_TAB_VIEW(p->tab_view), i);
    if (g_object_get_data(G_OBJECT(page), "preview"))
      return page;
  }
  return NULL;
}

/* AdwTabPage carries no styling, so the preview tab's own widget gets a
 * CSS class: the tab bar holds one "tabboxchild" per page, in page order,
 * inside the tab box for unpinned pages. Looked up again after any change. */
static GtkWidget *find_css(GtkWidget *w, const char *name, int depth) {
  for (; w && depth < 8; w = gtk_widget_get_next_sibling(w)) {
    if (g_str_equal(gtk_widget_get_css_name(w), name))
      return w;
    GtkWidget *found = find_css(gtk_widget_get_first_child(w), name, depth + 1);
    if (found)
      return found;
  }
  return NULL;
}

static gboolean sync_preview_styles(gpointer data) {
  LaceConnectionPage *p = data;
  p->style_idle = 0;
  /* The tab box with tabboxchild entries is the unpinned one */
  GtkWidget *box = NULL;
  for (GtkWidget *w = find_css(p->tab_bar, "tabbox", 0); w; w = find_css(gtk_widget_get_next_sibling(w), "tabbox", 0)) {
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
      if (g_str_equal(gtk_widget_get_css_name(c), "tabboxchild"))
        box = w;
    if (box)
      break;
  }
  if (!box) {
    /* The second scrolled window holds it; the first find hit the pinned box */
    GtkWidget *first = find_css(p->tab_bar, "tabbox", 0);
    GtkWidget *sw = first ? gtk_widget_get_parent(first) : NULL;
    for (GtkWidget *n = sw ? gtk_widget_get_next_sibling(sw) : NULL; n && !box; n = gtk_widget_get_next_sibling(n))
      box = find_css(gtk_widget_get_first_child(n), "tabbox", 0);
  }
  if (!box)
    return G_SOURCE_REMOVE;
  int i = 0;
  int n = adw_tab_view_get_n_pages(ADW_TAB_VIEW(p->tab_view));
  for (GtkWidget *c = gtk_widget_get_first_child(box); c; c = gtk_widget_get_next_sibling(c)) {
    if (!g_str_equal(gtk_widget_get_css_name(c), "tabboxchild"))
      continue;
    GtkWidget *tab = gtk_widget_get_first_child(c);
    if (!tab || i >= n)
      break;
    AdwTabPage *page = adw_tab_view_get_nth_page(ADW_TAB_VIEW(p->tab_view), i++);
    if (g_object_get_data(G_OBJECT(page), "preview"))
      gtk_widget_add_css_class(tab, "preview");
    else
      gtk_widget_remove_css_class(tab, "preview");
  }
  return G_SOURCE_REMOVE;
}

static void schedule_style_sync(LaceConnectionPage *p) {
  if (!p->style_idle)
    p->style_idle = g_timeout_add(50, sync_preview_styles, p);
}

static void set_preview(LaceConnectionPage *p, AdwTabPage *page, gboolean preview) {
  g_object_set_data(G_OBJECT(page), "preview", GINT_TO_POINTER(preview));
  schedule_style_sync(p);
  GIcon *icon = g_icon_new_for_string(preview ? "view-reveal-symbolic" : "lace-table-symbolic", NULL);
  adw_tab_page_set_icon(page, icon);
  g_object_unref(icon);
  const char *table = adw_tab_page_get_title(page);
  char *tip = preview ? g_strdup_printf("%s (preview: double-click the table or edit to keep it)", table)
                      : g_strdup(table);
  adw_tab_page_set_tooltip(page, tip);
  g_free(tip);
}

/* The view changed (filter, sort, edit): a preview becomes a kept tab */
static AdwTabPage *page_of_view(LaceConnectionPage *p, GtkWidget *view) {
  int n = adw_tab_view_get_n_pages(ADW_TAB_VIEW(p->tab_view));
  for (int i = 0; i < n; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(ADW_TAB_VIEW(p->tab_view), i);
    if (page_view(page) == view)
      return page;
  }
  return NULL;
}

static void on_view_touched(LaceTableView *view, LaceConnectionPage *p) {
  AdwTabPage *page = page_of_view(p, GTK_WIDGET(view));
  if (page && g_object_get_data(G_OBJECT(page), "preview"))
    set_preview(p, page, FALSE);
}

static void on_open_filtered(GObject *view, const char *table, gpointer filters, guint n, LaceConnectionPage *p) {
  (void)view;
  lace_connection_page_open_filtered(p, table, filters, n);
}

static void on_open_object(LaceTableView *view, int kind, const char *name, const char *table, int subview,
                           LaceConnectionPage *p) {
  (void)view;
  LaceTableView *tv = lace_connection_page_open_object(p, (LaceObjectKind)kind, name, table, NULL, FALSE);
  if (tv)
    lace_table_view_set_subview(tv, (LaceSubView)subview);
}

static const char *kind_icon(LaceObjectKind k) {
  switch (k) {
  case LACE_OBJ_VIEW: return "view-paged-symbolic";
  case LACE_OBJ_ROUTINE: return "system-run-symbolic";
  case LACE_OBJ_TRIGGER: return "lace-key-symbolic";
  default: return "lace-table-symbolic";
  }
}

static LaceTableView *open_object(LaceConnectionPage *p, LaceObjectKind kind, const char *table, const char *otable,
                                  const char *detail, gboolean preview) {
  AdwTabPage *existing = find_object_page(p, kind, table);
  if (existing) {
    if (!preview && g_object_get_data(G_OBJECT(existing), "preview"))
      set_preview(p, existing, FALSE);
    adw_tab_view_set_selected_page(ADW_TAB_VIEW(p->tab_view), existing);
    lace_table_view_focus_grid(LACE_TABLE_VIEW(page_view(existing)));
    return LACE_TABLE_VIEW(page_view(existing));
  }
  LaceTableView *view = lace_table_view_new_object(p->conn, kind, table, otable, detail, p->prefs);
  g_signal_connect(view, "touched", G_CALLBACK(on_view_touched), p);
  g_signal_connect(view, "open-filtered", G_CALLBACK(on_open_filtered), p);
  g_signal_connect(view, "open-object", G_CALLBACK(on_open_object), p);
  AdwTabPage *page;
  AdwTabPage *old = preview ? preview_page(p) : NULL;
  if (old) {
    /* The preview tab stays; only what it shows changes */
    page = old;
    adw_bin_set_child(ADW_BIN(adw_tab_page_get_child(page)), GTK_WIDGET(view));
  } else if (preview) {
    GtkWidget *bin = adw_bin_new();
    adw_bin_set_child(ADW_BIN(bin), GTK_WIDGET(view));
    page = adw_tab_view_append(ADW_TAB_VIEW(p->tab_view), bin);
  } else {
    page = adw_tab_view_append(ADW_TAB_VIEW(p->tab_view), GTK_WIDGET(view));
  }
  adw_tab_page_set_title(page, table);
  set_preview(p, page, preview);
  if (!preview && kind != LACE_OBJ_TABLE) {
    GIcon *icon = g_icon_new_for_string(kind_icon(kind), NULL);
    adw_tab_page_set_icon(page, icon);
    g_object_unref(icon);
  }
  adw_tab_view_set_selected_page(ADW_TAB_VIEW(p->tab_view), page);
  update_empty(p);
  if (!preview)
    lace_table_view_focus_grid(view);
  return view;
}

LaceTableView *lace_connection_page_open_table(LaceConnectionPage *p, const char *table) {
  return open_object(p, LACE_OBJ_TABLE, table, NULL, NULL, FALSE);
}

LaceTableView *lace_connection_page_preview_table(LaceConnectionPage *p, const char *table) {
  return open_object(p, LACE_OBJ_TABLE, table, NULL, NULL, TRUE);
}

LaceTableView *lace_connection_page_open_object(LaceConnectionPage *p, LaceObjectKind kind, const char *name,
                                                const char *table, const char *detail, gboolean preview) {
  return open_object(p, kind, name, table, detail, preview);
}

LaceTableView *lace_connection_page_open_filtered(LaceConnectionPage *p, const char *table,
                                                  const LaceFilter *filters, gsize n) {
  char *name = g_strdup(table); /* opening may free what table pointed into */
  LaceTableView *tv = open_object(p, LACE_OBJ_TABLE, name, NULL, NULL, FALSE);
  lace_table_view_show_filtered(tv, filters, n);
  GString *cond = g_string_new(NULL);
  for (gsize k = 0; k < n; k++)
    g_string_append_printf(cond, "%s%s = %s", k ? ", " : "", filters[k].column ? filters[k].column : "", filters[k].value ? filters[k].value : "");
  lace_toast(GTK_WIDGET(p), "%s where %s", name, cond->str);
  g_string_free(cond, TRUE);
  g_free(name);
  return tv;
}

LaceSidebar *lace_connection_page_get_sidebar(LaceConnectionPage *p) { return p->sidebar; }

static void on_query_title_changed(LaceQueryView *qv, LaceConnectionPage *p) {
  AdwTabPage *page = adw_tab_view_get_page(ADW_TAB_VIEW(p->tab_view), GTK_WIDGET(qv));
  if (!page)
    return;
  char *t = lace_query_view_get_title(qv);
  adw_tab_page_set_title(page, t);
  g_free(t);
}

LaceQueryView *lace_connection_page_new_query(LaceConnectionPage *p, const char *text) {
  LaceQueryView *view = lace_query_view_new(p->conn, p->prefs);
  g_signal_connect(view, "open-filtered", G_CALLBACK(on_open_filtered), p);
  AdwTabPage *page = adw_tab_view_append(ADW_TAB_VIEW(p->tab_view), GTK_WIDGET(view));
  p->query_counter++;
  char *title = g_strdup_printf("Query %u", p->query_counter);
  adw_tab_page_set_title(page, title);
  g_free(title);
  adw_tab_page_set_icon(page, g_icon_new_for_string("lace-query-symbolic", NULL));
  adw_tab_view_set_selected_page(ADW_TAB_VIEW(p->tab_view), page);
  if (text && *text)
    lace_query_view_set_text(view, text);
  g_signal_connect(view, "title-changed", G_CALLBACK(on_query_title_changed), p);
  on_query_title_changed(view, p);
  update_empty(p);
  lace_query_view_focus_editor(view);
  return view;
}

GtkWidget *lace_connection_page_current_view(LaceConnectionPage *p) {
  AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(p->tab_view));
  return page ? page_view(page) : NULL;
}

GtkWidget *lace_connection_page_view_of(AdwTabPage *page) { return page_view(page); }

gboolean lace_connection_page_close_current(LaceConnectionPage *p) {
  AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(p->tab_view));
  if (!page)
    return FALSE;
  adw_tab_view_close_page(ADW_TAB_VIEW(p->tab_view), page);
  return TRUE;
}

void lace_connection_page_select_tab(LaceConnectionPage *p, int offset) {
  if (offset > 0)
    adw_tab_view_select_next_page(ADW_TAB_VIEW(p->tab_view));
  else
    adw_tab_view_select_previous_page(ADW_TAB_VIEW(p->tab_view));
}

AdwTabView *lace_connection_page_get_tab_view(LaceConnectionPage *p) {
  return ADW_TAB_VIEW(p->tab_view);
}

gboolean lace_connection_page_sidebar_visible(LaceConnectionPage *p) {
  return adw_overlay_split_view_get_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(p->split));
}

void lace_connection_page_set_sidebar_visible(LaceConnectionPage *p, gboolean visible) {
  adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(p->split), visible);
}

void lace_connection_page_focus_sidebar(LaceConnectionPage *p) {
  lace_connection_page_set_sidebar_visible(p, TRUE);
  lace_sidebar_focus_search(p->sidebar);
}

void lace_connection_page_focus_content(LaceConnectionPage *p) {
  GtkWidget *v = lace_connection_page_current_view(p);
  if (LACE_IS_TABLE_VIEW(v))
    lace_table_view_focus_grid(LACE_TABLE_VIEW(v));
  else if (LACE_IS_QUERY_VIEW(v))
    lace_query_view_focus_editor(LACE_QUERY_VIEW(v));
}

LaceConnection *lace_connection_page_get_connection(LaceConnectionPage *p) { return p->conn; }

/* Sidebar signals (the entry is the sidebar's: copy what outlives the emission) */
static void on_open_entry(LaceSidebar *s, gpointer entry, LaceConnectionPage *p) {
  (void)s;
  const LaceSidebarEntry *e = entry;
  char *name = g_strdup(e->name), *table = e->table ? g_strdup(e->table) : NULL;
  char *detail = e->detail ? g_strdup(e->detail) : NULL;
  open_object(p, e->kind, name, table, detail, FALSE);
  g_free(name);
  g_free(table);
  g_free(detail);
}

static void on_preview_entry(LaceSidebar *s, gpointer entry, LaceConnectionPage *p) {
  (void)s;
  const LaceSidebarEntry *e = entry;
  char *name = g_strdup(e->name), *table = e->table ? g_strdup(e->table) : NULL;
  char *detail = e->detail ? g_strdup(e->detail) : NULL;
  open_object(p, e->kind, name, table, detail, TRUE);
  g_free(name);
  g_free(table);
  g_free(detail);
}

static void on_show_structure(LaceSidebar *s, gpointer entry, LaceConnectionPage *p) {
  (void)s;
  const LaceSidebarEntry *e = entry;
  char *name = g_strdup(e->name), *table = e->table ? g_strdup(e->table) : NULL;
  char *detail = e->detail ? g_strdup(e->detail) : NULL;
  LaceTableView *tv = open_object(p, e->kind, name, table, detail, FALSE);
  if (tv)
    lace_table_view_set_subview(tv, LACE_SUBVIEW_STRUCTURE);
  g_free(name);
  g_free(table);
  g_free(detail);
}

static void on_new_query(LaceSidebar *s, LaceConnectionPage *p) {
  const char *text = g_object_get_data(G_OBJECT(s), "query-text");
  lace_connection_page_new_query(p, text);
  g_object_set_data(G_OBJECT(s), "query-text", NULL);
}

/* Tab context menu */
static void act_close_others(GSimpleAction *a, GVariant *v, gpointer data) {
  (void)a; (void)v;
  LaceConnectionPage *p = data;
  AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(p->tab_view));
  if (page)
    adw_tab_view_close_other_pages(ADW_TAB_VIEW(p->tab_view), page);
}

static void act_close_all(GSimpleAction *a, GVariant *v, gpointer data) {
  (void)a; (void)v;
  LaceConnectionPage *p = data;
  while (adw_tab_view_get_n_pages(ADW_TAB_VIEW(p->tab_view)) > 0)
    adw_tab_view_close_page(ADW_TAB_VIEW(p->tab_view), adw_tab_view_get_nth_page(ADW_TAB_VIEW(p->tab_view), 0));
}

static void act_close(GSimpleAction *a, GVariant *v, gpointer data) {
  (void)a; (void)v;
  lace_connection_page_close_current(LACE_CONNECTION_PAGE(data));
}

static void lace_connection_page_dispose(GObject *obj) {
  LaceConnectionPage *p = LACE_CONNECTION_PAGE(obj);
  if (p->style_idle) {
    g_source_remove(p->style_idle);
    p->style_idle = 0;
  }
  g_clear_pointer(&p->split, gtk_widget_unparent);
  p->sidebar = NULL;
  g_clear_object(&p->conn);
  G_OBJECT_CLASS(lace_connection_page_parent_class)->dispose(obj);
}

static void lace_connection_page_class_init(LaceConnectionPageClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = lace_connection_page_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
}

static void lace_connection_page_init(LaceConnectionPage *p) { (void)p; }

LaceConnectionPage *lace_connection_page_new(LaceConnection *conn, const LacePrefs *prefs) {
  LaceConnectionPage *p = g_object_new(LACE_TYPE_CONNECTION_PAGE, NULL);
  p->conn = g_object_ref(conn);
  p->prefs = prefs;

  p->split = adw_overlay_split_view_new();
  adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(p->split), 200);
  adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(p->split), 360);
  adw_overlay_split_view_set_sidebar_width_fraction(ADW_OVERLAY_SPLIT_VIEW(p->split), 0.22);
  gtk_widget_set_parent(p->split, GTK_WIDGET(p));

  p->sidebar = lace_sidebar_new(conn);
  adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(p->split), GTK_WIDGET(p->sidebar));
  g_signal_connect(p->sidebar, "open-object", G_CALLBACK(on_open_entry), p);
  g_signal_connect(p->sidebar, "preview-object", G_CALLBACK(on_preview_entry), p);
  g_signal_connect(p->sidebar, "show-structure", G_CALLBACK(on_show_structure), p);
  g_signal_connect(p->sidebar, "new-query", G_CALLBACK(on_new_query), p);

  GtkWidget *tv = adw_toolbar_view_new();
  p->tab_view = GTK_WIDGET(adw_tab_view_new());
  p->tab_bar = GTK_WIDGET(adw_tab_bar_new());
  adw_tab_bar_set_view(ADW_TAB_BAR(p->tab_bar), ADW_TAB_VIEW(p->tab_view));
  adw_tab_bar_set_autohide(ADW_TAB_BAR(p->tab_bar), FALSE);
  adw_tab_bar_set_expand_tabs(ADW_TAB_BAR(p->tab_bar), FALSE);
  gtk_widget_add_css_class(p->tab_bar, "inline");
  gtk_widget_add_css_class(p->tab_bar, "inner-tabs");
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), p->tab_bar);
  adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(tv), ADW_TOOLBAR_FLAT);

  p->stack = gtk_stack_new();
  GtkWidget *empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "lace-table-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(empty), "Pick a table");
  adw_status_page_set_description(ADW_STATUS_PAGE(empty),
      "Choose a table in the sidebar, or press Ctrl+E for a SQL editor.");
  gtk_stack_add_named(GTK_STACK(p->stack), empty, "empty");
  gtk_stack_add_named(GTK_STACK(p->stack), p->tab_view, "tabs");
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), p->stack);
  adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(p->split), tv);

  GMenu *menu = g_menu_new();
  g_menu_append(menu, "Close tab", "tabs.close");
  g_menu_append(menu, "Close other tabs", "tabs.close-others");
  g_menu_append(menu, "Close all tabs", "tabs.close-all");
  adw_tab_view_set_menu_model(ADW_TAB_VIEW(p->tab_view), G_MENU_MODEL(menu));
  g_object_unref(menu);
  static const GActionEntry entries[] = {
      {"close", act_close, NULL, NULL, NULL, {0}},
      {"close-others", act_close_others, NULL, NULL, NULL, {0}},
      {"close-all", act_close_all, NULL, NULL, NULL, {0}},
  };
  GSimpleActionGroup *group = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(group), entries, G_N_ELEMENTS(entries), p);
  gtk_widget_insert_action_group(GTK_WIDGET(p), "tabs", G_ACTION_GROUP(group));
  g_object_unref(group);

  g_signal_connect(p->tab_view, "close-page", G_CALLBACK(on_close_page), p);
  g_signal_connect(p->tab_view, "notify::n-pages", G_CALLBACK(on_pages_changed), p);
  g_signal_connect(p->tab_view, "page-reordered", G_CALLBACK(on_page_reordered), p);
  update_empty(p);
  return p;
}
