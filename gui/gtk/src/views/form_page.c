/*
 * Lace GTK frontend
 * LaceFormPage: a connection form tab
 *
 * The fields are a ConnFields (core/conn_fields.c, the TUIs' model): every
 * row writes its text into it, and the URI row shows conn_fields_uri() (every
 * password masked) after each field edit. What is typed into the URI row goes
 * into the fields when the row loses the focus or an action runs, never while
 * it is typed (a half-typed URI often parses, and would empty the fields).
 * Highlighting a saved connection or a Recent entry fills the form, after
 * "Drop the changes in the form?" when it holds edits. Open uses the saved
 * entry while the fields are as it filled them, else the string the fields
 * build; an open connection to the same database is used again
 * (lace_app_open).
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "form_page.h"
#include "core/conn_fields.h"
#include "util.h"
#include "window.h"
#include <config/recent.h>
#include <config/saved_connection.h>
#include <util/connstr.h>
#include <util/str.h>
#include <stdlib.h>
#include <string.h>

typedef enum { ROW_CONN = 1, ROW_FOLDER, ROW_RECENT } RowKind;

struct _LaceFormPage {
  GtkWidget parent_instance;
  LaceApp *app;
  ConnFields fields;
  GtkWidget *paned;
  GtkWidget *search, *list;
  GtkWidget *uri_row, *message;
  GtkWidget *row[CF_COUNT]; /* the field rows (CF_SSH: the expander) */
  GtkWidget *folder_row;
  GtkWidget *open_button, *to_window_button;
  GPtrArray *folder_paths; /* "" is the top level */
  RecentList recent;
  char *highlight_id;    /* the highlighted saved connection */
  char *label;           /* the tab's label */
  GtkListBoxRow *picked; /* the highlighted row (not owned) */
  gboolean filling;      /* rows being set from the fields */
  gboolean uri_edited;   /* typed into since the fields were applied to it */
  gboolean reverting;    /* the list's selection going back after "Keep" */
  /* What the list's context menu was opened on, copied (the rows are rebuilt
   * at any time): its kind, a saved connection's id, a folder's path, a
   * Recent index */
  RowKind menu_kind;
  char *menu_id, *menu_path;
  gsize menu_index;
};

enum { SIG_LABEL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(LaceFormPage, lace_form_page, GTK_TYPE_WIDGET)

/* The window the page is in now (a tab can be moved to another one) */
static LaceWindow *window_of(LaceFormPage *p) {
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(p));
  return LACE_IS_WINDOW(root) ? LACE_WINDOW(root) : NULL;
}

/* A widget to put dialogs over */
static GtkWidget *parent_of(LaceFormPage *p) {
  LaceWindow *w = window_of(p);
  return w ? GTK_WIDGET(w) : GTK_WIDGET(p);
}

static void rebuild_list(LaceFormPage *p);
static void refresh_folders(LaceFormPage *p);

/* ==========================================================================
 * Fields and rows
 * ========================================================================== */

static gboolean is_text(int f) { return f != CF_DRIVER && !conn_fields_is_check(f); }

static void set_label(LaceFormPage *p, const char *name) {
  const char *l = name && *name ? name : "New connection";
  if (g_strcmp0(p->label, l) == 0)
    return;
  g_free(p->label);
  p->label = g_strdup(l);
  g_signal_emit(p, signals[SIG_LABEL_CHANGED], 0);
}

static void set_highlight(LaceFormPage *p, const char *id, const char *name) {
  g_free(p->highlight_id);
  p->highlight_id = id ? g_strdup(id) : NULL;
  set_label(p, name);
}

static void show_message(LaceFormPage *p) {
  const char *m = p->fields.message;
  gtk_label_set_text(GTK_LABEL(p->message), m ? m : "");
  gtk_widget_set_visible(p->message, m && *m);
  if (p->fields.message_error)
    gtk_widget_add_css_class(p->message, "error");
  else
    gtk_widget_remove_css_class(p->message, "error");
}

static void say(LaceFormPage *p, gboolean error, const char *text) {
  conn_fields_set_message(&p->fields, error, "%s", text);
  show_message(p);
}

static void sync_uri(LaceFormPage *p) {
  char *u = conn_fields_uri(&p->fields);
  gboolean was = p->filling;
  p->filling = TRUE;
  gtk_editable_set_text(GTK_EDITABLE(p->uri_row), u ? u : "");
  p->filling = was;
  str_secure_free(u);
  p->uri_edited = FALSE;
}

static void driver_model(LaceFormPage *p) {
  GtkStringList *list = gtk_string_list_new(NULL);
  for (int i = 0; i < conn_fields_num_drivers(&p->fields); i++)
    gtk_string_list_append(list, conn_fields_driver_name(&p->fields, i));
  adw_combo_row_set_model(ADW_COMBO_ROW(p->row[CF_DRIVER]), G_LIST_MODEL(list));
  g_object_unref(list);
}

static void update_visibility(LaceFormPage *p) {
  for (int f = 0; f < CF_COUNT; f++)
    if (p->row[f] && f != CF_SSH && f != CF_SSHHOST && f != CF_SSHPORT && f != CF_SSHUSER && f != CF_SSHKEY &&
        f != CF_SSHPASS && f != CF_SAVESSHPW)
      gtk_widget_set_visible(p->row[f], conn_fields_visible(&p->fields, f));
  gtk_widget_set_visible(p->row[CF_SSH], conn_fields_visible(&p->fields, CF_SSH));
}

/* Every row from the fields */
static void fields_to_rows(LaceFormPage *p) {
  gboolean was = p->filling;
  p->filling = TRUE;
  if (g_list_model_get_n_items(adw_combo_row_get_model(ADW_COMBO_ROW(p->row[CF_DRIVER]))) !=
      (guint)conn_fields_num_drivers(&p->fields))
    driver_model(p);
  adw_combo_row_set_selected(ADW_COMBO_ROW(p->row[CF_DRIVER]), (guint)p->fields.driver);
  for (int f = 0; f < CF_COUNT; f++) {
    if (is_text(f))
      gtk_editable_set_text(GTK_EDITABLE(p->row[f]), p->fields.in[f].buf ? p->fields.in[f].buf : "");
  }
  adw_switch_row_set_active(ADW_SWITCH_ROW(p->row[CF_SAVEPW]), p->fields.chk[CF_SAVEPW]);
  adw_switch_row_set_active(ADW_SWITCH_ROW(p->row[CF_SAVESSHPW]), p->fields.chk[CF_SAVESSHPW]);
  adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(p->row[CF_SSH]), p->fields.chk[CF_SSH]);
  update_visibility(p);
  p->filling = was;
  sync_uri(p);
  show_message(p);
}

static void on_text_changed(GtkEditable *e, LaceFormPage *p) {
  if (p->filling)
    return;
  int f = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(e), "field"));
  text_input_set(&p->fields.in[f], gtk_editable_get_text(e));
  sync_uri(p);
}

static void on_switch_changed(GObject *row, GParamSpec *ps, LaceFormPage *p) {
  (void)ps;
  if (p->filling)
    return;
  int f = GPOINTER_TO_INT(g_object_get_data(row, "field"));
  p->fields.chk[f] = f == CF_SSH ? adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(row))
                                 : adw_switch_row_get_active(ADW_SWITCH_ROW(row));
  update_visibility(p);
  sync_uri(p);
}

