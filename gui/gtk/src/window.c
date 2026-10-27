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
#include "views/form_page.h"
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
  GtkWidget *sidebar_toggle;
  guint window_counter;
};

G_DEFINE_TYPE(LaceWindow, lace_window, ADW_TYPE_APPLICATION_WINDOW)

static guint g_window_counter;

static void update_title(LaceWindow *w) {
  adw_window_title_set_title(ADW_WINDOW_TITLE(w->title_widget), w->name);
  AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(w->tab_view));
  if (page && LACE_IS_CONNECTION_PAGE(adw_tab_page_get_child(page))) {
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

guint lace_window_get_number(LaceWindow *w) { return w->window_counter; }

void lace_window_set_name(LaceWindow *w, const char *name) {
  g_free(w->name);
  w->name = g_strdup(name && *name ? name : "Workspace");
  update_title(w);
}

LaceConnectionPage *lace_window_current_page(LaceWindow *w) {
  AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(w->tab_view));
  GtkWidget *child = page ? adw_tab_page_get_child(page) : NULL;
  return LACE_IS_CONNECTION_PAGE(child) ? LACE_CONNECTION_PAGE(child) : NULL;
}

LaceFormPage *lace_window_current_form(LaceWindow *w) {
  AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(w->tab_view));
  GtkWidget *child = page ? adw_tab_page_get_child(page) : NULL;
  return LACE_IS_FORM_PAGE(child) ? LACE_FORM_PAGE(child) : NULL;
}

AdwTabView *lace_window_get_tab_view(LaceWindow *w) { return ADW_TAB_VIEW(w->tab_view); }

/* ==========================================================================
 * Opening a SQLite file: into the current form, or a new one
 * ========================================================================== */

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
  LaceFormPage *form = lace_window_current_form(w);
  if (!form)
    form = lace_window_add_form(w, -1);
  lace_form_page_set_uri(form, connstr);
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

/* ==========================================================================
 * Connection tabs
 * ========================================================================== */

static void update_stack(LaceWindow *w) {
  gtk_widget_set_visible(w->sidebar_toggle, lace_window_current_page(w) != NULL);
  update_title(w);
}

static AdwTabPage *page_of_connection(LaceWindow *w, LaceConnection *conn) {
  int n = adw_tab_view_get_n_pages(ADW_TAB_VIEW(w->tab_view));
  for (int i = 0; i < n; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(ADW_TAB_VIEW(w->tab_view), i);
    GtkWidget *child = adw_tab_page_get_child(page);
    if (LACE_IS_CONNECTION_PAGE(child) && lace_connection_page_get_connection(LACE_CONNECTION_PAGE(child)) == conn)
      return page;
  }
  return NULL;
}

/* The window a tab is in now (tabs can be moved between windows) */
static LaceWindow *window_of_page(GtkWidget *child) {
  GtkRoot *root = gtk_widget_get_root(child);
  return LACE_IS_WINDOW(root) ? LACE_WINDOW(root) : NULL;
}

