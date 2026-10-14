/*
 * Lace GTK frontend
 * LaceSidebar
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "sidebar.h"
#include "util.h"

struct _LaceSidebar {
  GtkWidget parent_instance;
  LaceConnection *conn;
  GtkWidget *box;
  GtkWidget *title;
  GtkWidget *subtitle;
  GtkWidget *tx_badge;
  GtkWidget *search;
  GtkWidget *list;
  GtkWidget *stack;
  GtkWidget *count_label;
  GtkStringFilter *filter;
  GtkSingleSelection *selection;
  GtkWidget *menu;
  char *menu_table;
};

enum { SIG_OPEN_TABLE, SIG_PREVIEW_TABLE, SIG_SHOW_STRUCTURE, SIG_NEW_QUERY, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(LaceSidebar, lace_sidebar, GTK_TYPE_WIDGET)

static const char *selected_table(LaceSidebar *s) {
  GtkStringObject *o = gtk_single_selection_get_selected_item(s->selection);
  return o ? gtk_string_object_get_string(o) : NULL;
}

static void on_activate(GtkListView *lv, guint pos, LaceSidebar *s) {
  (void)lv;
  GListModel *m = G_LIST_MODEL(s->selection);
  GtkStringObject *o = g_list_model_get_item(m, pos);
  if (o) {
    g_signal_emit(s, signals[SIG_OPEN_TABLE], 0, gtk_string_object_get_string(o));
    g_object_unref(o);
  }
}

static void item_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data) {
  (void)f;
  (void)data;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *icon = gtk_image_new_from_icon_name("lace-table-symbolic");
  gtk_widget_add_css_class(icon, "dim-label");
  GtkWidget *label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), label);
  gtk_list_item_set_child(li, box);
}

static void item_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data) {
  (void)f;
  (void)data;
  GtkWidget *box = gtk_list_item_get_child(li);
  GtkWidget *label = gtk_widget_get_last_child(box);
  GtkStringObject *o = gtk_list_item_get_item(li);
  gtk_label_set_text(GTK_LABEL(label), gtk_string_object_get_string(o));
  gtk_widget_set_tooltip_text(box, gtk_string_object_get_string(o));
}

static void update_count(LaceSidebar *s) {
  guint shown = g_list_model_get_n_items(G_LIST_MODEL(s->selection));
  guint all = g_list_model_get_n_items(lace_connection_get_tables(s->conn));
  char *t = shown == all ? g_strdup_printf("%u table%s", all, all == 1 ? "" : "s")
                         : g_strdup_printf("%u of %u tables", shown, all);
  gtk_label_set_text(GTK_LABEL(s->count_label), t);
  g_free(t);
  if (!lace_connection_tables_loaded(s->conn))
    gtk_stack_set_visible_child_name(GTK_STACK(s->stack), "loading");
  else if (all == 0)
    gtk_stack_set_visible_child_name(GTK_STACK(s->stack), "empty");
  else
    gtk_stack_set_visible_child_name(GTK_STACK(s->stack), "list");
}

static void on_tables_changed(LaceConnection *c, LaceSidebar *s) {
  (void)c;
  update_count(s);
}

static void on_items_changed(GListModel *m, guint p, guint r, guint a, LaceSidebar *s) {
  (void)m; (void)p; (void)r; (void)a;
  update_count(s);
}

static void on_search_changed(GtkSearchEntry *e, LaceSidebar *s) {
  gtk_string_filter_set_search(s->filter, gtk_editable_get_text(GTK_EDITABLE(e)));
}

static void on_search_activate(GtkSearchEntry *e, LaceSidebar *s) {
  /* Enter in the search opens the first match (the filter is debounced:
   * apply the text now) */
  gtk_string_filter_set_search(s->filter, gtk_editable_get_text(GTK_EDITABLE(e)));
  GListModel *m = G_LIST_MODEL(s->selection);
  if (g_list_model_get_n_items(m) > 0) {
    GtkStringObject *o = g_list_model_get_item(m, 0);
    g_signal_emit(s, signals[SIG_OPEN_TABLE], 0, gtk_string_object_get_string(o));
    g_object_unref(o);
  }
}

static void on_search_stop(GtkSearchEntry *e, LaceSidebar *s) {
  gtk_editable_set_text(GTK_EDITABLE(e), "");
  gtk_widget_grab_focus(s->list);
}