static void on_driver_changed(GObject *row, GParamSpec *ps, LaceFormPage *p) {
  (void)ps;
  if (p->filling)
    return;
  guint sel = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
  if (sel == GTK_INVALID_LIST_POSITION)
    return;
  p->fields.driver = (int)sel;
  update_visibility(p);
  sync_uri(p);
}

static void on_uri_changed(GtkEditable *e, LaceFormPage *p) {
  (void)e;
  if (!p->filling)
    p->uri_edited = TRUE;
}

/* What the URI row holds goes into the fields; FALSE (the message says why,
 * the text stays) when it does not parse */
static gboolean commit_uri(LaceFormPage *p) {
  if (!p->uri_edited)
    return TRUE;
  const char *t = gtk_editable_get_text(GTK_EDITABLE(p->uri_row));
  if (!t || !*t) { /* cleared: the fields stay as they are */
    sync_uri(p);
    return TRUE;
  }
  if (!conn_fields_set_uri(&p->fields, t)) {
    show_message(p);
    return FALSE;
  }
  fields_to_rows(p);
  return TRUE;
}

static void on_uri_leave(GtkEventControllerFocus *c, LaceFormPage *p) {
  (void)c;
  commit_uri(p);
}

/* Edits not saved: in the fields, or typed into the URI row */
static gboolean form_dirty(LaceFormPage *p) { return p->uri_edited || conn_fields_dirty(&p->fields); }

/* ==========================================================================
 * The left panel: saved connections and Recent
 * ========================================================================== */

static void collect_folders(ConnectionItem *folder, const char *prefix, GPtrArray *out) {
  for (size_t i = 0; folder && i < folder->folder.num_children; i++) {
    ConnectionItem *it = &folder->folder.children[i];
    if (it->type != CONN_ITEM_FOLDER)
      continue;
    char *path = *prefix ? g_strdup_printf("%s/%s", prefix, it->folder.name) : g_strdup(it->folder.name);
    g_ptr_array_add(out, path);
    collect_folders(it, path, out);
  }
}

static ConnectionItem *folder_by_path(ConnectionManager *mgr, const char *path) {
  ConnectionItem *cur = &mgr->root;
  if (!path || !*path)
    return cur;
  char **parts = g_strsplit(path, "/", -1);
  for (int i = 0; parts[i] && cur; i++) {
    ConnectionItem *next = NULL;
    for (size_t k = 0; k < cur->folder.num_children; k++) {
      ConnectionItem *it = &cur->folder.children[k];
      if (it->type == CONN_ITEM_FOLDER && g_strcmp0(it->folder.name, parts[i]) == 0)
        next = it;
    }
    cur = next;
  }
  g_strfreev(parts);
  return cur;
}

static char *folder_path_of(ConnectionItem *item) {
  GPtrArray *names = g_ptr_array_new();
  for (ConnectionItem *f = item ? item->parent : NULL; f && f->parent; f = f->parent)
    g_ptr_array_add(names, f->folder.name);
  GString *s = g_string_new(NULL);
  for (guint i = names->len; i > 0; i--) {
    if (s->len)
      g_string_append_c(s, '/');
    g_string_append(s, g_ptr_array_index(names, i - 1));
  }
  g_ptr_array_unref(names);
  return g_string_free(s, FALSE);
}

static void refresh_folders(LaceFormPage *p) {
  /* The folder shown stays shown (the list is rebuilt after every save) */
  guint sel = p->folder_row ? adw_combo_row_get_selected(ADW_COMBO_ROW(p->folder_row)) : GTK_INVALID_LIST_POSITION;
  char *keep = sel < p->folder_paths->len ? g_strdup(g_ptr_array_index(p->folder_paths, sel)) : NULL;
  g_ptr_array_set_size(p->folder_paths, 0);
  g_ptr_array_add(p->folder_paths, g_strdup(""));
  collect_folders(&lace_app_get_saved(p->app)->root, "", p->folder_paths);
  GtkStringList *list = gtk_string_list_new(NULL);
  for (guint i = 0; i < p->folder_paths->len; i++) {
    const char *f = g_ptr_array_index(p->folder_paths, i);
    gtk_string_list_append(list, *f ? f : "(top level)");
  }
  adw_combo_row_set_model(ADW_COMBO_ROW(p->folder_row), G_LIST_MODEL(list));
  g_object_unref(list);
  for (guint i = 0; keep && i < p->folder_paths->len; i++)
    if (g_str_equal(g_ptr_array_index(p->folder_paths, i), keep))
      adw_combo_row_set_selected(ADW_COMBO_ROW(p->folder_row), i);
  g_free(keep);
}

static void select_folder(LaceFormPage *p, const char *path) {
  for (guint i = 0; i < p->folder_paths->len; i++)
    if (g_strcmp0(g_ptr_array_index(p->folder_paths, i), path ? path : "") == 0)
      adw_combo_row_set_selected(ADW_COMBO_ROW(p->folder_row), i);
}

static gboolean matches(const char *text, const char *needle) {
  if (!needle || !*needle)
    return TRUE;
  if (!text)
    return FALSE;
  char *a = g_utf8_casefold(text, -1), *b = g_utf8_casefold(needle, -1);
  gboolean hit = strstr(a, b) != NULL;
  g_free(a);
  g_free(b);
  return hit;
}

static const char *driver_icon(const char *driver) {
  if (g_strcmp0(driver, "sqlite") == 0)
    return "lace-sqlite-symbolic";
  if (g_strcmp0(driver, "postgres") == 0 || g_strcmp0(driver, "postgresql") == 0)
    return "lace-postgres-symbolic";
  return "lace-mysql-symbolic";
}

static void add_header(LaceFormPage *p, const char *title) {
  GtkWidget *row = gtk_list_box_row_new();
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  GtkWidget *l = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(l), 0);
  gtk_widget_add_css_class(l, "heading");
  gtk_widget_add_css_class(l, "dim-label");
  gtk_widget_set_margin_top(l, 6);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), l);
  gtk_list_box_append(GTK_LIST_BOX(p->list), row);
}

static GtkWidget *add_row(LaceFormPage *p, RowKind kind, const char *title, const char *subtitle,
                          const char *icon, int depth) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title ? title : "");
  if (subtitle && *subtitle)
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  GtkWidget *img = gtk_image_new_from_icon_name(icon);
  gtk_widget_set_margin_start(img, depth * 12);
  adw_action_row_add_prefix(ADW_ACTION_ROW(row), img);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
  g_object_set_data(G_OBJECT(row), "kind", GINT_TO_POINTER(kind));
  g_object_set_data_full(G_OBJECT(row), "title", g_strdup(title ? title : ""), g_free);
  gtk_list_box_append(GTK_LIST_BOX(p->list), row);
  return row;
}

/* "1, 3": the windows showing saved connection id ("" if none) */
static char *open_in(LaceApp *app, const char *id) {
  GString *s = g_string_new(NULL);
  GList *wins = lace_app_get_lace_windows(app);
  for (GList *l = wins; l && id; l = l->next) {
    AdwTabView *tv = lace_window_get_tab_view(l->data);
    for (int i = 0; i < adw_tab_view_get_n_pages(tv); i++) {
      GtkWidget *c = adw_tab_page_get_child(adw_tab_view_get_nth_page(tv, i));
      if (!LACE_IS_CONNECTION_PAGE(c))
        continue;
      const char *sid = lace_connection_get_saved_id(lace_connection_page_get_connection(LACE_CONNECTION_PAGE(c)));
      if (g_strcmp0(sid, id) == 0) {
        g_string_append_printf(s, "%s%u", s->len ? ", " : "", lace_window_get_number(l->data));
        break;
      }
    }
  }
  g_list_free(wins);
  return g_string_free(s, FALSE);
}

