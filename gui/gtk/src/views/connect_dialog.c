/*
 * Lace GTK frontend
 * Connect dialog
 *
 * Left: the saved connections (folders as section headers, searchable).
 * Right: the connection form. Top: a URL or file to connect to directly.
 * Every change to the saved list is written at once.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connect_dialog.h"
#include "util.h"
#include <config/saved_connection.h>
#include <util/connstr.h>
#include <util/str.h>
#include <stdlib.h>
#include <string.h>

enum { DRV_SQLITE, DRV_POSTGRES, DRV_MYSQL, DRV_MARIADB };
static const char *const g_driver_keys[] = {"sqlite", "postgres", "mysql", "mariadb"};
static const char *const g_driver_names[] = {"SQLite", "PostgreSQL", "MySQL", "MariaDB", NULL};

typedef struct {
  LaceWindow *window;
  LaceApp *app;
  AdwDialog *dialog;
  GtkWidget *banner;
  GtkWidget *search;
  GtkWidget *list;
  GtkWidget *url_entry;
  GtkWidget *form_title;
  GtkWidget *name_row, *driver_row, *folder_row;
  GtkWidget *file_row, *host_row, *port_row, *db_row, *user_row, *pass_row, *save_pass_row;
  GtkWidget *ssh_expander, *ssh_host_row, *ssh_port_row, *ssh_user_row,
      *ssh_pass_row, *ssh_save_row;
  GtkWidget *test_button, *save_button, *connect_button, *delete_button, *dup_button;
  GPtrArray *folder_paths; /* char*, "" is the root */
  char *editing_id;
  gboolean filling;
  gboolean busy;
} ConnectDialog;

static void rebuild_list(ConnectDialog *cd);
static void fill_form(ConnectDialog *cd, const LaceSavedConnection *sc);

/* ==========================================================================
 * Folders
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
  for (int i = 0; parts[i]; i++) {
    ConnectionItem *next = NULL;
    for (size_t k = 0; k < cur->folder.num_children; k++) {
      ConnectionItem *it = &cur->folder.children[k];
      if (it->type == CONN_ITEM_FOLDER && g_strcmp0(it->folder.name, parts[i]) == 0) {
        next = it;
        break;
      }
    }
    if (!next) {
      cur = NULL;
      break;
    }
    cur = next;
  }
  g_strfreev(parts);
  return cur;
}

static char *folder_path_of(ConnectionItem *item) {
  GString *s = g_string_new(NULL);
  ConnectionItem *p = item ? item->parent : NULL;
  GPtrArray *names = g_ptr_array_new();
  while (p && p->parent) { /* The root has no parent */
    g_ptr_array_add(names, p->folder.name);
    p = p->parent;
  }
  for (guint i = names->len; i > 0; i--) {
    if (s->len)
      g_string_append_c(s, '/');
    g_string_append(s, g_ptr_array_index(names, i - 1));
  }
  g_ptr_array_unref(names);
  return g_string_free(s, FALSE);
}

static void refresh_folder_combo(ConnectDialog *cd) {
  g_ptr_array_set_size(cd->folder_paths, 0);
  g_ptr_array_add(cd->folder_paths, g_strdup(""));
  collect_folders(&lace_app_get_saved(cd->app)->root, "", cd->folder_paths);
  GtkStringList *list = gtk_string_list_new(NULL);
  for (guint i = 0; i < cd->folder_paths->len; i++) {
    const char *p = g_ptr_array_index(cd->folder_paths, i);
    gtk_string_list_append(list, *p ? p : "(top level)");
  }
  adw_combo_row_set_model(ADW_COMBO_ROW(cd->folder_row), G_LIST_MODEL(list));
  g_object_unref(list);
}

/* ==========================================================================
 * Saved list
 * ========================================================================== */

static gboolean matches(const LaceSavedConnection *c, const char *needle) {
  if (!needle || !*needle)
    return TRUE;
  char *n = g_utf8_casefold(needle, -1);
  gboolean hit = FALSE;
  const char *fields[] = {c->name, c->host, c->database, c->user, c->driver, c->ssh_host};
  for (gsize i = 0; i < G_N_ELEMENTS(fields) && !hit; i++) {
    if (!fields[i])
      continue;
    char *f = g_utf8_casefold(fields[i], -1);
    hit = strstr(f, n) != NULL;
    g_free(f);
  }
  g_free(n);
  return hit;
}

static void on_folder_rename_cb(const char *text, gpointer data) {
  ConnectDialog *cd = g_object_get_data(G_OBJECT(data), "cd");
  const char *path = g_object_get_data(G_OBJECT(data), "path");
  if (text && *text) {
    ConnectionItem *f = folder_by_path(lace_app_get_saved(cd->app), path);
    if (f && f != &lace_app_get_saved(cd->app)->root) {
      free(f->folder.name);
      f->folder.name = str_dup(text);
      lace_app_save_saved(cd->app, GTK_WIDGET(cd->dialog));
      rebuild_list(cd);
      refresh_folder_combo(cd);
    }
  }
  g_object_unref(data);
}

