/*
 * Lace GTK frontend
 * LaceWindow: one workspace
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "window.h"
#include "session.h"
#include "util.h"
#include "views/connect_dialog.h"
#include "views/history_dialog.h"
#include <config/saved_connection.h>
#include <util/connstr.h>
#include <util/str.h>
#include <stdlib.h>
#include <string.h>

struct _LaceWindow {
  AdwApplicationWindow parent_instance;
  LaceApp *app;
  char *name;
  GtkWidget *toast_overlay;
  GtkWidget *title_widget;
  GtkWidget *tab_bar;
  GtkWidget *tab_view;
  GtkWidget *stack;
  GtkWidget *welcome_list;
  GtkWidget *welcome_group;
  GtkWidget *sidebar_toggle;
  guint window_counter;
};

G_DEFINE_TYPE(LaceWindow, lace_window, ADW_TYPE_APPLICATION_WINDOW)

static guint g_window_counter;

static void update_title(LaceWindow *w) {
  adw_window_title_set_title(ADW_WINDOW_TITLE(w->title_widget), w->name);
  AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(w->tab_view));
  if (page) {
    LaceConnectionPage *cp = LACE_CONNECTION_PAGE(adw_tab_page_get_child(page));
    char *d = lace_connection_get_display(lace_connection_page_get_connection(cp));
    adw_window_title_set_subtitle(ADW_WINDOW_TITLE(w->title_widget), d);
    g_free(d);
  } else {
    adw_window_title_set_subtitle(ADW_WINDOW_TITLE(w->title_widget), "");
  }
  char *t = g_strdup_printf("%s · Lace", w->name);
  gtk_window_set_title(GTK_WINDOW(w), t);
  g_free(t);
}

const char *lace_window_get_name(LaceWindow *w) { return w->name; }

void lace_window_set_name(LaceWindow *w, const char *name) {
  g_free(w->name);
  w->name = g_strdup(name && *name ? name : "Workspace");
  update_title(w);
}

LaceConnectionPage *lace_window_current_page(LaceWindow *w) {
  AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(w->tab_view));
  return page ? LACE_CONNECTION_PAGE(adw_tab_page_get_child(page)) : NULL;
}

AdwTabView *lace_window_get_tab_view(LaceWindow *w) { return ADW_TAB_VIEW(w->tab_view); }

/* ==========================================================================
 * Welcome page
 * ========================================================================== */

static void on_welcome_row_activated(GtkListBox *lb, GtkListBoxRow *row, LaceWindow *w) {
  (void)lb;
  const char *id = g_object_get_data(G_OBJECT(row), "id");
  ConnectionItem *item = id ? connmgr_find_by_id(lace_app_get_saved(w->app), id) : NULL;
  if (!item || item->type != CONN_ITEM_CONNECTION)
    return;
  char *connstr = lace_saved_conn_to_connstr(&item->connection);
  lace_window_connect_to(w, connstr, item->connection.id, item->connection.name);
  str_secure_free(connstr);
}

static char *saved_subtitle(const LaceSavedConnection *c) {
  if (g_strcmp0(c->driver, "sqlite") == 0)
    return g_strdup_printf("SQLite · %s", c->database ? c->database : "");
  return g_strdup_printf("%s · %s%s%s/%s%s%s", lace_driver_normalize(c->driver),
                         c->user && *c->user ? c->user : "", c->user && *c->user ? "@" : "",
                         c->host ? c->host : "", c->database ? c->database : "",
                         c->ssh ? " via " : "", c->ssh && c->ssh_host ? c->ssh_host : "");
}

static void collect_saved(ConnectionItem *folder, GtkListBox *list, int *count) {
  for (size_t i = 0; folder && i < folder->folder.num_children; i++) {
    ConnectionItem *it = &folder->folder.children[i];
    if (it->type == CONN_ITEM_FOLDER) {
      collect_saved(it, list, count);
      continue;
    }
    if (*count >= 12)
      return;
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), it->connection.name ? it->connection.name : "Connection");
    char *sub = saved_subtitle(&it->connection);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
    g_free(sub);
    const char *icon = g_strcmp0(it->connection.driver, "sqlite") == 0 ? "lace-sqlite-symbolic"
                     : g_strcmp0(it->connection.driver, "postgres") == 0 ? "lace-postgres-symbolic"
                                                                          : "lace-mysql-symbolic";
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(icon));
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), NULL);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data_full(G_OBJECT(row), "id", g_strdup(it->connection.id), g_free);
    gtk_list_box_append(list, row);
    (*count)++;
  }
}