static void walk_tree(LaceFormPage *p, ConnectionItem *folder, const char *prefix, int depth, const char *needle) {
  for (size_t i = 0; folder && i < folder->folder.num_children; i++) {
    ConnectionItem *it = &folder->folder.children[i];
    if (it->type == CONN_ITEM_FOLDER) {
      char *path = *prefix ? g_strdup_printf("%s/%s", prefix, it->folder.name) : g_strdup(it->folder.name);
      gboolean open = it->folder.expanded || (needle && *needle);
      if (!needle || !*needle) {
        GtkWidget *row = add_row(p, ROW_FOLDER, it->folder.name, NULL,
                                 open ? "folder-open-symbolic" : "folder-symbolic", depth);
        g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path), g_free);
      }
      if (open)
        walk_tree(p, it, path, depth + 1, needle);
      g_free(path);
      continue;
    }
    const LaceSavedConnection *c = &it->connection;
    if (!matches(c->name, needle) && !matches(c->host, needle) && !matches(c->database, needle))
      continue;
    char *in = open_in(p->app, c->id);
    char *sub;
    if (g_strcmp0(c->driver, "sqlite") == 0)
      sub = g_strdup_printf("%s%s%s", c->database ? c->database : "", *in ? " · open in " : "", in);
    else
      sub = g_strdup_printf("%s%s%s/%s%s%s%s", c->user && *c->user ? c->user : "", c->user && *c->user ? "@" : "",
                            c->host ? c->host : "", c->database ? c->database : "", c->ssh ? " · ssh" : "",
                            *in ? " · open in " : "", in);
    GtkWidget *row = add_row(p, ROW_CONN, c->name, sub, driver_icon(c->driver), needle && *needle ? 0 : depth);
    g_object_set_data_full(G_OBJECT(row), "id", g_strdup(c->id), g_free);
    if (p->highlight_id && g_str_equal(p->highlight_id, c->id ? c->id : "")) {
      p->reverting = TRUE;
      gtk_list_box_select_row(GTK_LIST_BOX(p->list), GTK_LIST_BOX_ROW(row));
      p->reverting = FALSE;
      p->picked = GTK_LIST_BOX_ROW(row);
    }
    g_free(sub);
    g_free(in);
  }
}

/* A recent URL's short name: a SQLite file's name, else "db on host" */
static char *short_name(const char *url) {
  ConnString *cs = url ? connstr_parse(url, NULL) : NULL;
  if (!cs)
    return g_strdup(url ? url : "");
  char *out;
  if (cs->driver && g_str_equal(cs->driver, "sqlite")) {
    const char *db = cs->database ? cs->database : "";
    const char *slash = strrchr(db, '/');
    out = g_strdup(slash && slash[1] ? slash + 1 : db);
  } else {
    const char *host = cs->host && *cs->host ? cs->host : "localhost";
    out = cs->database && *cs->database ? g_strdup_printf("%s on %s", cs->database, host) : g_strdup(host);
  }
  connstr_free(cs);
  return out;
}

static void rebuild_list(LaceFormPage *p) {
  p->picked = NULL;
  p->reverting = TRUE;
  gtk_list_box_remove_all(GTK_LIST_BOX(p->list));
  p->reverting = FALSE;
  const char *needle = gtk_editable_get_text(GTK_EDITABLE(p->search));
  add_header(p, "Saved");
  walk_tree(p, &lace_app_get_saved(p->app)->root, "", 0, needle);
  recent_free(&p->recent);
  recent_load(&p->recent);
  gboolean header = FALSE;
  for (size_t i = 0; i < p->recent.count; i++) {
    char *name = short_name(p->recent.items[i].url);
    char *display = connstr_mask_password(p->recent.items[i].url);
    if (matches(name, needle) || matches(p->recent.items[i].url, needle)) {
      if (!header)
        add_header(p, "Recent");
      header = TRUE;
      GtkWidget *row = add_row(p, ROW_RECENT, name, display, "document-open-recent-symbolic", 0);
      g_object_set_data(G_OBJECT(row), "index", GSIZE_TO_POINTER(i));
    }
    free(display);
    g_free(name);
  }
}

static void on_saved_changed(LaceApp *app, LaceFormPage *p) {
  (void)app;
  refresh_folders(p);
  rebuild_list(p);
}

/* The row of the list as it is now that names what row named (the list
 * may have been rebuilt while a question was open); NULL if gone */
static GtkListBoxRow *current_row(LaceFormPage *p, GtkListBoxRow *row) {
  if (gtk_widget_get_parent(GTK_WIDGET(row)) == p->list)
    return row;
  const char *keys[] = {"id", "path"};
  for (GtkWidget *c = gtk_widget_get_first_child(p->list); c; c = gtk_widget_get_next_sibling(c)) {
    if (g_object_get_data(G_OBJECT(c), "kind") != g_object_get_data(G_OBJECT(row), "kind"))
      continue;
    for (gsize k = 0; k < G_N_ELEMENTS(keys); k++) {
      const char *a = g_object_get_data(G_OBJECT(c), keys[k]), *b = g_object_get_data(G_OBJECT(row), keys[k]);
      if (a && b && g_str_equal(a, b))
        return GTK_LIST_BOX_ROW(c);
    }
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind")) == ROW_RECENT &&
        g_object_get_data(G_OBJECT(c), "index") == g_object_get_data(G_OBJECT(row), "index"))
      return GTK_LIST_BOX_ROW(c);
  }
  return NULL;
}

/* The row becomes the highlight and fills the form */
static void apply_pick(LaceFormPage *p, GtkListBoxRow *row) {
  RowKind kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind"));
  p->picked = current_row(p, row); /* never a row the list no longer holds */
  if (kind == ROW_CONN) {
    ConnectionItem *it = connmgr_find_by_id(lace_app_get_saved(p->app), g_object_get_data(G_OBJECT(row), "id"));
    if (!it || it->type != CONN_ITEM_CONNECTION)
      return;
    conn_fields_fill(&p->fields, &it->connection);
    set_highlight(p, it->connection.id, it->connection.name);
    char *path = folder_path_of(it);
    select_folder(p, path);
    g_free(path);
  } else if (kind == ROW_RECENT) {
    gsize i = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "index"));
    conn_fields_clear(&p->fields);
    if (i < p->recent.count)
      conn_fields_set_uri(&p->fields, p->recent.items[i].url);
    conn_fields_mark_clean(&p->fields);
    set_highlight(p, NULL, NULL);
  } else { /* a folder: opens or closes, the form stays */
    ConnectionItem *f = folder_by_path(lace_app_get_saved(p->app), g_object_get_data(G_OBJECT(row), "path"));
    if (f) {
      connmgr_toggle_folder(f);
      lace_app_save_saved(p->app, parent_of(p));
    }
    return;
  }
  fields_to_rows(p);
}

typedef struct {
  LaceFormPage *page;
  GtkListBoxRow *row;
} Pick;