static void on_folder_rename(GtkButton *b, ConnectDialog *cd) {
  const char *path = g_object_get_data(G_OBJECT(b), "path");
  GObject *holder = g_object_new(G_TYPE_OBJECT, NULL);
  g_object_set_data(holder, "cd", cd);
  g_object_set_data_full(holder, "path", g_strdup(path), g_free);
  const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
  lace_prompt(GTK_WIDGET(cd->dialog), "Rename folder", NULL, name, "_Rename", FALSE,
              on_folder_rename_cb, holder);
}

static void on_folder_delete(GtkButton *b, ConnectDialog *cd) {
  const char *path = g_object_get_data(G_OBJECT(b), "path");
  ConnectionManager *mgr = lace_app_get_saved(cd->app);
  ConnectionItem *f = folder_by_path(mgr, path);
  if (!f || f == &mgr->root)
    return;
  if (f->folder.num_children > 0) {
    lace_toast(GTK_WIDGET(cd->dialog), "Move or delete the folder's connections first");
    return;
  }
  connmgr_remove_item(mgr, f);
  lace_app_save_saved(cd->app, GTK_WIDGET(cd->dialog));
  rebuild_list(cd);
  refresh_folder_combo(cd);
}

static void add_folder_header(ConnectDialog *cd, const char *path, const char *name, int depth) {
  GtkWidget *row = gtk_list_box_row_new();
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_widget_add_css_class(row, "folder-row");
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(box, depth * 12);
  GtkWidget *icon = gtk_image_new_from_icon_name("folder-symbolic");
  GtkWidget *label = gtk_label_new(name);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(label, TRUE);
  GtkWidget *rename = gtk_button_new_from_icon_name("document-edit-symbolic");
  gtk_widget_add_css_class(rename, "flat");
  gtk_widget_set_tooltip_text(rename, "Rename folder");
  g_object_set_data_full(G_OBJECT(rename), "path", g_strdup(path), g_free);
  g_signal_connect(rename, "clicked", G_CALLBACK(on_folder_rename), cd);
  GtkWidget *del = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_add_css_class(del, "flat");
  gtk_widget_set_tooltip_text(del, "Delete the empty folder");
  g_object_set_data_full(G_OBJECT(del), "path", g_strdup(path), g_free);
  g_signal_connect(del, "clicked", G_CALLBACK(on_folder_delete), cd);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), label);
  gtk_box_append(GTK_BOX(box), rename);
  gtk_box_append(GTK_BOX(box), del);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_list_box_append(GTK_LIST_BOX(cd->list), row);
}

static const char *driver_icon(const char *driver) {
  if (g_strcmp0(driver, "sqlite") == 0)
    return "lace-sqlite-symbolic";
  if (g_strcmp0(driver, "postgres") == 0 || g_strcmp0(driver, "postgresql") == 0)
    return "lace-postgres-symbolic";
  return "lace-mysql-symbolic";
}

static void add_connection_row(ConnectDialog *cd, const LaceSavedConnection *c, int depth) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), c->name && *c->name ? c->name : "Connection");
  char *sub;
  if (g_strcmp0(c->driver, "sqlite") == 0)
    sub = g_strdup(c->database ? c->database : "");
  else
    sub = g_strdup_printf("%s%s%s/%s%s", c->user && *c->user ? c->user : "", c->user && *c->user ? "@" : "",
                          c->host ? c->host : "", c->database ? c->database : "",
                          c->ssh ? " · ssh" : "");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
  g_free(sub);
  GtkWidget *icon = gtk_image_new_from_icon_name(driver_icon(c->driver));
  gtk_widget_set_margin_start(icon, depth * 12);
  adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
  g_object_set_data_full(G_OBJECT(row), "id", g_strdup(c->id), g_free);
  gtk_list_box_append(GTK_LIST_BOX(cd->list), row);
}

static void walk_tree(ConnectDialog *cd, ConnectionItem *folder, const char *prefix, int depth,
                      const char *needle, int *count) {
  for (size_t i = 0; folder && i < folder->folder.num_children; i++) {
    ConnectionItem *it = &folder->folder.children[i];
    if (it->type == CONN_ITEM_FOLDER) {
      char *path = *prefix ? g_strdup_printf("%s/%s", prefix, it->folder.name) : g_strdup(it->folder.name);
      if (!needle || !*needle)
        add_folder_header(cd, path, it->folder.name, depth);
      walk_tree(cd, it, path, depth + 1, needle, count);
      g_free(path);
    } else if (matches(&it->connection, needle)) {
      add_connection_row(cd, &it->connection, (needle && *needle) ? 0 : depth);
      (*count)++;
    }
  }
}

static void rebuild_list(ConnectDialog *cd) {
  gtk_list_box_remove_all(GTK_LIST_BOX(cd->list));
  const char *needle = gtk_editable_get_text(GTK_EDITABLE(cd->search));
  int count = 0;
  walk_tree(cd, &lace_app_get_saved(cd->app)->root, "", 0, needle, &count);
  /* Keep the edited item selected */
  if (cd->editing_id) {
    for (GtkWidget *row = gtk_widget_get_first_child(cd->list); row; row = gtk_widget_get_next_sibling(row)) {
      const char *id = g_object_get_data(G_OBJECT(row), "id");
      if (id && g_strcmp0(id, cd->editing_id) == 0) {
        gtk_list_box_select_row(GTK_LIST_BOX(cd->list), GTK_LIST_BOX_ROW(row));
        break;
      }
    }
  }
}

