/*
 * Lace GTK frontend
 * Query history dialog
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "history_dialog.h"
#include "util.h"
#include <string.h>

typedef struct {
  AdwDialog *dialog;
  LaceConnection *conn;
  GtkWidget *search;
  GtkWidget *list;
  GtkWidget *stack;
  LaceHistoryUse use_sql;
  gpointer user_data;
} HistoryDialog;

static char *first_line(const char *sql) {
  const char *p = sql;
  while (*p == ' ' || *p == '\n' || *p == '\t')
    p++;
  const char *nl = strchr(p, '\n');
  char *line = nl ? g_strndup(p, (gsize)(nl - p)) : g_strdup(p);
  if (nl) {
    char *more = g_strdup_printf("%s …", line);
    g_free(line);
    line = more;
  }
  return line;
}

static char *when(time_t t) {
  GDateTime *dt = g_date_time_new_from_unix_local((gint64)t);
  GDateTime *now = g_date_time_new_now_local();
  char *s;
  if (g_date_time_get_year(dt) == g_date_time_get_year(now) &&
      g_date_time_get_day_of_year(dt) == g_date_time_get_day_of_year(now))
    s = g_date_time_format(dt, "today %H:%M");
  else
    s = g_date_time_format(dt, "%Y-%m-%d %H:%M");
  g_date_time_unref(dt);
  g_date_time_unref(now);
  return s;
}

static void rebuild(HistoryDialog *h);

static void on_use(GtkButton *b, HistoryDialog *h) {
  const char *sql = g_object_get_data(G_OBJECT(b), "sql");
  if (h->use_sql && sql)
    h->use_sql(sql, h->user_data);
  adw_dialog_close(h->dialog);
}

static void on_copy(GtkButton *b, HistoryDialog *h) {
  const char *sql = g_object_get_data(G_OBJECT(b), "sql");
  lace_copy_text(GTK_WIDGET(h->dialog), sql);
  lace_toast(GTK_WIDGET(h->dialog), "Copied");
}

static void on_delete(GtkButton *b, HistoryDialog *h) {
  gsize index = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(b), "index"));
  lace_connection_history_remove(h->conn, index);
  rebuild(h);
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, HistoryDialog *h) {
  (void)lb;
  const char *sql = g_object_get_data(G_OBJECT(row), "sql");
  if (h->use_sql && sql) {
    h->use_sql(sql, h->user_data);
    adw_dialog_close(h->dialog);
  }
}

static gboolean contains_nocase(const char *hay, const char *needle) {
  if (!needle || !*needle)
    return TRUE;
  char *h = g_utf8_casefold(hay, -1);
  char *n = g_utf8_casefold(needle, -1);
  gboolean r = strstr(h, n) != NULL;
  g_free(h);
  g_free(n);
  return r;
}

static void rebuild(HistoryDialog *h) {
  gtk_list_box_remove_all(GTK_LIST_BOX(h->list));
  LaceHistory *hist = lace_connection_get_history(h->conn);
  const char *needle = gtk_editable_get_text(GTK_EDITABLE(h->search));
  size_t n = lace_history_count(hist);
  guint shown = 0;
  for (size_t i = n; i > 0; i--) {
    const LaceHistoryEntry *e = lace_history_get(hist, i - 1);
    if (!e || !e->sql || !contains_nocase(e->sql, needle))
      continue;
    GtkWidget *row = adw_action_row_new();
    char *title = first_line(e->sql);
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    g_free(title);
    char *w = when(e->timestamp);
    char *sub = g_strdup_printf("%s · %s", lace_history_type_tag(e->type), w);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
    g_free(sub);
    g_free(w);
    gtk_widget_set_tooltip_text(row, e->sql);
    gtk_widget_add_css_class(row, "history-row");
    g_object_set_data_full(G_OBJECT(row), "sql", g_strdup(e->sql), g_free);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), h->use_sql != NULL);

    GtkWidget *use = gtk_button_new_from_icon_name("document-edit-symbolic");
    gtk_widget_set_tooltip_text(use, "Insert into the editor");
    gtk_widget_add_css_class(use, "flat");
    gtk_widget_set_valign(use, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(use, h->use_sql != NULL);
    g_object_set_data_full(G_OBJECT(use), "sql", g_strdup(e->sql), g_free);
    g_signal_connect(use, "clicked", G_CALLBACK(on_use), h);
    GtkWidget *copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
    gtk_widget_set_tooltip_text(copy, "Copy");
    gtk_widget_add_css_class(copy, "flat");
    gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
    g_object_set_data_full(G_OBJECT(copy), "sql", g_strdup(e->sql), g_free);
    g_signal_connect(copy, "clicked", G_CALLBACK(on_copy), h);
    GtkWidget *del = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_set_tooltip_text(del, "Remove from history");
    gtk_widget_add_css_class(del, "flat");
    gtk_widget_set_valign(del, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(del), "index", GSIZE_TO_POINTER(i - 1));
    g_signal_connect(del, "clicked", G_CALLBACK(on_delete), h);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), use);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), copy);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), del);
    gtk_list_box_append(GTK_LIST_BOX(h->list), row);
    shown++;
  }
  gtk_stack_set_visible_child_name(GTK_STACK(h->stack), shown ? "list" : "empty");
}

static void on_search(GtkSearchEntry *e, HistoryDialog *h) {
  (void)e;
  rebuild(h);
}

static void clear_confirmed(gboolean ok, gpointer data) {
  HistoryDialog *h = data;
  if (ok) {
    lace_connection_history_clear(h->conn);
    rebuild(h);
  }
}

static void on_clear(GtkButton *b, HistoryDialog *h) {
  (void)b;
  lace_confirm(GTK_WIDGET(h->dialog), "Clear history",
               "Remove every entry of this connection's query history?", "_Clear", TRUE,
               clear_confirmed, h);
}

static void on_closed(AdwDialog *d, HistoryDialog *h) {
  (void)d;
  g_object_unref(h->conn);
  g_free(h);
}

void lace_history_dialog_show(GtkWidget *parent, LaceConnection *conn,
                              LaceHistoryUse use_sql, gpointer user_data) {
  HistoryDialog *h = g_new0(HistoryDialog, 1);
  h->conn = g_object_ref(conn);
  h->use_sql = use_sql;
  h->user_data = user_data;
  h->dialog = adw_dialog_new();
  adw_dialog_set_title(h->dialog, "Query history");
  adw_dialog_set_content_width(h->dialog, 720);
  adw_dialog_set_content_height(h->dialog, 600);

  GtkWidget *tv = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  GtkWidget *clear = gtk_button_new_with_label("Clear");
  gtk_widget_add_css_class(clear, "flat");
  g_signal_connect(clear, "clicked", G_CALLBACK(on_clear), h);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), clear);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);
  h->search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(h->search), "Search statements");
  gtk_widget_set_margin_start(h->search, 12);
  gtk_widget_set_margin_end(h->search, 12);
  gtk_widget_set_margin_bottom(h->search, 6);
  g_signal_connect(h->search, "search-changed", G_CALLBACK(on_search), h);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), h->search);

  h->stack = gtk_stack_new();
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  h->list = gtk_list_box_new();
  gtk_widget_add_css_class(h->list, "boxed-list");
  gtk_widget_set_margin_start(h->list, 12);
  gtk_widget_set_margin_end(h->list, 12);
  gtk_widget_set_margin_bottom(h->list, 12);
  gtk_widget_set_valign(h->list, GTK_ALIGN_START);
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(h->list), GTK_SELECTION_NONE);
  g_signal_connect(h->list, "row-activated", G_CALLBACK(on_row_activated), h);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), h->list);
  gtk_stack_add_named(GTK_STACK(h->stack), scrolled, "list");
  GtkWidget *empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "document-open-recent-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No history");
  adw_status_page_set_description(ADW_STATUS_PAGE(empty), "Statements you run are listed here.");
  gtk_stack_add_named(GTK_STACK(h->stack), empty, "empty");
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), h->stack);
  adw_dialog_set_child(h->dialog, tv);
  g_signal_connect(h->dialog, "closed", G_CALLBACK(on_closed), h);
  rebuild(h);
  adw_dialog_present(h->dialog, parent);
  gtk_widget_grab_focus(h->search);
}