static void update_tx(LaceSidebar *s) {
  LaceTxState tx = lace_connection_get_tx_state(s->conn);
  const char *text = tx == LACE_TX_OPEN ? "TX" : tx == LACE_TX_FAILED ? "TX FAILED"
                   : tx == LACE_TX_LOST ? "TX LOST" : NULL;
  gtk_widget_set_visible(s->tx_badge, text != NULL);
  if (text)
    gtk_label_set_text(GTK_LABEL(s->tx_badge), text);
  gtk_widget_remove_css_class(s->tx_badge, "error");
  gtk_widget_remove_css_class(s->tx_badge, "warning");
  gtk_widget_add_css_class(s->tx_badge, tx == LACE_TX_OPEN ? "warning" : "error");
}

static void on_tx_changed(LaceConnection *c, LaceSidebar *s) {
  (void)c;
  update_tx(s);
}

static void on_new_query(GtkButton *b, LaceSidebar *s) {
  (void)b;
  g_signal_emit(s, signals[SIG_NEW_QUERY], 0);
}

static void on_refresh(GtkButton *b, LaceSidebar *s) {
  (void)b;
  lace_connection_refresh_tables(s->conn);
}

/* The table name of the list row under (x, y), or NULL */
static const char *table_at(LaceSidebar *s, double x, double y) {
  GtkWidget *w = gtk_widget_pick(s->list, x, y, GTK_PICK_DEFAULT);
  while (w && w != s->list && !g_str_equal(gtk_widget_get_css_name(w), "row"))
    w = gtk_widget_get_parent(w);
  if (!w || w == s->list)
    return NULL;
  /* The row widget's child is our box; its last child is the label */
  GtkWidget *box = gtk_widget_get_first_child(w);
  GtkWidget *label = box ? gtk_widget_get_last_child(box) : NULL;
  return label && GTK_IS_LABEL(label) ? gtk_label_get_text(GTK_LABEL(label)) : NULL;
}

/* A single click previews the table (a double click activates the row,
 * which keeps it) */
static void on_list_released(GtkGestureClick *g, int n, double x, double y, LaceSidebar *s) {
  (void)g;
  if (n != 1)
    return;
  const char *t = table_at(s, x, y);
  if (t) {
    char *copy = g_strdup(t);
    g_signal_emit(s, signals[SIG_PREVIEW_TABLE], 0, copy);
    g_free(copy);
  }
}