static void rebuild_welcome(LaceWindow *w) {
  gtk_list_box_remove_all(GTK_LIST_BOX(w->welcome_list));
  int count = 0;
  collect_saved(&lace_app_get_saved(w->app)->root, GTK_LIST_BOX(w->welcome_list), &count);
  gtk_widget_set_visible(w->welcome_group, count > 0);
}

static void on_open_file_done(GObject *src, GAsyncResult *res, gpointer data) {
  LaceWindow *w = data;
  GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
  if (!file)
    return;
  char *path = g_file_get_path(file);
  g_object_unref(file);
  if (!path)
    return;
  char *enc = connstr_encode_path(path);
  char *connstr = g_strdup_printf("sqlite://%s", enc ? enc : path);
  free(enc);
  lace_window_connect_to(w, connstr, NULL, NULL);
  g_free(connstr);
  g_free(path);
}

static void act_open_file(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceWindow *w = data;
  GtkFileDialog *d = gtk_file_dialog_new();
  gtk_file_dialog_set_title(d, "Open a SQLite database");
  GtkFileFilter *f = gtk_file_filter_new();
  gtk_file_filter_set_name(f, "SQLite databases");
  gtk_file_filter_add_suffix(f, "db");
  gtk_file_filter_add_suffix(f, "sqlite");
  gtk_file_filter_add_suffix(f, "sqlite3");
  gtk_file_filter_add_suffix(f, "db3");
  GtkFileFilter *any = gtk_file_filter_new();
  gtk_file_filter_set_name(any, "All files");
  gtk_file_filter_add_pattern(any, "*");
  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, f);
  g_list_store_append(filters, any);
  gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
  g_object_unref(filters);
  g_object_unref(f);
  g_object_unref(any);
  gtk_file_dialog_open(d, GTK_WINDOW(w), NULL, on_open_file_done, w);
  g_object_unref(d);
}

static GtkWidget *build_welcome(LaceWindow *w) {
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 560);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(box, 36);
  gtk_widget_set_margin_bottom(box, 36);

  GtkWidget *icon = gtk_image_new_from_icon_name("dev.hexed.lace");
  gtk_widget_add_css_class(icon, "welcome-icon");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 96);
  gtk_box_append(GTK_BOX(box), icon);
  GtkWidget *title = gtk_label_new("Lace");
  gtk_widget_add_css_class(title, "welcome-title");
  gtk_box_append(GTK_BOX(box), title);
  GtkWidget *desc = gtk_label_new("Each window is a workspace. Each tab is a database connection.");
  gtk_widget_add_css_class(desc, "dim-label");
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_label_set_justify(GTK_LABEL(desc), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(box), desc);

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(buttons, 12);
  GtkWidget *connect = gtk_button_new_with_label("Connect…");
  gtk_widget_add_css_class(connect, "suggested-action");
  gtk_widget_add_css_class(connect, "pill");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(connect), "win.connect");
  GtkWidget *open = gtk_button_new_with_label("Open SQLite file…");
  gtk_widget_add_css_class(open, "pill");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "win.open-file");
  gtk_box_append(GTK_BOX(buttons), connect);
  gtk_box_append(GTK_BOX(buttons), open);
  gtk_box_append(GTK_BOX(box), buttons);

  GtkWidget *group = adw_preferences_group_new();
  w->welcome_group = group;
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Saved connections");
  gtk_widget_set_margin_top(group, 24);
  w->welcome_list = gtk_list_box_new();
  gtk_widget_add_css_class(w->welcome_list, "boxed-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(w->welcome_list), GTK_SELECTION_NONE);
  g_signal_connect(w->welcome_list, "row-activated", G_CALLBACK(on_welcome_row_activated), w);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), w->welcome_list);
  gtk_box_append(GTK_BOX(box), group);
  GtkWidget *hint = gtk_label_new("Ctrl+T connects · Ctrl+N opens a new workspace · Ctrl+? lists every shortcut");
  gtk_widget_add_css_class(hint, "dim-label");
  gtk_widget_add_css_class(hint, "caption");
  gtk_widget_set_margin_top(hint, 12);
  gtk_box_append(GTK_BOX(box), hint);

  adw_clamp_set_child(ADW_CLAMP(clamp), box);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), clamp);
  return scrolled;
}