/* ==========================================================================
 * Form
 * ========================================================================== */

static int driver_index(const char *driver) {
  const char *n = driver ? lace_driver_normalize(driver) : "sqlite";
  for (int i = 0; i < 4; i++)
    if (g_strcmp0(n, g_driver_keys[i]) == 0)
      return i;
  return DRV_SQLITE;
}

static void sync_driver_fields(ConnectDialog *cd) {
  guint d = adw_combo_row_get_selected(ADW_COMBO_ROW(cd->driver_row));
  gboolean sqlite = d == DRV_SQLITE;
  gtk_widget_set_visible(cd->file_row, sqlite);
  GtkWidget *server[] = {cd->host_row, cd->port_row, cd->db_row, cd->user_row, cd->pass_row,
                         cd->save_pass_row, cd->ssh_expander};
  for (gsize i = 0; i < G_N_ELEMENTS(server); i++)
    gtk_widget_set_visible(server[i], !sqlite);
  if (!cd->filling && !sqlite) {
    double port = adw_spin_row_get_value(ADW_SPIN_ROW(cd->port_row));
    int def = lace_driver_default_port(g_driver_keys[d]);
    if (port <= 1 || port == 5432 || port == 3306)
      adw_spin_row_set_value(ADW_SPIN_ROW(cd->port_row), def > 0 ? def : 5432);
  }
}

static void on_driver_changed(GObject *o, GParamSpec *p, ConnectDialog *cd) {
  (void)o; (void)p;
  sync_driver_fields(cd);
}

static const char *row_text(GtkWidget *row) {
  return gtk_editable_get_text(GTK_EDITABLE(row));
}

static void set_row_text(GtkWidget *row, const char *text) {
  gtk_editable_set_text(GTK_EDITABLE(row), text ? text : "");
}

static void clear_form(ConnectDialog *cd) {
  cd->filling = TRUE;
  g_clear_pointer(&cd->editing_id, g_free);
  gtk_label_set_text(GTK_LABEL(cd->form_title), "New connection");
  set_row_text(cd->name_row, "");
  adw_combo_row_set_selected(ADW_COMBO_ROW(cd->driver_row), DRV_SQLITE);
  adw_combo_row_set_selected(ADW_COMBO_ROW(cd->folder_row), 0);
  set_row_text(cd->file_row, "");
  set_row_text(cd->host_row, "localhost");
  adw_spin_row_set_value(ADW_SPIN_ROW(cd->port_row), 5432);
  set_row_text(cd->db_row, "");
  set_row_text(cd->user_row, "");
  set_row_text(cd->pass_row, "");
  adw_switch_row_set_active(ADW_SWITCH_ROW(cd->save_pass_row), FALSE);
  adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(cd->ssh_expander), FALSE);
  adw_expander_row_set_expanded(ADW_EXPANDER_ROW(cd->ssh_expander), FALSE);
  set_row_text(cd->ssh_host_row, "");
  adw_spin_row_set_value(ADW_SPIN_ROW(cd->ssh_port_row), 22);
  set_row_text(cd->ssh_user_row, "");
  set_row_text(cd->ssh_pass_row, "");
  adw_switch_row_set_active(ADW_SWITCH_ROW(cd->ssh_save_row), FALSE);
  gtk_widget_set_visible(cd->delete_button, FALSE);
  gtk_widget_set_visible(cd->dup_button, FALSE);
  gtk_list_box_unselect_all(GTK_LIST_BOX(cd->list));
  cd->filling = FALSE;
  sync_driver_fields(cd);
}

static void fill_form(ConnectDialog *cd, const LaceSavedConnection *sc) {
  cd->filling = TRUE;
  g_free(cd->editing_id);
  cd->editing_id = g_strdup(sc->id);
  gtk_label_set_text(GTK_LABEL(cd->form_title), sc->name && *sc->name ? sc->name : "Connection");
  set_row_text(cd->name_row, sc->name);
  adw_combo_row_set_selected(ADW_COMBO_ROW(cd->driver_row), (guint)driver_index(sc->driver));
  ConnectionItem *item = connmgr_find_by_id(lace_app_get_saved(cd->app), sc->id);
  char *path = folder_path_of(item);
  guint fi = 0;
  for (guint i = 0; i < cd->folder_paths->len; i++)
    if (g_strcmp0(g_ptr_array_index(cd->folder_paths, i), path) == 0)
      fi = i;
  g_free(path);
  adw_combo_row_set_selected(ADW_COMBO_ROW(cd->folder_row), fi);
  gboolean sqlite = driver_index(sc->driver) == DRV_SQLITE;
  set_row_text(cd->file_row, sqlite ? sc->database : "");
  set_row_text(cd->host_row, sc->host && *sc->host ? sc->host : "localhost");
  adw_spin_row_set_value(ADW_SPIN_ROW(cd->port_row),
                         sc->port > 0 ? sc->port : lace_driver_default_port(sc->driver ? sc->driver : "postgres"));
  set_row_text(cd->db_row, sqlite ? "" : sc->database);
  set_row_text(cd->user_row, sc->user);
  set_row_text(cd->pass_row, sc->save_password ? sc->password : "");
  adw_switch_row_set_active(ADW_SWITCH_ROW(cd->save_pass_row), sc->save_password);
  adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(cd->ssh_expander), sc->ssh);
  adw_expander_row_set_expanded(ADW_EXPANDER_ROW(cd->ssh_expander), sc->ssh);
  set_row_text(cd->ssh_host_row, sc->ssh_host);
  adw_spin_row_set_value(ADW_SPIN_ROW(cd->ssh_port_row), sc->ssh_port > 0 ? sc->ssh_port : 22);
  set_row_text(cd->ssh_user_row, sc->ssh_user);
  set_row_text(cd->ssh_pass_row, sc->save_ssh_password ? sc->ssh_password : "");
  adw_switch_row_set_active(ADW_SWITCH_ROW(cd->ssh_save_row), sc->save_ssh_password);
  gtk_widget_set_visible(cd->delete_button, TRUE);
  gtk_widget_set_visible(cd->dup_button, TRUE);
  cd->filling = FALSE;
  sync_driver_fields(cd);
}