static void drop_answered(gboolean drop, gpointer data) {
  Pick *k = data;
  LaceFormPage *p = k->page;
  if (drop) {
    GtkListBoxRow *row = current_row(p, k->row);
    if (row)
      apply_pick(p, row);
  } else { /* the selection goes back */
    p->reverting = TRUE;
    if (p->picked)
      gtk_list_box_select_row(GTK_LIST_BOX(p->list), p->picked);
    else
      gtk_list_box_unselect_all(GTK_LIST_BOX(p->list));
    p->reverting = FALSE;
  }
  g_object_unref(k->row);
  g_object_unref(p);
  g_free(k);
}

static void request_pick(LaceFormPage *p, GtkListBoxRow *row) {
  if (row == p->picked)
    return;
  if (!form_dirty(p) || GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind")) == ROW_FOLDER) {
    apply_pick(p, row);
    return;
  }
  Pick *k = g_new0(Pick, 1);
  k->page = g_object_ref(p);
  k->row = g_object_ref(row);
  lace_confirm(GTK_WIDGET(p), "Unsaved changes", "Drop the changes in the form?", "_Drop", TRUE, drop_answered, k);
}

static void on_row_selected(GtkListBox *lb, GtkListBoxRow *row, LaceFormPage *p) {
  (void)lb;
  if (row && !p->reverting)
    request_pick(p, row);
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, LaceFormPage *p) {
  (void)lb;
  RowKind kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind"));
  if (kind != ROW_FOLDER && row == p->picked)
    lace_form_page_action(p, "open");
}

static void on_search_changed(GtkSearchEntry *e, LaceFormPage *p) {
  (void)e;
  rebuild_list(p);
}

/* ==========================================================================
 * Actions
 * ========================================================================== */

typedef struct {
  LaceFormPage *page; /* weak: Open here frees the page */
  LaceWindow *window;
  char *action;
  guint target; /* to-window: the window's number */
} OpenCtx;

static LaceWindow *window_by_number(LaceApp *app, guint n) {
  GList *wins = lace_app_get_lace_windows(app);
  LaceWindow *found = NULL;
  for (GList *l = wins; l; l = l->next)
    if (lace_window_get_number(l->data) == n)
      found = l->data;
  g_list_free(wins);
  return found;
}

static void opened(LaceConnection *conn, const char *error, gpointer data) {
  OpenCtx *o = data;
  LaceFormPage *p = o->page;
  if (!conn) {
    if (p && error)
      say(p, TRUE, error);
  } else if (g_str_equal(o->action, "background")) {
    if (p)
      lace_window_place_after(o->window, GTK_WIDGET(p), conn);
    else
      lace_window_add_connection(o->window, conn);
  } else if (g_str_equal(o->action, "new-window")) {
    LaceWindow *nw = lace_window_new(lace_app_get_default(), NULL); /* its only tab: this one */
    lace_window_add_connection(nw, conn);
    gtk_window_present(GTK_WINDOW(nw));
  } else if (g_str_equal(o->action, "to-window")) {
    LaceWindow *to = window_by_number(lace_app_get_default(), o->target);
    if (to) {
      lace_window_add_connection(to, conn);
      gtk_window_present(GTK_WINDOW(to));
    } else if (p) {
      say(p, TRUE, "That window was closed");
    }
  } else if (p) {
    lace_window_place_here(o->window, GTK_WIDGET(p), conn); /* frees p */
  } else {
    lace_window_add_connection(o->window, conn);
  }
  if (o->page)
    g_object_remove_weak_pointer(G_OBJECT(o->page), (gpointer *)&o->page);
  g_object_unref(o->window);
  g_free(o->action);
  g_free(o);
}

static void open_with(LaceFormPage *p, const char *action, guint target) {
  if (!commit_uri(p))
    return; /* what the URI row shows does not parse: nothing opens */
  ConnectionItem *it = p->highlight_id && !conn_fields_dirty(&p->fields)
                           ? connmgr_find_by_id(lace_app_get_saved(p->app), p->highlight_id)
                           : NULL;
  char *connstr = NULL;
  const char *saved_id = NULL, *name = NULL;
  if (it && it->type == CONN_ITEM_CONNECTION) {
    const LaceSavedConnection *c = &it->connection;
    connstr = lace_saved_conn_build_connstr(c, c->password && *c->password ? c->password : NULL,
                                            c->ssh_password && *c->ssh_password ? c->ssh_password : NULL);
    saved_id = c->id;
    name = c->name;
  } else {
    connstr = conn_fields_connstr(&p->fields);
    name = p->fields.in[CF_NAME].buf && *p->fields.in[CF_NAME].buf ? p->fields.in[CF_NAME].buf : NULL;
  }
  if (!connstr) {
    show_message(p);
    return;
  }
  say(p, FALSE, "Connecting\xe2\x80\xa6");
  LaceWindow *here = window_of(p);
  if (!here) {
    str_secure_free(connstr);
    return;
  }
  OpenCtx *o = g_new0(OpenCtx, 1);
  o->page = p;
  g_object_add_weak_pointer(G_OBJECT(p), (gpointer *)&o->page);
  o->window = g_object_ref(here);
  o->action = g_strdup(action);
  o->target = target;
  lace_app_open(p->app, parent_of(p), connstr, saved_id, name, opened, o);
  str_secure_free(connstr);
}

static void tested(LaceConnection *conn, const char *error, gpointer data) {
  (void)conn;
  LaceFormPage *p = data;
  say(p, error != NULL, error ? error : "Connection works");
  g_object_unref(p);
}

static void test(LaceFormPage *p) {
  if (!commit_uri(p))
    return;
  char *connstr = conn_fields_connstr(&p->fields);
  if (!connstr) {
    show_message(p);
    return;
  }
  say(p, FALSE, "Testing\xe2\x80\xa6");
  lace_app_test_connection(p->app, parent_of(p), connstr, tested, g_object_ref(p));
  str_secure_free(connstr);
}

static void clear(LaceFormPage *p) {
  conn_fields_clear(&p->fields);
  set_highlight(p, NULL, NULL);
  p->picked = NULL;
  p->reverting = TRUE;
  gtk_list_box_unselect_all(GTK_LIST_BOX(p->list));
  p->reverting = FALSE;
  select_folder(p, "");
  fields_to_rows(p);
}

/* Write c (taken over): over the highlighted entry, or as a new one in the
 * Folder row's folder; the entry moves when the folder changed */