/* ==========================================================================
 * Connection tabs
 * ========================================================================== */

static void update_stack(LaceWindow *w) {
  int n = adw_tab_view_get_n_pages(ADW_TAB_VIEW(w->tab_view));
  if (n == 0) {
    rebuild_welcome(w);
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "welcome");
  } else {
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "tabs");
  }
  gtk_widget_set_visible(w->tab_bar, n > 0);
  gtk_widget_set_visible(w->sidebar_toggle, n > 0);
  update_title(w);
}

static AdwTabPage *page_of_connection(LaceWindow *w, LaceConnection *conn) {
  int n = adw_tab_view_get_n_pages(ADW_TAB_VIEW(w->tab_view));
  for (int i = 0; i < n; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(ADW_TAB_VIEW(w->tab_view), i);
    LaceConnectionPage *cp = LACE_CONNECTION_PAGE(adw_tab_page_get_child(page));
    if (lace_connection_page_get_connection(cp) == conn)
      return page;
  }
  return NULL;
}

static void refresh_page_state(LaceWindow *w, LaceConnection *conn) {
  AdwTabPage *page = page_of_connection(w, conn);
  if (!page)
    return;
  adw_tab_page_set_loading(page, lace_connection_is_busy(conn));
  LaceTxState tx = lace_connection_get_tx_state(conn);
  if (tx == LACE_TX_NONE) {
    adw_tab_page_set_indicator_icon(page, NULL);
  } else {
    GIcon *icon = g_icon_new_for_string(tx == LACE_TX_OPEN ? "emblem-synchronizing-symbolic"
                                                            : "dialog-warning-symbolic", NULL);
    adw_tab_page_set_indicator_icon(page, icon);
    g_object_unref(icon);
    adw_tab_page_set_indicator_tooltip(page, tx == LACE_TX_OPEN ? "Transaction open"
                                             : tx == LACE_TX_FAILED ? "Transaction failed: only ROLLBACK works"
                                                                    : "Transaction lost: run ROLLBACK to acknowledge");
  }
  adw_tab_page_set_title(page, lace_connection_get_name(conn));
}

static void on_conn_state(LaceConnection *conn, LaceWindow *w) {
  refresh_page_state(w, conn);
}

static void on_conn_notice(LaceConnection *conn, const char *msg, LaceWindow *w) {
  (void)conn;
  lace_toast(GTK_WIDGET(w->toast_overlay), "%s", msg);
}

static gboolean on_close_page(AdwTabView *view, AdwTabPage *page, LaceWindow *w) {
  LaceConnectionPage *cp = LACE_CONNECTION_PAGE(adw_tab_page_get_child(page));
  LaceConnection *conn = lace_connection_page_get_connection(cp);
  g_signal_handlers_disconnect_by_func(conn, on_conn_state, w);
  g_signal_handlers_disconnect_by_func(conn, on_conn_notice, w);
  lace_connection_close(conn);
  adw_tab_view_close_page_finish(view, page, TRUE);
  return GDK_EVENT_STOP;
}

static void on_pages_changed(GObject *o, GParamSpec *p, LaceWindow *w) {
  (void)o; (void)p;
  update_stack(w);
}

static void on_selected_page(GObject *o, GParamSpec *p, LaceWindow *w) {
  (void)o; (void)p;
  update_title(w);
}

LaceConnectionPage *lace_window_add_connection(LaceWindow *w, LaceConnection *conn) {
  LaceConnectionPage *cp = lace_connection_page_new(conn, lace_app_get_prefs(w->app));
  AdwTabPage *page = adw_tab_view_append(ADW_TAB_VIEW(w->tab_view), GTK_WIDGET(cp));
  adw_tab_page_set_title(page, lace_connection_get_name(conn));
  char *d = lace_connection_get_display(conn);
  adw_tab_page_set_tooltip(page, d);
  g_free(d);
  GIcon *icon = g_icon_new_for_string(lace_connection_get_icon_name(conn), NULL);
  adw_tab_page_set_icon(page, icon);
  g_object_unref(icon);
  adw_tab_view_set_selected_page(ADW_TAB_VIEW(w->tab_view), page);
  g_signal_connect(conn, "tx-changed", G_CALLBACK(on_conn_state), w);
  g_signal_connect(conn, "busy-changed", G_CALLBACK(on_conn_state), w);
  g_signal_connect(conn, "notice", G_CALLBACK(on_conn_notice), w);
  refresh_page_state(w, conn);
  update_stack(w);
  lace_connection_page_focus_sidebar(cp);
  return cp;
}