static char *trimmed(const char *s) {
  char *t = g_strdup(s ? s : "");
  g_strstrip(t);
  return t;
}

/* The form as a saved connection (new UUID unless editing); NULL with a
 * message when a field is missing */
static LaceSavedConnection *form_to_saved(ConnectDialog *cd, char **why) {
  guint d = adw_combo_row_get_selected(ADW_COMBO_ROW(cd->driver_row));
  LaceSavedConnection *sc = lace_saved_conn_new();
  if (cd->editing_id) {
    free(sc->id);
    sc->id = str_dup(cd->editing_id);
  }
  sc->driver = str_dup(g_driver_keys[d]);
  char *name = trimmed(row_text(cd->name_row));
  if (d == DRV_SQLITE) {
    char *file = trimmed(row_text(cd->file_row));
    if (!*file) {
      *why = g_strdup("Choose a database file");
      g_free(file);
      g_free(name);
      lace_saved_conn_free(sc);
      return NULL;
    }
    sc->database = str_dup(file);
    sc->host = str_dup("");
    sc->user = str_dup("");
    if (!*name) {
      g_free(name);
      name = g_path_get_basename(file);
    }
    g_free(file);
  } else {
    char *host = trimmed(row_text(cd->host_row));
    char *db = trimmed(row_text(cd->db_row));
    char *user = trimmed(row_text(cd->user_row));
    if (!*host || !*db || !*user) {
      *why = g_strdup("Host, database and user are required");
      g_free(host); g_free(db); g_free(user); g_free(name);
      lace_saved_conn_free(sc);
      return NULL;
    }
    sc->host = str_dup(host);
    sc->database = str_dup(db);
    sc->user = str_dup(user);
    sc->port = (int)adw_spin_row_get_value(ADW_SPIN_ROW(cd->port_row));
    sc->save_password = adw_switch_row_get_active(ADW_SWITCH_ROW(cd->save_pass_row));
    const char *pw = row_text(cd->pass_row);
    sc->password = str_dup(pw ? pw : "");
    if (!*name) {
      g_free(name);
      name = g_strdup_printf("%s on %s", db, host);
    }
    sc->ssh = adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(cd->ssh_expander));
    if (sc->ssh) {
      char *sh = trimmed(row_text(cd->ssh_host_row));
      if (!*sh) {
        *why = g_strdup("The SSH host is required for a tunnel");
        g_free(sh); g_free(host); g_free(db); g_free(user); g_free(name);
        lace_saved_conn_free(sc);
        return NULL;
      }
      sc->ssh_host = str_dup(sh);
      g_free(sh);
      sc->ssh_port = (int)adw_spin_row_get_value(ADW_SPIN_ROW(cd->ssh_port_row));
      char *su = trimmed(row_text(cd->ssh_user_row));
      sc->ssh_user = *su ? str_dup(su) : NULL;
      g_free(su);
      const char *spw = row_text(cd->ssh_pass_row);
      sc->ssh_password = spw && *spw ? str_dup(spw) : NULL;
      sc->save_ssh_password = adw_switch_row_get_active(ADW_SWITCH_ROW(cd->ssh_save_row));
    }
    g_free(host); g_free(db); g_free(user);
  }
  sc->name = str_dup(name);
  g_free(name);
  return sc;
}

/* ==========================================================================
 * Actions
 * ========================================================================== */

static void show_banner(ConnectDialog *cd, const char *msg) {
  adw_banner_set_title(ADW_BANNER(cd->banner), msg);
  adw_banner_set_revealed(ADW_BANNER(cd->banner), TRUE);
}

static void set_busy(ConnectDialog *cd, gboolean busy) {
  cd->busy = busy;
  gtk_widget_set_sensitive(cd->test_button, !busy);
  gtk_widget_set_sensitive(cd->connect_button, !busy);
  gtk_widget_set_sensitive(cd->save_button, !busy);
}

static void connected(LaceConnection *conn, const char *error, gpointer data) {
  ConnectDialog *cd = data;
  set_busy(cd, FALSE);
  if (conn) {
    lace_window_add_connection(cd->window, conn);
    adw_dialog_close(cd->dialog);
  } else if (error) {
    show_banner(cd, error);
  }
}