static void store(LaceFormPage *p, LaceSavedConnection *c, gboolean as_new) {
  ConnectionManager *mgr = lace_app_get_saved(p->app);
  guint fi = adw_combo_row_get_selected(ADW_COMBO_ROW(p->folder_row));
  const char *path = fi < p->folder_paths->len ? g_ptr_array_index(p->folder_paths, fi) : "";
  ConnectionItem *folder = folder_by_path(mgr, path);
  if (!folder)
    folder = &mgr->root;
  ConnectionItem *it = !as_new && p->highlight_id ? connmgr_find_by_id(mgr, p->highlight_id) : NULL;
  char *id;
  if (it && it->type == CONN_ITEM_CONNECTION) {
    id = g_strdup(it->connection.id);
    LaceSavedConnection *dst = &it->connection;
    char *keep = dst->id;
    dst->id = NULL;
    connmgr_free_connection(dst);
    *dst = *c;
    free(dst->id);
    dst->id = keep;
    free(c);
    char *cur = folder_path_of(it);
    if (g_strcmp0(cur, path) != 0)
      connmgr_move_item(mgr, it, folder, NULL);
    g_free(cur);
  } else {
    free(c->id);
    c->id = lace_generate_uuid();
    id = g_strdup(c->id);
    if (!connmgr_add_connection(folder, c)) {
      lace_saved_conn_free(c);
      g_free(id);
      say(p, TRUE, "Cannot add the connection");
      return;
    }
  }
  /* A saved URL is no longer a recent one */
  char *url = conn_fields_connstr(&p->fields);
  char *clean = url ? recent_strip_passwords(url) : NULL;
  for (size_t i = 0; clean && i < p->recent.count; i++) {
    if (g_str_equal(p->recent.items[i].url, clean)) {
      recent_remove(&p->recent, i);
      recent_save(&p->recent, NULL);
      break;
    }
  }
  str_secure_free(url);
  str_secure_free(clean);
  if (lace_app_save_saved(p->app, parent_of(p))) {
    it = connmgr_find_by_id(mgr, id);
    const char *name = it ? it->connection.name : NULL;
    set_highlight(p, id, name);
    if (name && !p->fields.in[CF_NAME].len)
      text_input_set(&p->fields.in[CF_NAME], name);
    conn_fields_mark_clean(&p->fields);
    rebuild_list(p);
    char *m = g_strdup_printf("Saved %s", name ? name : "");
    say(p, FALSE, m);
    g_free(m);
    fields_to_rows(p);
  }
  g_free(id);
}

typedef struct {
  LaceFormPage *page;
  LaceSavedConnection *c;
  gboolean as_new;
} StoreCtx;

static void store_answered(gboolean yes, gpointer data) {
  StoreCtx *s = data;
  if (!yes) { /* saved without the passwords */
    s->c->save_password = s->c->save_ssh_password = false;
    str_secure_free(s->c->password);
    str_secure_free(s->c->ssh_password);
    s->c->password = str_dup("");
    s->c->ssh_password = str_dup("");
    s->page->fields.chk[CF_SAVEPW] = s->page->fields.chk[CF_SAVESSHPW] = false;
  }
  store(s->page, s->c, s->as_new);
  g_object_unref(s->page);
  g_free(s);
}

static gboolean holds(gboolean saved, const char *pw, const char *typed) {
  return saved && pw && typed && g_str_equal(pw, typed);
}

static void save(LaceFormPage *p, gboolean as_new) {
  if (!commit_uri(p))
    return;
  LaceSavedConnection *c = conn_fields_collect(&p->fields, false);
  if (!c) {
    show_message(p);
    return;
  }
  ConnectionItem *it = !as_new && p->highlight_id ? connmgr_find_by_id(lace_app_get_saved(p->app), p->highlight_id)
                                                  : NULL;
  const LaceSavedConnection *e = it && it->type == CONN_ITEM_CONNECTION ? &it->connection : NULL;
  gboolean new_pw = c->save_password && c->password && *c->password &&
                    !(e && holds(e->save_password, e->password, c->password));
  gboolean new_ssh = c->save_ssh_password && c->ssh_password && *c->ssh_password &&
                     !(e && holds(e->save_ssh_password, e->ssh_password, c->ssh_password));
  if (!new_pw && !new_ssh) {
    store(p, c, as_new);
    return;
  }
  StoreCtx *s = g_new0(StoreCtx, 1);
  s->page = g_object_ref(p);
  s->c = c;
  s->as_new = as_new;
  lace_confirm(GTK_WIDGET(p), "Save password", "Store the password in connections.json (readable only by you)?",
               "_Store", FALSE, store_answered, s);
}

void lace_form_page_action(LaceFormPage *p, const char *action) {
  if (g_str_equal(action, "open"))
    open_with(p, "open", 0);
  else if (g_str_equal(action, "background"))
    open_with(p, "background", 0);
  else if (g_str_equal(action, "new-window"))
    open_with(p, "new-window", 0);
  else if (g_str_has_prefix(action, "to-window:"))
    open_with(p, "to-window", (guint)atoi(action + strlen("to-window:")));
  else if (g_str_equal(action, "save"))
    save(p, FALSE);
  else if (g_str_equal(action, "save-as-new"))
    save(p, TRUE);
  else if (g_str_equal(action, "test"))
    test(p);
  else if (g_str_equal(action, "clear"))
    clear(p);
}

/* Enter in the URI row or a field: Open here */
static void on_entry_activated(AdwEntryRow *row, LaceFormPage *p) {
  (void)row;
  lace_form_page_action(p, "open");
}

/* The "form.*" actions of the page's buttons, keys and menus */
static void act(GSimpleAction *a, GVariant *v, gpointer data) {
  LaceFormPage *p = data;
  const char *name = g_action_get_name(G_ACTION(a));
  if (g_str_equal(name, "to-window") && v) {
    char *t = g_strdup_printf("to-window:%d", g_variant_get_int32(v));
    lace_form_page_action(p, t);
    g_free(t);
  } else {
    lace_form_page_action(p, name);
  }
}

/* "To window ▾": the other windows, rebuilt when the menu opens */
static void on_to_window_activate(GtkMenuButton *b, LaceFormPage *p) {
  (void)b;
  GMenu *menu = g_menu_new();
  GList *wins = lace_app_get_lace_windows(p->app);
  for (GList *l = wins; l; l = l->next) {
    if (l->data == (gpointer)window_of(p))
      continue;
    char *label = g_strdup_printf("%u  %s", lace_window_get_number(l->data), lace_window_get_name(l->data));
    GMenuItem *item = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target(item, "form.to-window", "i", (int)lace_window_get_number(l->data));
    g_menu_append_item(menu, item);
    g_object_unref(item);
    g_free(label);
  }
  if (!wins || !wins->next)
    g_menu_append(menu, "No other window", "form.none");
  g_list_free(wins);
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(p->to_window_button), G_MENU_MODEL(menu));
  g_object_unref(menu);
}

/* ==========================================================================
 * The tree's own actions (a row's context menu)
 * ========================================================================== */

/* Aim the context menu at row (NULL: none) */
static void set_menu_target(LaceFormPage *p, GtkListBoxRow *row) {
  g_clear_pointer(&p->menu_id, g_free);
  g_clear_pointer(&p->menu_path, g_free);
  p->menu_kind = row ? GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind")) : 0;
  if (row) {
    p->menu_id = g_strdup(g_object_get_data(G_OBJECT(row), "id"));
    p->menu_path = g_strdup(g_object_get_data(G_OBJECT(row), "path"));
    p->menu_index = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "index"));
  }
}

static ConnectionItem *menu_item(LaceFormPage *p) {
  ConnectionManager *mgr = lace_app_get_saved(p->app);
  if (p->menu_kind == ROW_CONN && p->menu_id)
    return connmgr_find_by_id(mgr, p->menu_id);
  if (p->menu_kind == ROW_FOLDER && p->menu_path)
    return folder_by_path(mgr, p->menu_path);
  return NULL;
}

static void renamed(const char *text, gpointer data) {
  LaceFormPage *p = data;
  ConnectionItem *it = text && *text ? menu_item(p) : NULL;
  if (it) {
    char **name = it->type == CONN_ITEM_FOLDER ? &it->folder.name : &it->connection.name;
    free(*name);
    *name = str_dup(text);
    if (it->type == CONN_ITEM_CONNECTION && p->highlight_id && g_str_equal(p->highlight_id, it->connection.id)) {
      set_label(p, text);
      if (!form_dirty(p)) {
        text_input_set(&p->fields.in[CF_NAME], text);
        conn_fields_mark_clean(&p->fields);
        fields_to_rows(p);
      }
    }
    lace_app_save_saved(p->app, parent_of(p));
  }
  g_object_unref(p);
}