static void connected_cb(LaceConnection *conn, const char *error, gpointer data) {
  LaceWindow *w = data;
  if (conn) {
    lace_window_add_connection(w, conn);
  } else if (error) {
    lace_error_dialog(GTK_WIDGET(w), "Could not connect", error);
  }
  g_object_unref(w);
}

void lace_window_connect_to(LaceWindow *w, const char *connstr, const char *saved_id,
                            const char *name) {
  lace_app_connect(w->app, GTK_WIDGET(w), connstr, saved_id, name, connected_cb, g_object_ref(w));
}

void lace_window_show_connect_dialog(LaceWindow *w) {
  lace_connect_dialog_show(w, NULL);
}

/* ==========================================================================
 * Actions
 * ========================================================================== */

static GtkWidget *current_view(LaceWindow *w) {
  LaceConnectionPage *cp = lace_window_current_page(w);
  return cp ? lace_connection_page_current_view(cp) : NULL;
}

static void act_connect(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  lace_window_show_connect_dialog(LACE_WINDOW(data));
}

static void act_new_query(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceWindow *w = data;
  LaceConnectionPage *cp = lace_window_current_page(w);
  if (cp)
    lace_connection_page_new_query(cp, NULL);
  else
    lace_toast(GTK_WIDGET(w->toast_overlay), "Connect to a database first (Ctrl+T)");
}

static void act_close_tab(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceWindow *w = data;
  LaceConnectionPage *cp = lace_window_current_page(w);
  if (!cp)
    return;
  if (!lace_connection_page_close_current(cp)) {
    AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(w->tab_view));
    adw_tab_view_close_page(ADW_TAB_VIEW(w->tab_view), page);
  }
}

static void act_close_connection(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceWindow *w = data;
  AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(w->tab_view));
  if (page)
    adw_tab_view_close_page(ADW_TAB_VIEW(w->tab_view), page);
}

static void act_toggle_sidebar(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceConnectionPage *cp = lace_window_current_page(LACE_WINDOW(data));
  if (cp)
    lace_connection_page_set_sidebar_visible(cp, !lace_connection_page_sidebar_visible(cp));
}

static void act_search_tables(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceConnectionPage *cp = lace_window_current_page(LACE_WINDOW(data));
  if (cp)
    lace_connection_page_focus_sidebar(cp);
}

static void history_use(const char *sql, gpointer data) {
  LaceWindow *w = data;
  LaceConnectionPage *cp = lace_window_current_page(w);
  if (!cp)
    return;
  GtkWidget *v = lace_connection_page_current_view(cp);
  if (LACE_IS_QUERY_VIEW(v))
    lace_query_view_insert_text(LACE_QUERY_VIEW(v), sql);
  else
    lace_connection_page_new_query(cp, sql);
}

static void act_history(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceWindow *w = data;
  LaceConnectionPage *cp = lace_window_current_page(w);
  if (cp)
    lace_history_dialog_show(GTK_WIDGET(w), lace_connection_page_get_connection(cp), history_use, w);
}