/* Context menu */
static void on_list_pressed(GtkGestureClick *g, int n, double x, double y, LaceSidebar *s) {
  (void)n;
  const char *t = table_at(s, x, y);
  if (!t)
    return;
  g_free(s->menu_table);
  s->menu_table = g_strdup(t);
  graphene_point_t p = {(float)x, (float)y}, out;
  if (!gtk_widget_compute_point(s->list, GTK_WIDGET(s), &p, &out))
    out = p;
  GdkRectangle rect = {(int)out.x, (int)out.y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(s->menu), &rect);
  gtk_popover_popup(GTK_POPOVER(s->menu));
  gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void act_open(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceSidebar *s = data;
  const char *t = s->menu_table ? s->menu_table : selected_table(s);
  if (t)
    g_signal_emit(s, signals[SIG_OPEN_TABLE], 0, t);
}

static void act_structure(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceSidebar *s = data;
  const char *t = s->menu_table ? s->menu_table : selected_table(s);
  if (t)
    g_signal_emit(s, signals[SIG_SHOW_STRUCTURE], 0, t);
}

static void act_copy(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceSidebar *s = data;
  const char *t = s->menu_table ? s->menu_table : selected_table(s);
  if (t)
    lace_copy_text(GTK_WIDGET(s), t);
}

static void act_select_query(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceSidebar *s = data;
  const char *t = s->menu_table ? s->menu_table : selected_table(s);
  if (!t)
    return;
  char *q = lace_quote_table(lace_connection_get_driver(s->conn), t);
  char *sql = g_strdup_printf("SELECT * FROM %s", q ? q : t);
  g_object_set_data_full(G_OBJECT(s), "query-text", sql, g_free);
  g_signal_emit(s, signals[SIG_NEW_QUERY], 0);
  free(q);
}

void lace_sidebar_focus_search(LaceSidebar *s) { gtk_widget_grab_focus(s->search); }
void lace_sidebar_focus_list(LaceSidebar *s) { gtk_widget_grab_focus(s->list); }

static void lace_sidebar_dispose(GObject *obj) {
  LaceSidebar *s = LACE_SIDEBAR(obj);
  if (s->conn) {
    g_signal_handlers_disconnect_by_func(s->conn, on_tables_changed, s);
    g_signal_handlers_disconnect_by_func(s->conn, on_tx_changed, s);
  }
  if (s->selection)
    g_signal_handlers_disconnect_by_func(s->selection, on_items_changed, s);
  g_clear_pointer(&s->menu, gtk_widget_unparent);
  g_clear_pointer(&s->box, gtk_widget_unparent);
  g_clear_object(&s->selection);
  g_clear_object(&s->conn);
  g_clear_pointer(&s->menu_table, g_free);
  G_OBJECT_CLASS(lace_sidebar_parent_class)->dispose(obj);
}

static void lace_sidebar_class_init(LaceSidebarClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = lace_sidebar_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "lace-sidebar");
  signals[SIG_OPEN_TABLE] = g_signal_new("open-table", LACE_TYPE_SIDEBAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_PREVIEW_TABLE] = g_signal_new("preview-table", LACE_TYPE_SIDEBAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_SHOW_STRUCTURE] = g_signal_new("show-structure", LACE_TYPE_SIDEBAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_NEW_QUERY] = g_signal_new("new-query", LACE_TYPE_SIDEBAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void lace_sidebar_init(LaceSidebar *s) { (void)s; }

LaceSidebar *lace_sidebar_new(LaceConnection *conn) {
  LaceSidebar *s = g_object_new(LACE_TYPE_SIDEBAR, NULL);
  s->conn = g_object_ref(conn);

  s->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(s->box, "sidebar-box");
  gtk_widget_set_parent(s->box, GTK_WIDGET(s));

  /* Header: connection name, where it points, transaction badge */
  GtkWidget *head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(head, "sidebar-head");
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *icon = gtk_image_new_from_icon_name(lace_connection_get_icon_name(conn));
  s->title = gtk_label_new(lace_connection_get_name(conn));
  gtk_widget_add_css_class(s->title, "heading");
  gtk_label_set_xalign(GTK_LABEL(s->title), 0);
  gtk_label_set_ellipsize(GTK_LABEL(s->title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(s->title, TRUE);
  s->tx_badge = gtk_label_new("TX");
  gtk_widget_add_css_class(s->tx_badge, "tx-badge");
  gtk_widget_set_visible(s->tx_badge, FALSE);
  gtk_box_append(GTK_BOX(row), icon);
  gtk_box_append(GTK_BOX(row), s->title);
  gtk_box_append(GTK_BOX(row), s->tx_badge);
  gtk_box_append(GTK_BOX(head), row);
  char *display = lace_connection_get_display(conn);
  s->subtitle = gtk_label_new(display);
  gtk_widget_set_tooltip_text(s->subtitle, display);
  g_free(display);
  gtk_widget_add_css_class(s->subtitle, "dim-label");
  gtk_widget_add_css_class(s->subtitle, "caption");
  gtk_label_set_xalign(GTK_LABEL(s->subtitle), 0);
  gtk_label_set_ellipsize(GTK_LABEL(s->subtitle), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append(GTK_BOX(head), s->subtitle);
  gtk_box_append(GTK_BOX(s->box), head);

  s->search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(s->search), "Filter tables");
  gtk_widget_add_css_class(s->search, "sidebar-search");
  g_signal_connect(s->search, "search-changed", G_CALLBACK(on_search_changed), s);
  g_signal_connect(s->search, "activate", G_CALLBACK(on_search_activate), s);
  g_signal_connect(s->search, "stop-search", G_CALLBACK(on_search_stop), s);
  gtk_box_append(GTK_BOX(s->box), s->search);

  /* List over the connection's tables, filtered by the search */
  GtkExpression *expr = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
  s->filter = gtk_string_filter_new(expr);
  gtk_string_filter_set_match_mode(s->filter, GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
  gtk_string_filter_set_ignore_case(s->filter, TRUE);
  GtkFilterListModel *filtered = gtk_filter_list_model_new(
      g_object_ref(lace_connection_get_tables(conn)), GTK_FILTER(s->filter));
  s->selection = gtk_single_selection_new(G_LIST_MODEL(filtered));
  gtk_single_selection_set_autoselect(s->selection, FALSE);
  gtk_single_selection_set_can_unselect(s->selection, TRUE);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(item_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(item_bind), NULL);
  s->list = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(s->selection)), factory);
  gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(s->list), FALSE);
  gtk_widget_add_css_class(s->list, "navigation-sidebar");
  g_signal_connect(s->list, "activate", G_CALLBACK(on_activate), s);
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_list_pressed), s);
  gtk_widget_add_controller(s->list, GTK_EVENT_CONTROLLER(click));
  GtkGesture *primary = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(primary), GDK_BUTTON_PRIMARY);
  g_signal_connect(primary, "released", G_CALLBACK(on_list_released), s);
  gtk_widget_add_controller(s->list, GTK_EVENT_CONTROLLER(primary));
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), s->list);
  gtk_widget_set_vexpand(scrolled, TRUE);

  s->stack = gtk_stack_new();
  gtk_widget_set_vexpand(s->stack, TRUE);
  gtk_stack_add_named(GTK_STACK(s->stack), scrolled, "list");
  GtkWidget *loading = adw_spinner_new();
  gtk_widget_set_size_request(loading, 32, 32);
  gtk_widget_set_halign(loading, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(loading, GTK_ALIGN_CENTER);
  gtk_stack_add_named(GTK_STACK(s->stack), loading, "loading");
  GtkWidget *empty = gtk_label_new("No tables");
  gtk_widget_add_css_class(empty, "dim-label");
  gtk_stack_add_named(GTK_STACK(s->stack), empty, "empty");
  gtk_box_append(GTK_BOX(s->box), s->stack);

  /* Footer: the same action bar as the content's, so both line up */
  GtkWidget *foot = gtk_action_bar_new();
  gtk_widget_add_css_class(foot, "table-footer");
  GtkWidget *nq = gtk_button_new();
  GtkWidget *content = adw_button_content_new();
  adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), "lace-query-symbolic");
  adw_button_content_set_label(ADW_BUTTON_CONTENT(content), "New query");
  gtk_button_set_child(GTK_BUTTON(nq), content);
  gtk_widget_set_tooltip_text(nq, "Open a SQL editor (Ctrl+E)");
  gtk_widget_add_css_class(nq, "flat");
  g_signal_connect(nq, "clicked", G_CALLBACK(on_new_query), s);
  gtk_action_bar_pack_start(GTK_ACTION_BAR(foot), nq);
  s->count_label = gtk_label_new("");
  gtk_widget_add_css_class(s->count_label, "dim-label");
  gtk_widget_add_css_class(s->count_label, "caption");
  GtkWidget *refresh = lace_icon_button("view-refresh-symbolic", "Reload the table list", NULL);
  g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), s);
  gtk_action_bar_pack_end(GTK_ACTION_BAR(foot), refresh);
  gtk_action_bar_pack_end(GTK_ACTION_BAR(foot), s->count_label);
  gtk_box_append(GTK_BOX(s->box), foot);

  /* Context menu */
  GMenu *menu = g_menu_new();
  g_menu_append(menu, "Open", "sidebar.open");
  g_menu_append(menu, "Query this table", "sidebar.select-query");
  g_menu_append(menu, "Structure…", "sidebar.structure");
  g_menu_append(menu, "Copy name", "sidebar.copy-name");
  s->menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  gtk_popover_set_has_arrow(GTK_POPOVER(s->menu), FALSE);
  gtk_widget_set_halign(s->menu, GTK_ALIGN_START);
  gtk_widget_set_parent(s->menu, GTK_WIDGET(s));
  static const GActionEntry entries[] = {
      {"open", act_open, NULL, NULL, NULL, {0}},
      {"select-query", act_select_query, NULL, NULL, NULL, {0}},
      {"structure", act_structure, NULL, NULL, NULL, {0}},
      {"copy-name", act_copy, NULL, NULL, NULL, {0}},
  };
  GSimpleActionGroup *group = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(group), entries, G_N_ELEMENTS(entries), s);
  gtk_widget_insert_action_group(GTK_WIDGET(s), "sidebar", G_ACTION_GROUP(group));
  g_object_unref(group);

  g_signal_connect(conn, "tables-changed", G_CALLBACK(on_tables_changed), s);
  g_signal_connect(conn, "tx-changed", G_CALLBACK(on_tx_changed), s);
  g_signal_connect(s->selection, "items-changed", G_CALLBACK(on_items_changed), s);
  update_count(s);
  update_tx(s);
  if (!lace_connection_tables_loaded(conn))
    lace_connection_refresh_tables(conn);
  return s;
}