static void tested(LaceConnection *conn, const char *error, gpointer data) {
  (void)conn;
  ConnectDialog *cd = data;
  set_busy(cd, FALSE);
  if (error)
    show_banner(cd, error);
  else
    lace_toast(GTK_WIDGET(cd->dialog), "Connection OK");
}

/* Connect from the form; the typed passwords are used even when not saved */
static void on_connect(GtkButton *b, ConnectDialog *cd) {
  (void)b;
  char *why = NULL;
  LaceSavedConnection *sc = form_to_saved(cd, &why);
  if (!sc) {
    show_banner(cd, why);
    g_free(why);
    return;
  }
  const char *pw = row_text(cd->pass_row);
  const char *spw = row_text(cd->ssh_pass_row);
  char *connstr = lace_saved_conn_build_connstr(sc, pw && *pw ? pw : NULL, spw && *spw ? spw : NULL);
  adw_banner_set_revealed(ADW_BANNER(cd->banner), FALSE);
  set_busy(cd, TRUE);
  lace_app_connect(cd->app, GTK_WIDGET(cd->dialog), connstr, cd->editing_id, sc->name, connected, cd);
  str_secure_free(connstr);
  lace_saved_conn_free(sc);
}

static void on_test(GtkButton *b, ConnectDialog *cd) {
  (void)b;
  char *why = NULL;
  LaceSavedConnection *sc = form_to_saved(cd, &why);
  if (!sc) {
    show_banner(cd, why);
    g_free(why);
    return;
  }
  const char *pw = row_text(cd->pass_row);
  const char *spw = row_text(cd->ssh_pass_row);
  char *connstr = lace_saved_conn_build_connstr(sc, pw && *pw ? pw : NULL, spw && *spw ? spw : NULL);
  adw_banner_set_revealed(ADW_BANNER(cd->banner), FALSE);
  set_busy(cd, TRUE);
  lace_app_test_connection(cd->app, GTK_WIDGET(cd->dialog), connstr, tested, cd);
  str_secure_free(connstr);
  lace_saved_conn_free(sc);
}

static void on_save(GtkButton *b, ConnectDialog *cd) {
  (void)b;
  char *why = NULL;
  LaceSavedConnection *sc = form_to_saved(cd, &why);
  if (!sc) {
    show_banner(cd, why);
    g_free(why);
    return;
  }
  ConnectionManager *mgr = lace_app_get_saved(cd->app);
  guint fi = adw_combo_row_get_selected(ADW_COMBO_ROW(cd->folder_row));
  const char *path = fi < cd->folder_paths->len ? g_ptr_array_index(cd->folder_paths, fi) : "";
  ConnectionItem *folder = folder_by_path(mgr, path);
  if (!folder)
    folder = &mgr->root;
  ConnectionItem *item = cd->editing_id ? connmgr_find_by_id(mgr, cd->editing_id) : NULL;
  if (item && item->type == CONN_ITEM_CONNECTION) {
    /* Update in place, then move if the folder changed */
    LaceSavedConnection *dst = &item->connection;
    char *id = dst->id;
    dst->id = NULL;
    connmgr_free_connection(dst);
    LaceSavedConnection *copy = lace_saved_conn_copy(sc);
    *dst = *copy;
    free(dst->id);
    dst->id = id;
    free(copy); /* Fields moved */
    char *cur = folder_path_of(item);
    if (g_strcmp0(cur, path) != 0)
      connmgr_move_item(mgr, item, folder, NULL);
    g_free(cur);
  } else {
    g_free(cd->editing_id);
    cd->editing_id = g_strdup(sc->id);
    connmgr_add_connection(folder, sc); /* Takes ownership */
    sc = NULL;
  }
  if (sc)
    lace_saved_conn_free(sc);
  if (lace_app_save_saved(cd->app, GTK_WIDGET(cd->dialog)))
    lace_toast(GTK_WIDGET(cd->dialog), "Saved");
  rebuild_list(cd);
  item = connmgr_find_by_id(mgr, cd->editing_id);
  if (item)
    fill_form(cd, &item->connection);
}

static void delete_confirmed(gboolean ok, gpointer data) {
  ConnectDialog *cd = data;
  if (!ok || !cd->editing_id)
    return;
  ConnectionManager *mgr = lace_app_get_saved(cd->app);
  ConnectionItem *item = connmgr_find_by_id(mgr, cd->editing_id);
  if (item)
    connmgr_remove_item(mgr, item);
  lace_app_save_saved(cd->app, GTK_WIDGET(cd->dialog));
  clear_form(cd);
  rebuild_list(cd);
}

static void on_delete(GtkButton *b, ConnectDialog *cd) {
  (void)b;
  if (!cd->editing_id)
    return;
  char *body = g_strdup_printf("Delete the saved connection \"%s\"?", row_text(cd->name_row));
  lace_confirm(GTK_WIDGET(cd->dialog), "Delete connection", body, "_Delete", TRUE, delete_confirmed, cd);
  g_free(body);
}