static void act_view(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)p;
  LaceWindow *w = data;
  const char *name = g_action_get_name(G_ACTION(a));
  GtkWidget *v = current_view(w);
  if (LACE_IS_TABLE_VIEW(v)) {
    LaceTableView *tv = LACE_TABLE_VIEW(v);
    if (g_str_equal(name, "structure")) lace_table_view_show_structure(tv);
    else if (g_str_equal(name, "refresh")) lace_table_view_refresh(tv);
    else if (g_str_equal(name, "filter")) lace_table_view_toggle_filters(tv);
    else if (g_str_equal(name, "goto-row")) lace_table_view_goto_row(tv);
    else if (g_str_equal(name, "add-row")) lace_table_view_add_row(tv);
    else if (g_str_equal(name, "run") || g_str_equal(name, "run-all") || g_str_equal(name, "run-tx"))
      lace_toast(GTK_WIDGET(w->toast_overlay), "Open a SQL editor first (Ctrl+E)");
  } else if (LACE_IS_QUERY_VIEW(v)) {
    LaceQueryView *qv = LACE_QUERY_VIEW(v);
    if (g_str_equal(name, "run")) lace_query_view_run(qv, LACE_RUN_AT_CURSOR);
    else if (g_str_equal(name, "run-all")) lace_query_view_run(qv, LACE_RUN_ALL);
    else if (g_str_equal(name, "run-tx")) lace_query_view_run(qv, LACE_RUN_TRANSACTION);
    else if (g_str_equal(name, "cancel")) lace_query_view_cancel(qv);
    else if (g_str_equal(name, "goto-row")) lace_query_view_goto_row(qv);
    else if (g_str_equal(name, "refresh")) lace_query_view_refresh(qv);
    else if (g_str_equal(name, "structure") || g_str_equal(name, "filter") || g_str_equal(name, "add-row"))
      lace_toast(GTK_WIDGET(w->toast_overlay), "Open a table for that");
  } else if (g_str_equal(name, "cancel")) {
    LaceConnectionPage *cp = lace_window_current_page(w);
    if (cp)
      lace_connection_cancel(lace_connection_page_get_connection(cp));
  }
}

static void act_next_tab(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceConnectionPage *cp = lace_window_current_page(LACE_WINDOW(data));
  if (cp)
    lace_connection_page_select_tab(cp, 1);
}

static void act_prev_tab(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceConnectionPage *cp = lace_window_current_page(LACE_WINDOW(data));
  if (cp)
    lace_connection_page_select_tab(cp, -1);
}

static void rename_cb(const char *text, gpointer data) {
  LaceWindow *w = data;
  if (text && *text)
    lace_window_set_name(w, text);
  g_object_unref(w);
}

static void act_rename(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceWindow *w = data;
  lace_prompt(GTK_WIDGET(w), "Rename workspace", "A name for this window", w->name, "_Rename",
              FALSE, rename_cb, g_object_ref(w));
}

static void act_shortcuts(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceWindow *w = data;
  AdwDialog *d = adw_shortcuts_dialog_new();
  struct { const char *section; const char *title; const char *accel; } items[] = {
      {"Workspaces", "New workspace window", "<Control>n"},
      {"Workspaces", "Rename workspace", "<Control><Shift>r"},
      {"Workspaces", "Preferences", "<Control>comma"},
      {"Workspaces", "Quit", "<Control>q"},
      {"Connections", "Connect (new tab)", "<Control>t"},
      {"Connections", "Close connection", "<Control><Shift>w"},
      {"Connections", "Next / previous connection", "<Control>Page_Down <Control>Page_Up"},
      {"Connections", "Toggle the tables sidebar", "F9"},
      {"Connections", "Search tables", "<Control>k"},
      {"Connections", "Query history", "<Control>h"},
      {"Tabs", "New SQL editor", "<Control>e"},
      {"Tabs", "Close tab", "<Control>w"},
      {"Tabs", "Next / previous tab", "<Control>Tab <Control><Shift>Tab"},
      {"Tables", "Filters", "<Control>f"},
      {"Tables", "Refresh", "F5"},
      {"Tables", "Go to row", "<Control>g"},
      {"Tables", "Structure sub-view (the strip: Data, Structure, Indexes, Foreign keys, Information)", "<Control>i"},
      {"Grid", "Follow a foreign key / the referencing tables", "<Control>click, the context menu"},
      {"Tables", "Add row", "<Control><Shift>n"},
      {"Grid", "Edit cell", "Return F2"},
      {"Grid", "Delete selected rows", "Delete"},
      {"Grid", "Copy cell / copy row as TSV", "<Control>c <Control><Shift>c"},
      {"Grid", "First / last row", "<Control>Up <Control>Down"},
      {"Grid", "Clear the selection", "Escape"},
      {"Editor", "Run the statement at the cursor", "<Control>Return"},
      {"Editor", "Run every statement", "<Control><Shift>Return"},
      {"Editor", "Run everything in a transaction", "<Control><Shift>t"},
      {"Editor", "Stop the running statement", "<Control>period Escape"},
      {"Cell editor", "Save / cancel", "Return Escape"},
      {"Cell editor", "Set NULL / set empty", "<Control>n <Control>d"},
  };
  AdwShortcutsSection *section = NULL;
  const char *current = NULL;
  for (gsize i = 0; i < G_N_ELEMENTS(items); i++) {
    if (g_strcmp0(current, items[i].section) != 0) {
      current = items[i].section;
      section = adw_shortcuts_section_new(current);
      adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(d), section);
    }
    adw_shortcuts_section_add(section, adw_shortcuts_item_new(items[i].title, items[i].accel));
  }
  adw_dialog_present(d, GTK_WIDGET(w));
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