static void deleted(gboolean yes, gpointer data) {
  LaceFormPage *p = data;
  ConnectionItem *it = yes ? menu_item(p) : NULL;
  if (it) {
    gboolean shown = it->type == CONN_ITEM_CONNECTION && p->highlight_id &&
                     g_str_equal(p->highlight_id, it->connection.id);
    connmgr_remove_item(lace_app_get_saved(p->app), it);
    if (shown) {
      set_highlight(p, NULL, NULL);
      if (!form_dirty(p))
        clear(p);
    }
    lace_app_save_saved(p->app, parent_of(p));
  }
  g_object_unref(p);
}

static void folder_named(const char *text, gpointer data) {
  LaceFormPage *p = data;
  if (text && *text) {
    ConnectionManager *mgr = lace_app_get_saved(p->app);
    ConnectionItem *at = menu_item(p);
    ConnectionItem *parent = at && at->type == CONN_ITEM_FOLDER ? at : &mgr->root;
    connmgr_add_folder(parent, connmgr_new_folder(text));
    lace_app_save_saved(p->app, parent_of(p));
  }
  g_object_unref(p);
}

static void tree_act(GSimpleAction *a, GVariant *v, gpointer data) {
  (void)v;
  LaceFormPage *p = data;
  const char *name = g_action_get_name(G_ACTION(a));
  ConnectionItem *it = menu_item(p);
  ConnectionManager *mgr = lace_app_get_saved(p->app);
  if (g_str_equal(name, "new-folder") || g_str_equal(name, "new-folder-here")) {
    if (g_str_equal(name, "new-folder"))
      set_menu_target(p, NULL); /* the button: at the top level */
    lace_prompt(GTK_WIDGET(p), "New folder", "A folder groups saved connections", NULL, "_Create", FALSE,
                folder_named, g_object_ref(p));
  } else if (g_str_equal(name, "rename") && it) {
    lace_prompt(GTK_WIDGET(p), "Rename", NULL, connmgr_item_name(it), "_Rename", FALSE, renamed, g_object_ref(p));
  } else if (g_str_equal(name, "duplicate") && it && it->type == CONN_ITEM_CONNECTION) {
    LaceSavedConnection *copy = lace_saved_conn_copy(&it->connection);
    free(copy->id);
    copy->id = lace_generate_uuid();
    char *n = g_strdup_printf("%s (copy)", it->connection.name ? it->connection.name : "");
    free(copy->name);
    copy->name = str_dup(n);
    g_free(n);
    if (!connmgr_add_connection(it->parent ? it->parent : &mgr->root, copy))
      lace_saved_conn_free(copy);
    lace_app_save_saved(p->app, parent_of(p));
  } else if (g_str_equal(name, "delete") && it) {
    char *body = it->type == CONN_ITEM_FOLDER
                     ? g_strdup_printf("Delete the folder \"%s\" and everything in it?", connmgr_item_name(it))
                     : g_strdup_printf("Delete the saved connection \"%s\"?", connmgr_item_name(it));
    lace_confirm(GTK_WIDGET(p), "Delete", body, "_Delete", TRUE, deleted, g_object_ref(p));
    g_free(body);
  } else if (g_str_equal(name, "forget") && p->menu_kind == ROW_RECENT) {
    gsize i = p->menu_index;
    if (i < p->recent.count) {
      recent_remove(&p->recent, i);
      char *err = NULL;
      if (!recent_save(&p->recent, &err))
        say(p, TRUE, err ? err : "recent.json could not be written");
      free(err);
      rebuild_list(p);
    }
  }
}

static void on_list_pressed(GtkGestureClick *g, int n, double x, double y, LaceFormPage *p) {
  (void)n;
  GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(p->list), (int)y);
  RowKind kind = row ? GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind")) : 0;
  set_menu_target(p, row);
  GMenu *menu = g_menu_new();
  if (kind == ROW_CONN || kind == ROW_FOLDER) {
    g_menu_append(menu, "Rename\xe2\x80\xa6", "tree.rename");
    if (kind == ROW_CONN)
      g_menu_append(menu, "Duplicate", "tree.duplicate");
    g_menu_append(menu, "Delete\xe2\x80\xa6", "tree.delete");
  } else if (kind == ROW_RECENT) {
    g_menu_append(menu, "Forget", "tree.forget");
  }
  g_menu_append(menu, kind == ROW_FOLDER ? "New Folder Here\xe2\x80\xa6" : "New Folder\xe2\x80\xa6",
                "tree.new-folder-here");
  GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  gtk_widget_set_parent(pop, p->list);
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &(GdkRectangle){(int)x, (int)y, 1, 1});
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
  g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
  gtk_popover_popup(GTK_POPOVER(pop));
  g_object_unref(menu);
  gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* ==========================================================================
 * Building
 * ========================================================================== */

static GtkWidget *text_row(LaceFormPage *p, int field, const char *title, gboolean password) {
  GtkWidget *row = password ? adw_password_entry_row_new() : adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  g_object_set_data(G_OBJECT(row), "field", GINT_TO_POINTER(field));
  g_signal_connect(row, "changed", G_CALLBACK(on_text_changed), p);
  p->row[field] = row;
  return row;
}

static GtkWidget *switch_row(LaceFormPage *p, int field, const char *title) {
  GtkWidget *row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  g_object_set_data(G_OBJECT(row), "field", GINT_TO_POINTER(field));
  g_signal_connect(row, "notify::active", G_CALLBACK(on_switch_changed), p);
  p->row[field] = row;
  return row;
}

static GtkWidget *button(const char *label, const char *action, gboolean suggested) {
  GtkWidget *b = gtk_button_new_with_mnemonic(label);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
  if (suggested)
    gtk_widget_add_css_class(b, "suggested-action");
  return b;
}