/* One tab's busy spinner, transaction mark and title, from its connection */
static void refresh_tab(AdwTabPage *page, LaceConnection *conn) {
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

/* The connection's signals reach each of its tabs (data: the page's
 * widget), in whichever window the tab is now */
static void on_conn_state(LaceConnection *conn, LaceConnectionPage *cp) {
  LaceWindow *w = window_of_page(GTK_WIDGET(cp));
  AdwTabPage *page = w ? adw_tab_view_get_page(ADW_TAB_VIEW(w->tab_view), GTK_WIDGET(cp)) : NULL;
  if (page)
    refresh_tab(page, conn);
}

static void on_conn_notice(LaceConnection *conn, const char *msg, LaceConnectionPage *cp) {
  LaceWindow *w = window_of_page(GTK_WIDGET(cp));
  /* Once per window: from the connection's first tab there */
  if (w && page_of_connection(w, conn) == adw_tab_view_get_page(ADW_TAB_VIEW(w->tab_view), GTK_WIDGET(cp)))
    lace_toast(GTK_WIDGET(w->toast_overlay), "%s", msg);
}

/* The page stops listening to its connection */
static void unwire(LaceConnectionPage *cp) {
  g_signal_handlers_disconnect_by_data(lace_connection_page_get_connection(cp), cp);
}

typedef struct {
  LaceWindow *w;
  AdwTabPage *page;
} ClosePage;

static void close_confirmed(gboolean yes, gpointer data) {
  ClosePage *c = data;
  if (yes) {
    LaceConnectionPage *cp = LACE_CONNECTION_PAGE(adw_tab_page_get_child(c->page));
    unwire(cp);
    lace_app_release_connection(c->w->app, lace_connection_page_get_connection(cp));
    adw_tab_view_close_page_finish(ADW_TAB_VIEW(c->w->tab_view), c->page, TRUE);
  } else {
    adw_tab_view_close_page_finish(ADW_TAB_VIEW(c->w->tab_view), c->page, FALSE);
  }
  g_object_unref(c->page);
  g_object_unref(c->w);
  g_free(c);
}

static gboolean on_close_page(AdwTabView *view, AdwTabPage *page, LaceWindow *w) {
  if (!LACE_IS_CONNECTION_PAGE(adw_tab_page_get_child(page))) { /* a form */
    adw_tab_view_close_page_finish(view, page, TRUE);
    return GDK_EVENT_STOP;
  }
  LaceConnectionPage *cp = LACE_CONNECTION_PAGE(adw_tab_page_get_child(page));
  LaceConnection *conn = lace_connection_page_get_connection(cp);
  /* The last page using the connection closes it: ask first when a
   * transaction would be rolled back */
  if (lace_app_connection_holders(w->app, conn) <= 1 && lace_connection_get_tx_state(conn) != LACE_TX_NONE &&
      !g_object_get_data(G_OBJECT(page), "asked")) {
    ClosePage *c = g_new0(ClosePage, 1);
    c->w = g_object_ref(w);
    c->page = g_object_ref(page);
    char *body = g_strdup_printf("A transaction is open on %s. Close and roll back?", lace_connection_get_name(conn));
    lace_confirm(GTK_WIDGET(w), "Close connection", body, "_Close", TRUE, close_confirmed, c);
    g_free(body);
    return GDK_EVENT_STOP;
  }
  unwire(cp);
  lace_app_release_connection(w->app, conn);
  adw_tab_view_close_page_finish(view, page, TRUE);
  return GDK_EVENT_STOP;
}

static gboolean close_if_empty(gpointer data) {
  LaceWindow *w = data;
  if (!gtk_widget_in_destruction(GTK_WIDGET(w)) && adw_tab_view_get_n_pages(ADW_TAB_VIEW(w->tab_view)) == 0)
    gtk_window_close(GTK_WINDOW(w)); /* a workspace left without tabs closes */
  return G_SOURCE_REMOVE;
}

static void on_pages_changed(GObject *o, GParamSpec *p, LaceWindow *w) {
  (void)o; (void)p;
  update_stack(w);
  if (adw_tab_view_get_n_pages(ADW_TAB_VIEW(w->tab_view)) == 0 && !gtk_widget_in_destruction(GTK_WIDGET(w)))
    g_idle_add_full(G_PRIORITY_DEFAULT, close_if_empty, g_object_ref(w), g_object_unref);
}

static void on_selected_page(GObject *o, GParamSpec *p, LaceWindow *w) {
  (void)o; (void)p;
  update_stack(w);
}

/* The first table, once the list is there (auto_open_first_table) */
static void open_first_table(LaceConnectionPage *cp) {
  GListModel *tables = lace_connection_get_tables(lace_connection_page_get_connection(cp));
  if (!tables || g_list_model_get_n_items(tables) == 0 || lace_connection_page_current_view(cp))
    return;
  GtkStringObject *first = g_list_model_get_item(tables, 0);
  lace_connection_page_open_table(cp, gtk_string_object_get_string(first));
  g_object_unref(first);
}

static void on_tables_for_auto_open(LaceConnection *conn, LaceConnectionPage *cp) {
  g_signal_handlers_disconnect_by_func(conn, on_tables_for_auto_open, cp);
  open_first_table(cp);
}

/* The page lets its connection go and a form takes its place */
static void into_form(LaceWindow *w, AdwTabPage *page) {
  int at = adw_tab_view_get_page_position(ADW_TAB_VIEW(w->tab_view), page);
  g_object_set_data(G_OBJECT(page), "asked", GINT_TO_POINTER(1));
  lace_window_add_form(w, at);
  adw_tab_view_close_page(ADW_TAB_VIEW(w->tab_view), page);
}

typedef struct {
  LaceWindow *w;
  AdwTabPage *page;
} IntoForm;

static void into_form_answered(gboolean yes, gpointer data) {
  IntoForm *f = data;
  if (yes && adw_tab_view_get_page_position(ADW_TAB_VIEW(f->w->tab_view), f->page) >= 0)
    into_form(f->w, f->page);
  g_object_unref(f->page);
  g_object_unref(f->w);
  g_free(f);
}

/* The user closed a connection tab's last table or editor: with
 * keep_connection_tab the tab becomes a form in its place, else it closes
 * (either way its hold on the connection goes; the last one asks about an
 * open transaction) */
static void on_last_tab_closed(LaceConnectionPage *cp, gpointer data) {
  (void)data;
  LaceWindow *w = window_of_page(GTK_WIDGET(cp));
  if (!w)
    return;
  AdwTabPage *page = adw_tab_view_get_page(ADW_TAB_VIEW(w->tab_view), GTK_WIDGET(cp));
  if (!page)
    return;
  if (!lace_app_get_prefs(w->app)->keep_connection_tab) {
    adw_tab_view_close_page(ADW_TAB_VIEW(w->tab_view), page);
    return;
  }
  LaceConnection *conn = lace_connection_page_get_connection(cp);
  if (lace_app_connection_holders(w->app, conn) <= 1 && lace_connection_get_tx_state(conn) != LACE_TX_NONE) {
    IntoForm *f = g_new0(IntoForm, 1);
    f->w = g_object_ref(w);
    f->page = g_object_ref(page);
    char *body = g_strdup_printf("A transaction is open on %s. Close and roll back?", lace_connection_get_name(conn));
    lace_confirm(GTK_WIDGET(w), "Close connection", body, "_Close", TRUE, into_form_answered, f);
    g_free(body);
    return;
  }
  into_form(w, page);
}

/* A tab for conn at position (-1: the end), selected or not */
static LaceConnectionPage *insert_connection(LaceWindow *w, LaceConnection *conn, int position, gboolean select,
                                             gboolean auto_open) {
  LaceConnectionPage *cp = lace_connection_page_new(conn, lace_app_get_prefs(w->app));
  AdwTabView *tv = ADW_TAB_VIEW(w->tab_view);
  AdwTabPage *page = position < 0 || position >= adw_tab_view_get_n_pages(tv)
                         ? adw_tab_view_append(tv, GTK_WIDGET(cp))
                         : adw_tab_view_insert(tv, GTK_WIDGET(cp), position);
  adw_tab_page_set_title(page, lace_connection_get_name(conn));
  char *d = lace_connection_get_display(conn);
  adw_tab_page_set_tooltip(page, d);
  g_free(d);
  GIcon *icon = g_icon_new_for_string(lace_connection_get_icon_name(conn), NULL);
  adw_tab_page_set_icon(page, icon);
  g_object_unref(icon);
  if (select)
    adw_tab_view_set_selected_page(tv, page);
  lace_app_hold_connection(w->app, conn);
  g_signal_connect_object(conn, "tx-changed", G_CALLBACK(on_conn_state), cp, 0);
  g_signal_connect_object(conn, "busy-changed", G_CALLBACK(on_conn_state), cp, 0);
  g_signal_connect_object(conn, "notice", G_CALLBACK(on_conn_notice), cp, 0);
  g_signal_connect(cp, "last-tab-closed", G_CALLBACK(on_last_tab_closed), NULL);
  refresh_tab(page, conn);
  update_stack(w);
  if (auto_open && lace_app_get_prefs(w->app)->auto_open_first_table) {
    if (lace_connection_tables_loaded(conn))
      open_first_table(cp);
    else
      g_signal_connect_object(conn, "tables-changed", G_CALLBACK(on_tables_for_auto_open), cp, 0);
  }
  if (select)
    lace_connection_page_focus_sidebar(cp);
  return cp;
}

LaceConnectionPage *lace_window_add_connection(LaceWindow *w, LaceConnection *conn) {
  return insert_connection(w, conn, -1, TRUE, TRUE);
}

static int position_of(LaceWindow *w, GtkWidget *child) {
  AdwTabPage *page = adw_tab_view_get_page(ADW_TAB_VIEW(w->tab_view), child);
  return page ? adw_tab_view_get_page_position(ADW_TAB_VIEW(w->tab_view), page) : -1;
}

LaceConnectionPage *lace_window_place_here(LaceWindow *w, GtkWidget *form, LaceConnection *conn) {
  int at = position_of(w, form);
  LaceConnectionPage *cp = insert_connection(w, conn, at, TRUE, TRUE);
  AdwTabPage *fp = at >= 0 ? adw_tab_view_get_page(ADW_TAB_VIEW(w->tab_view), form) : NULL;
  if (fp)
    adw_tab_view_close_page(ADW_TAB_VIEW(w->tab_view), fp);
  return cp;
}

LaceConnectionPage *lace_window_place_after(LaceWindow *w, GtkWidget *form, LaceConnection *conn) {
  int at = position_of(w, form);
  return insert_connection(w, conn, at < 0 ? -1 : at + 1, FALSE, FALSE);
}

static void on_form_label(LaceFormPage *form, gpointer data) {
  (void)data;
  LaceWindow *w = window_of_page(GTK_WIDGET(form)); /* where the tab is now */
  if (!w)
    return;
  AdwTabPage *page = adw_tab_view_get_page(ADW_TAB_VIEW(w->tab_view), GTK_WIDGET(form));
  if (page)
    adw_tab_page_set_title(page, lace_form_page_get_label(form));
}

LaceFormPage *lace_window_add_form(LaceWindow *w, int position) {
  LaceFormPage *form = lace_form_page_new(w);
  AdwTabView *tv = ADW_TAB_VIEW(w->tab_view);
  AdwTabPage *sel = adw_tab_view_get_selected_page(tv);
  if (position < 0)
    position = sel ? adw_tab_view_get_page_position(tv, sel) + 1 : adw_tab_view_get_n_pages(tv);
  AdwTabPage *page = position >= adw_tab_view_get_n_pages(tv) ? adw_tab_view_append(tv, GTK_WIDGET(form))
                                                              : adw_tab_view_insert(tv, GTK_WIDGET(form), position);
  adw_tab_page_set_title(page, lace_form_page_get_label(form));
  GIcon *icon = g_icon_new_for_string("network-server-symbolic", NULL);
  adw_tab_page_set_icon(page, icon);
  g_object_unref(icon);
  g_signal_connect(form, "label-changed", G_CALLBACK(on_form_label), NULL);
  adw_tab_view_set_selected_page(tv, page);
  update_stack(w);
  return form;
}

static void connected_cb(LaceConnection *conn, const char *error, gpointer data) {
  LaceWindow *w = data;
  if (conn) {
    lace_window_add_connection(w, conn);
  } else {
    if (adw_tab_view_get_n_pages(ADW_TAB_VIEW(w->tab_view)) == 0)
      lace_window_add_form(w, -1); /* never an empty window */
    if (error)
      lace_error_dialog(GTK_WIDGET(w), "Could not connect", error);
  }
  g_object_unref(w);
}

void lace_window_connect_to(LaceWindow *w, const char *connstr, const char *saved_id,
                            const char *name) {
  lace_app_open(w->app, GTK_WIDGET(w), connstr, saved_id, name, connected_cb, g_object_ref(w));
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
  lace_window_add_form(LACE_WINDOW(data), -1); /* right after the current tab */
}

static void act_new_query(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceWindow *w = data;
  LaceConnectionPage *cp = lace_window_current_page(w);
  if (cp)
    lace_connection_page_new_query(cp, NULL);
  else
    lace_toast(GTK_WIDGET(w->toast_overlay), "Open a connection first");
}

static void act_close_tab(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceWindow *w = data;
  LaceConnectionPage *cp = lace_window_current_page(w);
  if (!cp) { /* a form: the tab itself */
    AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(w->tab_view));
    if (page)
      adw_tab_view_close_page(ADW_TAB_VIEW(w->tab_view), page);
    return;
  }
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
      {"Connections", "New connection tab", "<Control>t"},
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
  /* on_last_workspace_close "new": a fresh window with a form ("quit": the
   * application ends with its last window) */
  if (last && lace_app_get_prefs(w->app)->last_workspace_close == CONFIG_LAST_WS_NEW)
    lace_app_new_window(w->app, NULL);
  int n = adw_tab_view_get_n_pages(ADW_TAB_VIEW(w->tab_view));
  for (int i = 0; i < n; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(ADW_TAB_VIEW(w->tab_view), i);
    GtkWidget *child = adw_tab_page_get_child(page);
    if (!LACE_IS_CONNECTION_PAGE(child))
      continue;
    LaceConnection *conn = lace_connection_page_get_connection(LACE_CONNECTION_PAGE(child));
    unwire(LACE_CONNECTION_PAGE(child));
    lace_app_release_connection(w->app, conn);
  }
  return GDK_EVENT_PROPAGATE;
}

static void lace_window_dispose(GObject *obj) {
  LaceWindow *w = LACE_WINDOW(obj);
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
  w->window_counter = g_window_counter;
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
  gtk_widget_set_tooltip_text(connect, "A new connection tab (Ctrl+T)");
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

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), w->tab_view);
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
  update_stack(w);
  return w;
}