static gboolean on_close_request(GtkWindow *win, gpointer data) {
  (void)data;
  LaceWindow *w = LACE_WINDOW(win);
  GList *wins = lace_app_get_lace_windows(w->app);
  gboolean last = g_list_length(wins) <= 1;
  g_list_free(wins);
  /* The last window closing is the end of the session: remember it */
  lace_session_save(w->app, last ? NULL : w);
  int n = adw_tab_view_get_n_pages(ADW_TAB_VIEW(w->tab_view));
  for (int i = 0; i < n; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(ADW_TAB_VIEW(w->tab_view), i);
    LaceConnectionPage *cp = LACE_CONNECTION_PAGE(adw_tab_page_get_child(page));
    LaceConnection *conn = lace_connection_page_get_connection(cp);
    g_signal_handlers_disconnect_by_func(conn, on_conn_state, w);
    g_signal_handlers_disconnect_by_func(conn, on_conn_notice, w);
    lace_connection_close(conn);
  }
  return GDK_EVENT_PROPAGATE;
}

static void on_saved_changed(LaceApp *app, LaceWindow *w) {
  (void)app;
  if (adw_tab_view_get_n_pages(ADW_TAB_VIEW(w->tab_view)) == 0)
    rebuild_welcome(w);
}

static void lace_window_dispose(GObject *obj) {
  LaceWindow *w = LACE_WINDOW(obj);
  if (w->app)
    g_signal_handlers_disconnect_by_func(w->app, on_saved_changed, w);
  g_clear_pointer(&w->name, g_free);
  G_OBJECT_CLASS(lace_window_parent_class)->dispose(obj);
}

static void lace_window_class_init(LaceWindowClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = lace_window_dispose;
}

static void lace_window_init(LaceWindow *w) { (void)w; }