static GtkWidget *build_form(LaceFormPage *p) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);
  gtk_widget_set_margin_top(box, 18);
  gtk_widget_set_margin_bottom(box, 18);

  GtkWidget *g0 = adw_preferences_group_new();
  p->uri_row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->uri_row), "URI, or a SQLite file");
  g_signal_connect(p->uri_row, "changed", G_CALLBACK(on_uri_changed), p);
  g_signal_connect(p->uri_row, "entry-activated", G_CALLBACK(on_entry_activated), p);
  GtkEventController *focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "leave", G_CALLBACK(on_uri_leave), p);
  gtk_widget_add_controller(p->uri_row, focus);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g0), p->uri_row);
  gtk_box_append(GTK_BOX(box), g0);

  GtkWidget *g1 = adw_preferences_group_new();
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), text_row(p, CF_NAME, "Name", FALSE));
  p->row[CF_DRIVER] = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->row[CF_DRIVER]), "Database");
  driver_model(p);
  g_signal_connect(p->row[CF_DRIVER], "notify::selected", G_CALLBACK(on_driver_changed), p);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), p->row[CF_DRIVER]);
  p->folder_row = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->folder_row), "Folder");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), p->folder_row);
  gtk_box_append(GTK_BOX(box), g1);

  GtkWidget *g2 = adw_preferences_group_new();
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), text_row(p, CF_FILE, "Database file", FALSE));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), text_row(p, CF_HOST, "Host", FALSE));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), text_row(p, CF_PORT, "Port", FALSE));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), text_row(p, CF_DB, "Database name", FALSE));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), text_row(p, CF_USER, "User", FALSE));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), text_row(p, CF_PASS, "Password", TRUE));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), switch_row(p, CF_SAVEPW, "Save password"));
  gtk_box_append(GTK_BOX(box), g2);

  GtkWidget *g3 = adw_preferences_group_new();
  GtkWidget *ssh = adw_expander_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(ssh), "SSH tunnel");
  adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(ssh), TRUE);
  g_object_set_data(G_OBJECT(ssh), "field", GINT_TO_POINTER(CF_SSH));
  g_signal_connect(ssh, "notify::enable-expansion", G_CALLBACK(on_switch_changed), p);
  p->row[CF_SSH] = ssh;
  adw_expander_row_add_row(ADW_EXPANDER_ROW(ssh), text_row(p, CF_SSHHOST, "SSH host", FALSE));
  adw_expander_row_add_row(ADW_EXPANDER_ROW(ssh), text_row(p, CF_SSHPORT, "SSH port", FALSE));
  adw_expander_row_add_row(ADW_EXPANDER_ROW(ssh), text_row(p, CF_SSHUSER, "SSH user", FALSE));
  adw_expander_row_add_row(ADW_EXPANDER_ROW(ssh), text_row(p, CF_SSHKEY, "SSH key", FALSE));
  adw_expander_row_add_row(ADW_EXPANDER_ROW(ssh), text_row(p, CF_SSHPASS, "SSH password", TRUE));
  adw_expander_row_add_row(ADW_EXPANDER_ROW(ssh), switch_row(p, CF_SAVESSHPW, "Save SSH password"));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), ssh);
  gtk_box_append(GTK_BOX(box), g3);

  GtkWidget *open_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  p->open_button = button("_Open here", "form.open", TRUE);
  gtk_box_append(GTK_BOX(open_box), p->open_button);
  gtk_box_append(GTK_BOX(open_box), button("_Background tab", "form.background", FALSE));
  gtk_box_append(GTK_BOX(open_box), button("New _window", "form.new-window", FALSE));
  p->to_window_button = gtk_menu_button_new();
  gtk_menu_button_set_label(GTK_MENU_BUTTON(p->to_window_button), "To window");
  g_signal_connect(p->to_window_button, "activate", G_CALLBACK(on_to_window_activate), p);
  GtkGesture *pre = gtk_gesture_click_new();
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(pre), GTK_PHASE_CAPTURE);
  g_signal_connect_swapped(pre, "pressed", G_CALLBACK(on_to_window_activate), p->to_window_button);
  gtk_widget_add_controller(p->to_window_button, GTK_EVENT_CONTROLLER(pre));
  gtk_box_append(GTK_BOX(open_box), p->to_window_button);
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(bar, 18);
  gtk_widget_set_margin_end(bar, 18);
  gtk_widget_set_margin_top(bar, 8);
  gtk_widget_set_margin_bottom(bar, 10);
  gtk_box_append(GTK_BOX(bar), open_box);

  GtkWidget *manage = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(manage), button("_Save", "form.save", FALSE));
  gtk_box_append(GTK_BOX(manage), button("Save as _new", "form.save-as-new", FALSE));
  gtk_box_append(GTK_BOX(manage), button("_Test", "form.test", FALSE));
  gtk_box_append(GTK_BOX(manage), button("_Clear", "form.clear", FALSE));
  gtk_box_append(GTK_BOX(bar), manage);

  p->message = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(p->message), 0);
  gtk_label_set_wrap(GTK_LABEL(p->message), TRUE);
  gtk_label_set_selectable(GTK_LABEL(p->message), TRUE);
  gtk_widget_set_visible(p->message, FALSE);
  gtk_box_append(GTK_BOX(bar), p->message);

  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 720);
  adw_clamp_set_child(ADW_CLAMP(clamp), box);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  gtk_widget_set_vexpand(scroll, TRUE);
  /* The buttons and the message stay in view under the fields */
  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(outer), scroll);
  gtk_box_append(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  GtkWidget *bclamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(bclamp), 720);
  adw_clamp_set_child(ADW_CLAMP(bclamp), bar);
  gtk_box_append(GTK_BOX(outer), bclamp);
  return outer;
}

static GtkWidget *build_panel(LaceFormPage *p) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_size_request(box, 260, -1);
  p->search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(p->search), "Search saved connections");
  gtk_widget_set_margin_start(p->search, 6);
  gtk_widget_set_margin_end(p->search, 6);
  gtk_widget_set_margin_top(p->search, 6);
  g_signal_connect(p->search, "search-changed", G_CALLBACK(on_search_changed), p);
  gtk_box_append(GTK_BOX(box), p->search);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  p->list = gtk_list_box_new();
  gtk_widget_add_css_class(p->list, "navigation-sidebar");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->list), GTK_SELECTION_SINGLE);
  g_signal_connect(p->list, "row-selected", G_CALLBACK(on_row_selected), p);
  g_signal_connect(p->list, "row-activated", G_CALLBACK(on_row_activated), p);
  GtkGesture *menu = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu), GDK_BUTTON_SECONDARY);
  g_signal_connect(menu, "pressed", G_CALLBACK(on_list_pressed), p);
  gtk_widget_add_controller(p->list, GTK_EVENT_CONTROLLER(menu));
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), p->list);
  gtk_box_append(GTK_BOX(box), scroll);
  GtkWidget *nf = gtk_button_new_from_icon_name("folder-new-symbolic");
  gtk_widget_set_tooltip_text(nf, "New folder");
  gtk_widget_add_css_class(nf, "flat");
  gtk_widget_set_halign(nf, GTK_ALIGN_START);
  gtk_widget_set_margin_start(nf, 6);
  gtk_widget_set_margin_bottom(nf, 6);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(nf), "tree.new-folder");
  gtk_box_append(GTK_BOX(box), nf);
  return box;
}