static void on_duplicate(GtkButton *b, ConnectDialog *cd) {
  (void)b;
  char *why = NULL;
  LaceSavedConnection *sc = form_to_saved(cd, &why);
  if (!sc) {
    show_banner(cd, why);
    g_free(why);
    return;
  }
  free(sc->id);
  sc->id = lace_generate_uuid();
  char *name = g_strdup_printf("%s (copy)", sc->name);
  free(sc->name);
  sc->name = str_dup(name);
  g_free(name);
  ConnectionManager *mgr = lace_app_get_saved(cd->app);
  ConnectionItem *item = connmgr_find_by_id(mgr, cd->editing_id);
  ConnectionItem *folder = item ? item->parent : &mgr->root;
  g_free(cd->editing_id);
  cd->editing_id = g_strdup(sc->id);
  connmgr_add_connection(folder ? folder : &mgr->root, sc);
  lace_app_save_saved(cd->app, GTK_WIDGET(cd->dialog));
  rebuild_list(cd);
  item = connmgr_find_by_id(mgr, cd->editing_id);
  if (item)
    fill_form(cd, &item->connection);
}

static void on_new(GtkButton *b, ConnectDialog *cd) {
  (void)b;
  clear_form(cd);
  gtk_widget_grab_focus(cd->name_row);
}

static void new_folder_cb(const char *text, gpointer data) {
  ConnectDialog *cd = data;
  if (!text || !*text)
    return;
  ConnectionManager *mgr = lace_app_get_saved(cd->app);
  ConnectionFolder *f = connmgr_new_folder(text);
  connmgr_add_folder(&mgr->root, f);
  lace_app_save_saved(cd->app, GTK_WIDGET(cd->dialog));
  rebuild_list(cd);
  refresh_folder_combo(cd);
}

static void on_new_folder(GtkButton *b, ConnectDialog *cd) {
  (void)b;
  lace_prompt(GTK_WIDGET(cd->dialog), "New folder", "A folder groups saved connections", NULL,
              "_Create", FALSE, new_folder_cb, cd);
}

static void on_row_selected(GtkListBox *lb, GtkListBoxRow *row, ConnectDialog *cd) {
  (void)lb;
  if (!row)
    return;
  const char *id = g_object_get_data(G_OBJECT(row), "id");
  ConnectionItem *item = id ? connmgr_find_by_id(lace_app_get_saved(cd->app), id) : NULL;
  if (item && item->type == CONN_ITEM_CONNECTION)
    fill_form(cd, &item->connection);
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, ConnectDialog *cd) {
  on_row_selected(lb, row, cd);
  if (cd->editing_id)
    on_connect(NULL, cd);
}

static void on_search_changed(GtkSearchEntry *e, ConnectDialog *cd) {
  (void)e;
  rebuild_list(cd);
}

static void on_url_connect(GtkWidget *w, ConnectDialog *cd) {
  (void)w;
  char *url = trimmed(row_text(cd->url_entry));
  if (!*url) {
    g_free(url);
    return;
  }
  char *connstr;
  if (strstr(url, "://")) {
    connstr = g_strdup(url);
  } else {
    char *err = NULL;
    char *c = connstr_from_path(url, &err);
    if (c) {
      connstr = g_strdup(c);
      free(c);
    } else {
      char *enc = connstr_encode_path(url);
      connstr = g_strdup_printf("sqlite://%s", enc ? enc : url);
      free(enc);
    }
    free(err);
  }
  g_free(url);
  adw_banner_set_revealed(ADW_BANNER(cd->banner), FALSE);
  set_busy(cd, TRUE);
  lace_app_connect(cd->app, GTK_WIDGET(cd->dialog), connstr, NULL, NULL, connected, cd);
  str_secure_free(connstr);
}

static void on_url_to_form(GtkButton *b, ConnectDialog *cd) {
  (void)b;
  char *url = trimmed(row_text(cd->url_entry));
  char *err = NULL;
  LaceSavedConnection *sc = strstr(url, "://") ? lace_saved_conn_from_connstr(url, &err) : NULL;
  if (!sc && !strstr(url, "://")) {
    sc = lace_saved_conn_new();
    sc->driver = str_dup("sqlite");
    sc->database = str_dup(url);
    sc->host = str_dup("");
    sc->user = str_dup("");
    sc->name = g_path_get_basename(url);
  }
  if (!sc) {
    show_banner(cd, err ? err : "The URL could not be parsed");
    free(err);
    g_free(url);
    return;
  }
  if (sc->password && *sc->password)
    sc->save_password = true;
  clear_form(cd);
  cd->filling = TRUE;
  fill_form(cd, sc);
  g_clear_pointer(&cd->editing_id, g_free);
  gtk_widget_set_visible(cd->delete_button, FALSE);
  gtk_widget_set_visible(cd->dup_button, FALSE);
  gtk_label_set_text(GTK_LABEL(cd->form_title), "New connection");
  cd->filling = FALSE;
  lace_saved_conn_free(sc);
  g_free(url);
}

static void file_chosen(GObject *src, GAsyncResult *res, gpointer data) {
  ConnectDialog *cd = data;
  GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
  if (!f)
    return;
  char *path = g_file_get_path(f);
  g_object_unref(f);
  if (path)
    set_row_text(cd->file_row, path);
  g_free(path);
}

static void on_browse_file(GtkButton *b, ConnectDialog *cd) {
  (void)b;
  GtkFileDialog *d = gtk_file_dialog_new();
  gtk_file_dialog_set_title(d, "Choose a SQLite database");
  gtk_file_dialog_open(d, lace_widget_window(GTK_WIDGET(cd->dialog)), NULL, file_chosen, cd);
  g_object_unref(d);
}