LaceWindow *lace_window_new(LaceApp *app, const char *name) {
  LaceWindow *w = g_object_new(LACE_TYPE_WINDOW, "application", app, NULL);
  w->app = app;
  g_window_counter++;
  w->name = name && *name ? g_strdup(name)
            : g_window_counter == 1 ? g_strdup("Workspace")
                                    : g_strdup_printf("Workspace %u", g_window_counter);
  gtk_window_set_default_size(GTK_WINDOW(w), 1200, 760);
  gtk_window_set_icon_name(GTK_WINDOW(w), "dev.hexed.lace");

  w->toast_overlay = adw_toast_overlay_new();
  GtkWidget *tv = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  w->title_widget = adw_window_title_new(w->name, "");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), w->title_widget);

  w->sidebar_toggle = lace_icon_button("sidebar-show-symbolic", "Toggle the tables sidebar (F9)", "win.toggle-sidebar");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), w->sidebar_toggle);
  GtkWidget *connect = gtk_button_new();
  GtkWidget *content = adw_button_content_new();
  adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), "list-add-symbolic");
  adw_button_content_set_label(ADW_BUTTON_CONTENT(content), "Connect");
  gtk_button_set_child(GTK_BUTTON(connect), content);
  gtk_widget_set_tooltip_text(connect, "Open a connection in a new tab (Ctrl+T)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(connect), "win.connect");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), connect);

  GMenu *menu = g_menu_new();
  GMenu *s1 = g_menu_new();
  g_menu_append(s1, "New Workspace Window", "app.new-window");
  g_menu_append(s1, "Rename Workspace…", "win.rename");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(s1));
  GMenu *s2 = g_menu_new();
  g_menu_append(s2, "New SQL Editor", "win.new-query");
  g_menu_append(s2, "Query History", "win.history");
  g_menu_append(s2, "Table Structure", "win.structure");
  g_menu_append(s2, "Open SQLite File…", "win.open-file");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(s2));
  GMenu *s3 = g_menu_new();
  g_menu_append(s3, "Preferences", "app.preferences");
  g_menu_append(s3, "Keyboard Shortcuts", "win.shortcuts");
  g_menu_append(s3, "About Lace", "app.about");
  g_menu_append(s3, "Quit", "app.quit");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(s3));
  g_object_unref(s1);
  g_object_unref(s2);
  g_object_unref(s3);
  GtkWidget *menu_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
  gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_button), TRUE);
  g_object_unref(menu);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header),
                          lace_icon_button("document-open-recent-symbolic", "Query history (Ctrl+H)", "win.history"));
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);

  w->tab_view = GTK_WIDGET(adw_tab_view_new());
  adw_tab_view_remove_shortcuts(ADW_TAB_VIEW(w->tab_view), ADW_TAB_VIEW_SHORTCUT_CONTROL_TAB |
                                ADW_TAB_VIEW_SHORTCUT_CONTROL_SHIFT_TAB);
  w->tab_bar = GTK_WIDGET(adw_tab_bar_new());
  adw_tab_bar_set_view(ADW_TAB_BAR(w->tab_bar), ADW_TAB_VIEW(w->tab_view));
  adw_tab_bar_set_autohide(ADW_TAB_BAR(w->tab_bar), FALSE);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), w->tab_bar);

  w->stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(w->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named(GTK_STACK(w->stack), build_welcome(w), "welcome");
  gtk_stack_add_named(GTK_STACK(w->stack), w->tab_view, "tabs");
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), w->stack);
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(w->toast_overlay), tv);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(w), w->toast_overlay);

  static const GActionEntry entries[] = {
      {"connect", act_connect, NULL, NULL, NULL, {0}},
      {"open-file", act_open_file, NULL, NULL, NULL, {0}},
      {"new-query", act_new_query, NULL, NULL, NULL, {0}},
      {"close-tab", act_close_tab, NULL, NULL, NULL, {0}},
      {"close-connection", act_close_connection, NULL, NULL, NULL, {0}},
      {"toggle-sidebar", act_toggle_sidebar, NULL, NULL, NULL, {0}},
      {"search-tables", act_search_tables, NULL, NULL, NULL, {0}},
      {"history", act_history, NULL, NULL, NULL, {0}},
      {"structure", act_view, NULL, NULL, NULL, {0}},
      {"refresh", act_view, NULL, NULL, NULL, {0}},
      {"filter", act_view, NULL, NULL, NULL, {0}},
      {"goto-row", act_view, NULL, NULL, NULL, {0}},
      {"add-row", act_view, NULL, NULL, NULL, {0}},
      {"run", act_view, NULL, NULL, NULL, {0}},
      {"run-all", act_view, NULL, NULL, NULL, {0}},
      {"run-tx", act_view, NULL, NULL, NULL, {0}},
      {"cancel", act_view, NULL, NULL, NULL, {0}},
      {"next-tab", act_next_tab, NULL, NULL, NULL, {0}},
      {"prev-tab", act_prev_tab, NULL, NULL, NULL, {0}},
      {"rename", act_rename, NULL, NULL, NULL, {0}},
      {"shortcuts", act_shortcuts, NULL, NULL, NULL, {0}},
  };
  g_action_map_add_action_entries(G_ACTION_MAP(w), entries, G_N_ELEMENTS(entries), w);

  g_signal_connect(w->tab_view, "close-page", G_CALLBACK(on_close_page), w);
  g_signal_connect(w->tab_view, "notify::n-pages", G_CALLBACK(on_pages_changed), w);
  g_signal_connect(w->tab_view, "notify::selected-page", G_CALLBACK(on_selected_page), w);
  g_signal_connect(w, "close-request", G_CALLBACK(on_close_request), NULL);
  g_signal_connect(app, "saved-changed", G_CALLBACK(on_saved_changed), w);
  update_stack(w);
  return w;
}