LaceFormPage *lace_form_page_new(LaceWindow *w) {
  LaceFormPage *p = g_object_new(LACE_TYPE_FORM_PAGE, NULL);
  (void)w; /* the page finds its window through the widget tree */
  p->app = lace_app_get_default();
  p->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_start_child(GTK_PANED(p->paned), build_panel(p));
  gtk_paned_set_end_child(GTK_PANED(p->paned), build_form(p));
  gtk_paned_set_resize_start_child(GTK_PANED(p->paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(p->paned), FALSE);
  gtk_paned_set_position(GTK_PANED(p->paned), 300);
  gtk_widget_set_parent(p->paned, GTK_WIDGET(p));

  static const GActionEntry entries[] = {
      {"open", act, NULL, NULL, NULL, {0}},         {"background", act, NULL, NULL, NULL, {0}},
      {"new-window", act, NULL, NULL, NULL, {0}},   {"to-window", act, "i", NULL, NULL, {0}},
      {"save", act, NULL, NULL, NULL, {0}},         {"save-as-new", act, NULL, NULL, NULL, {0}},
      {"test", act, NULL, NULL, NULL, {0}},         {"clear", act, NULL, NULL, NULL, {0}},
  };
  GSimpleActionGroup *group = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(group), entries, G_N_ELEMENTS(entries), p);
  GSimpleAction *none = g_simple_action_new("none", NULL);
  g_simple_action_set_enabled(none, FALSE);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(none));
  g_object_unref(none);
  gtk_widget_insert_action_group(GTK_WIDGET(p), "form", G_ACTION_GROUP(group));
  g_object_unref(group);
  static const GActionEntry tree[] = {
      {"new-folder", tree_act, NULL, NULL, NULL, {0}}, {"new-folder-here", tree_act, NULL, NULL, NULL, {0}},
      {"rename", tree_act, NULL, NULL, NULL, {0}},
      {"duplicate", tree_act, NULL, NULL, NULL, {0}},  {"delete", tree_act, NULL, NULL, NULL, {0}},
      {"forget", tree_act, NULL, NULL, NULL, {0}},
  };
  GSimpleActionGroup *tg = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(tg), tree, G_N_ELEMENTS(tree), p);
  gtk_widget_insert_action_group(GTK_WIDGET(p), "tree", G_ACTION_GROUP(tg));
  g_object_unref(tg);

  /* Keys of this page: Ctrl+S saves, Ctrl+B opens in a background tab,
   * Enter in a field opens here */
  GtkEventController *keys = gtk_shortcut_controller_new();
  gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(keys), GTK_SHORTCUT_SCOPE_LOCAL);
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
      gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>s"), gtk_named_action_new("form.save")));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
      gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>b"), gtk_named_action_new("form.background")));
  gtk_widget_add_controller(GTK_WIDGET(p), keys);
  for (int f = 0; f < CF_COUNT; f++)
    if (p->row[f] && is_text(f))
      g_signal_connect(p->row[f], "entry-activated", G_CALLBACK(on_entry_activated), p);

  g_signal_connect_object(p->app, "saved-changed", G_CALLBACK(on_saved_changed), p, 0);
  refresh_folders(p);
  rebuild_list(p);
  fields_to_rows(p);
  return p;
}

const char *lace_form_page_get_label(LaceFormPage *p) { return p->label; }
const char *lace_form_page_get_uri(LaceFormPage *p) { return gtk_editable_get_text(GTK_EDITABLE(p->uri_row)); }
const char *lace_form_page_get_message(LaceFormPage *p) { return p->fields.message; }

void lace_form_page_set_uri(LaceFormPage *p, const char *text) {
  gtk_editable_set_text(GTK_EDITABLE(p->uri_row), text ? text : "");
  p->uri_edited = TRUE;
  commit_uri(p);
}

gboolean lace_form_page_pick(LaceFormPage *p, const char *name) {
  for (GtkWidget *c = gtk_widget_get_first_child(p->list); c; c = gtk_widget_get_next_sibling(c)) {
    const char *title = g_object_get_data(G_OBJECT(c), "title");
    if (title && g_str_equal(title, name)) {
      gtk_list_box_select_row(GTK_LIST_BOX(p->list), GTK_LIST_BOX_ROW(c));
      return TRUE;
    }
  }
  return FALSE;
}

gboolean lace_form_page_menu_at(LaceFormPage *p, const char *name) {
  for (GtkWidget *c = gtk_widget_get_first_child(p->list); c; c = gtk_widget_get_next_sibling(c)) {
    const char *title = g_object_get_data(G_OBJECT(c), "title");
    if (title && g_str_equal(title, name)) {
      set_menu_target(p, GTK_LIST_BOX_ROW(c));
      return TRUE;
    }
  }
  return FALSE;
}

void lace_form_page_search(LaceFormPage *p, const char *text) {
  gtk_editable_set_text(GTK_EDITABLE(p->search), text ? text : "");
  rebuild_list(p); /* at once, not after the search entry's delay */
}

void lace_form_page_tree_action(LaceFormPage *p, const char *action) {
  char *name = g_strdup_printf("tree.%s", action);
  gtk_widget_activate_action(GTK_WIDGET(p), name, NULL);
  g_free(name);
}

static const struct {
  const char *key;
  int field;
} g_keys[] = {{"name", CF_NAME},         {"driver", CF_DRIVER},       {"file", CF_FILE},
              {"host", CF_HOST},         {"port", CF_PORT},           {"database", CF_DB},
              {"user", CF_USER},         {"password", CF_PASS},       {"save-password", CF_SAVEPW},
              {"ssh", CF_SSH},           {"ssh-host", CF_SSHHOST},    {"ssh-port", CF_SSHPORT},
              {"ssh-user", CF_SSHUSER},  {"ssh-key", CF_SSHKEY},      {"ssh-password", CF_SSHPASS},
              {"save-ssh-password", CF_SAVESSHPW}};

static int field_of(const char *key) {
  for (gsize i = 0; i < G_N_ELEMENTS(g_keys); i++)
    if (g_str_equal(g_keys[i].key, key))
      return g_keys[i].field;
  return -1;
}

gboolean lace_form_page_set_field(LaceFormPage *p, const char *key, const char *value) {
  int f = field_of(key);
  if (f < 0)
    return FALSE;
  value = value ? value : "";
  if (f == CF_DRIVER) {
    for (int i = 0; i < conn_fields_num_drivers(&p->fields); i++)
      if (g_str_equal(conn_fields_driver_name(&p->fields, i), value))
        adw_combo_row_set_selected(ADW_COMBO_ROW(p->row[CF_DRIVER]), (guint)i);
  } else if (f == CF_SSH) {
    adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(p->row[f]), g_str_equal(value, "true"));
  } else if (conn_fields_is_check(f)) {
    adw_switch_row_set_active(ADW_SWITCH_ROW(p->row[f]), g_str_equal(value, "true"));
  } else {
    gtk_editable_set_text(GTK_EDITABLE(p->row[f]), value);
  }
  return TRUE;
}

char *lace_form_page_get_field(LaceFormPage *p, const char *key) {
  int f = field_of(key);
  if (f < 0)
    return NULL;
  if (f == CF_DRIVER)
    return g_strdup(conn_fields_driver_name(&p->fields, p->fields.driver));
  if (conn_fields_is_check(f))
    return g_strdup(p->fields.chk[f] ? "true" : "false");
  return g_strdup(p->fields.in[f].buf ? p->fields.in[f].buf : "");
}

/* ==========================================================================
 * GObject
 * ========================================================================== */

static void lace_form_page_dispose(GObject *obj) {
  LaceFormPage *p = LACE_FORM_PAGE(obj);
  g_clear_pointer(&p->paned, gtk_widget_unparent);
  G_OBJECT_CLASS(lace_form_page_parent_class)->dispose(obj);
}

static void lace_form_page_finalize(GObject *obj) {
  LaceFormPage *p = LACE_FORM_PAGE(obj);
  conn_fields_free(&p->fields);
  recent_free(&p->recent);
  g_ptr_array_unref(p->folder_paths);
  g_free(p->highlight_id);
  g_free(p->label);
  g_free(p->menu_id);
  g_free(p->menu_path);
  G_OBJECT_CLASS(lace_form_page_parent_class)->finalize(obj);
}

static void lace_form_page_class_init(LaceFormPageClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = lace_form_page_dispose;
  G_OBJECT_CLASS(klass)->finalize = lace_form_page_finalize;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
  signals[SIG_LABEL_CHANGED] = g_signal_new("label-changed", LACE_TYPE_FORM_PAGE, G_SIGNAL_RUN_LAST, 0, NULL,
                                            NULL, NULL, G_TYPE_NONE, 0);
}

static void lace_form_page_init(LaceFormPage *p) {
  conn_fields_init(&p->fields);
  p->folder_paths = g_ptr_array_new_with_free_func(g_free);
  p->label = g_strdup("New connection");
}