static void on_closed(AdwDialog *d, ConnectDialog *cd) {
  (void)d;
  g_ptr_array_unref(cd->folder_paths);
  g_free(cd->editing_id);
  g_free(cd);
}

/* ==========================================================================
 * Building
 * ========================================================================== */

static GtkWidget *entry_row(const char *title) {
  GtkWidget *row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  return row;
}

static GtkWidget *browse_button(const char *tooltip) {
  GtkWidget *b = gtk_button_new_from_icon_name("document-open-symbolic");
  gtk_widget_add_css_class(b, "flat");
  gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(b, tooltip);
  return b;
}

void lace_connect_dialog_show(LaceWindow *window, const char *select_id) {
  ConnectDialog *cd = g_new0(ConnectDialog, 1);
  cd->window = window;
  cd->app = lace_app_get_default();
  cd->folder_paths = g_ptr_array_new_with_free_func(g_free);

  cd->dialog = adw_dialog_new();
  adw_dialog_set_title(cd->dialog, "Connect");
  adw_dialog_set_content_width(cd->dialog, 980);
  adw_dialog_set_content_height(cd->dialog, 680);
  GtkWidget *tv = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);
  cd->banner = adw_banner_new("");
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), cd->banner);

  /* URL row */
  GtkWidget *url_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(url_box, 12);
  gtk_widget_set_margin_end(url_box, 12);
  gtk_widget_set_margin_bottom(url_box, 8);
  cd->url_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(cd->url_entry),
      "postgres://user@host/db, mysql://…, a SQLite file path, or driver+ssh://…");
  gtk_widget_set_hexpand(cd->url_entry, TRUE);
  g_signal_connect(cd->url_entry, "activate", G_CALLBACK(on_url_connect), cd);
  GtkWidget *url_form = gtk_button_new_with_label("Fill form");
  gtk_widget_set_tooltip_text(url_form, "Put the URL into the form to edit or save it");
  g_signal_connect(url_form, "clicked", G_CALLBACK(on_url_to_form), cd);
  GtkWidget *url_go = gtk_button_new_with_label("Connect");
  gtk_widget_add_css_class(url_go, "suggested-action");
  g_signal_connect(url_go, "clicked", G_CALLBACK(on_url_connect), cd);
  gtk_box_append(GTK_BOX(url_box), cd->url_entry);
  gtk_box_append(GTK_BOX(url_box), url_form);
  gtk_box_append(GTK_BOX(url_box), url_go);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), url_box);

  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  /* Saved list */
  GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_size_request(left, 300, -1);
  gtk_widget_add_css_class(left, "connect-list");
  cd->search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(cd->search), "Search saved connections");
  gtk_widget_set_margin_start(cd->search, 12);
  gtk_widget_set_margin_end(cd->search, 6);
  g_signal_connect(cd->search, "search-changed", G_CALLBACK(on_search_changed), cd);
  gtk_box_append(GTK_BOX(left), cd->search);
  GtkWidget *lscroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(lscroll, TRUE);
  cd->list = gtk_list_box_new();
  gtk_widget_add_css_class(cd->list, "navigation-sidebar");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(cd->list), GTK_SELECTION_SINGLE);
  g_signal_connect(cd->list, "row-selected", G_CALLBACK(on_row_selected), cd);
  g_signal_connect(cd->list, "row-activated", G_CALLBACK(on_row_activated), cd);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lscroll), cd->list);
  gtk_box_append(GTK_BOX(left), lscroll);
  GtkWidget *lbuttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(lbuttons, 12);
  gtk_widget_set_margin_bottom(lbuttons, 8);
  GtkWidget *new_btn = gtk_button_new();
  GtkWidget *nc = adw_button_content_new();
  adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(nc), "list-add-symbolic");
  adw_button_content_set_label(ADW_BUTTON_CONTENT(nc), "New");
  gtk_button_set_child(GTK_BUTTON(new_btn), nc);
  gtk_widget_add_css_class(new_btn, "flat");
  g_signal_connect(new_btn, "clicked", G_CALLBACK(on_new), cd);
  GtkWidget *folder_btn = gtk_button_new_from_icon_name("folder-new-symbolic");
  gtk_widget_add_css_class(folder_btn, "flat");
  gtk_widget_set_tooltip_text(folder_btn, "New folder");
  g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_new_folder), cd);
  gtk_box_append(GTK_BOX(lbuttons), new_btn);
  gtk_box_append(GTK_BOX(lbuttons), folder_btn);
  gtk_box_append(GTK_BOX(left), lbuttons);
  gtk_box_append(GTK_BOX(body), left);
  gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

  /* Form */
  GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(right, TRUE);
  GtkWidget *fscroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(fscroll, TRUE);
  GtkWidget *form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class(form, "connect-form");
  cd->form_title = gtk_label_new("New connection");
  gtk_widget_add_css_class(cd->form_title, "title-2");
  gtk_label_set_xalign(GTK_LABEL(cd->form_title), 0);
  gtk_label_set_ellipsize(GTK_LABEL(cd->form_title), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(form), cd->form_title);

  GtkWidget *g1 = adw_preferences_group_new();
  cd->name_row = entry_row("Name");
  cd->driver_row = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cd->driver_row), "Database");
  GtkStringList *drivers = gtk_string_list_new(g_driver_names);
  adw_combo_row_set_model(ADW_COMBO_ROW(cd->driver_row), G_LIST_MODEL(drivers));
  g_object_unref(drivers);
  g_signal_connect(cd->driver_row, "notify::selected", G_CALLBACK(on_driver_changed), cd);
  cd->folder_row = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cd->folder_row), "Folder");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), cd->name_row);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), cd->driver_row);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), cd->folder_row);
  gtk_box_append(GTK_BOX(form), g1);

  GtkWidget *g2 = adw_preferences_group_new();
  cd->file_row = entry_row("Database file");
  GtkWidget *bf = browse_button("Browse…");
  g_signal_connect(bf, "clicked", G_CALLBACK(on_browse_file), cd);
  adw_entry_row_add_suffix(ADW_ENTRY_ROW(cd->file_row), bf);
  cd->host_row = entry_row("Host");
  cd->port_row = adw_spin_row_new_with_range(1, 65535, 1);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cd->port_row), "Port");
  cd->db_row = entry_row("Database name");
  cd->user_row = entry_row("User");
  cd->pass_row = adw_password_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cd->pass_row), "Password");
  cd->save_pass_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cd->save_pass_row), "Save password");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(cd->save_pass_row), "Stored in connections.json, readable only by you");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), cd->file_row);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), cd->host_row);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), cd->port_row);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), cd->db_row);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), cd->user_row);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), cd->pass_row);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), cd->save_pass_row);
  gtk_box_append(GTK_BOX(form), g2);

  GtkWidget *g3 = adw_preferences_group_new();
  cd->ssh_expander = adw_expander_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cd->ssh_expander), "SSH tunnel");
  adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(cd->ssh_expander), "Reach the database through an SSH server (keys and ~/.ssh/config apply)");
  adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(cd->ssh_expander), TRUE);
  cd->ssh_host_row = entry_row("SSH host");
  cd->ssh_port_row = adw_spin_row_new_with_range(1, 65535, 1);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cd->ssh_port_row), "SSH port");
  adw_spin_row_set_value(ADW_SPIN_ROW(cd->ssh_port_row), 22);
  cd->ssh_user_row = entry_row("SSH user");
  cd->ssh_pass_row = adw_password_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cd->ssh_pass_row), "SSH password");
  cd->ssh_save_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cd->ssh_save_row), "Save SSH password");
  adw_expander_row_add_row(ADW_EXPANDER_ROW(cd->ssh_expander), cd->ssh_host_row);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(cd->ssh_expander), cd->ssh_port_row);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(cd->ssh_expander), cd->ssh_user_row);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(cd->ssh_expander), cd->ssh_pass_row);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(cd->ssh_expander), cd->ssh_save_row);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), cd->ssh_expander);
  gtk_box_append(GTK_BOX(form), g3);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(fscroll), form);
  gtk_box_append(GTK_BOX(right), fscroll);

  GtkWidget *actions = gtk_action_bar_new();
  cd->delete_button = gtk_button_new_with_label("Delete");
  gtk_widget_add_css_class(cd->delete_button, "destructive-action");
  g_signal_connect(cd->delete_button, "clicked", G_CALLBACK(on_delete), cd);
  cd->dup_button = gtk_button_new_with_label("Duplicate");
  g_signal_connect(cd->dup_button, "clicked", G_CALLBACK(on_duplicate), cd);
  gtk_action_bar_pack_start(GTK_ACTION_BAR(actions), cd->delete_button);
  gtk_action_bar_pack_start(GTK_ACTION_BAR(actions), cd->dup_button);
  cd->test_button = gtk_button_new_with_label("Test");
  g_signal_connect(cd->test_button, "clicked", G_CALLBACK(on_test), cd);
  cd->save_button = gtk_button_new_with_label("Save");
  g_signal_connect(cd->save_button, "clicked", G_CALLBACK(on_save), cd);
  cd->connect_button = gtk_button_new_with_label("Connect");
  gtk_widget_add_css_class(cd->connect_button, "suggested-action");
  g_signal_connect(cd->connect_button, "clicked", G_CALLBACK(on_connect), cd);
  gtk_action_bar_pack_end(GTK_ACTION_BAR(actions), cd->connect_button);
  gtk_action_bar_pack_end(GTK_ACTION_BAR(actions), cd->save_button);
  gtk_action_bar_pack_end(GTK_ACTION_BAR(actions), cd->test_button);
  gtk_box_append(GTK_BOX(right), actions);
  gtk_box_append(GTK_BOX(body), right);

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), body);
  adw_dialog_set_child(cd->dialog, tv);
  adw_dialog_set_default_widget(cd->dialog, cd->connect_button);
  g_signal_connect(cd->dialog, "closed", G_CALLBACK(on_closed), cd);

  refresh_folder_combo(cd);
  clear_form(cd);
  if (select_id) {
    ConnectionItem *item = connmgr_find_by_id(lace_app_get_saved(cd->app), select_id);
    if (item && item->type == CONN_ITEM_CONNECTION)
      fill_form(cd, &item->connection);
  }
  rebuild_list(cd);
  adw_dialog_present(cd->dialog, GTK_WIDGET(window));
  gtk_widget_grab_focus(cd->url_entry);
}
