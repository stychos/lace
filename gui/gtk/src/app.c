/*
 * Lace GTK frontend
 * LaceApp
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "app.h"
#include "conn/conn_registry.h"
#include "config/recent.h"
#include "session.h"
#include "util.h"
#include "views/prefs_dialog.h"
#include "window.h"
#include <config/saved_connection.h>
#include <util/connstr.h>
#include <util/str.h>
#include <stdlib.h>
#include <string.h>

struct _LaceApp {
  AdwApplication parent_instance;
  lace_client_t *client;
  LacePrefs *prefs;
  ConnectionManager *saved;
  char *saved_error;
  gboolean no_session;
  gboolean activated;
  LaceConnRegistry *conns;
};

enum { SIG_SAVED_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(LaceApp, lace_app, ADW_TYPE_APPLICATION)

static LaceApp *g_app;

LaceApp *lace_app_get_default(void) { return g_app; }
lace_client_t *lace_app_get_client(LaceApp *app) { return app->client; }
LacePrefs *lace_app_get_prefs(LaceApp *app) { return app->prefs; }
ConnectionManager *lace_app_get_saved(LaceApp *app) { return app->saved; }

gboolean lace_app_save_saved(LaceApp *app, GtkWidget *parent) {
  char *err = NULL;
  gboolean ok = connmgr_save(app->saved, &err);
  if (!ok) {
    lace_error_dialog(parent, "Could not save connections", err ? err : "unknown error");
    free(err);
  }
  g_signal_emit(app, signals[SIG_SAVED_CHANGED], 0);
  return ok;
}

void lace_app_save_prefs(LaceApp *app, GtkWidget *parent) {
  char *err = NULL;
  if (!lace_prefs_save(app->prefs, &err))
    lace_error_dialog(parent, "Could not save preferences", err ? err : "unknown error");
  free(err);
}

void lace_app_apply_color_scheme(LaceApp *app) {
  AdwStyleManager *sm = adw_style_manager_get_default();
  switch (app->prefs->color_scheme) {
  case LACE_COLOR_LIGHT:
    adw_style_manager_set_color_scheme(sm, ADW_COLOR_SCHEME_FORCE_LIGHT);
    break;
  case LACE_COLOR_DARK:
    adw_style_manager_set_color_scheme(sm, ADW_COLOR_SCHEME_FORCE_DARK);
    break;
  default:
    adw_style_manager_set_color_scheme(sm, ADW_COLOR_SCHEME_DEFAULT);
  }
}

GList *lace_app_get_lace_windows(LaceApp *app) {
  GList *out = NULL;
  for (GList *l = gtk_application_get_windows(GTK_APPLICATION(app)); l; l = l->next)
    if (LACE_IS_WINDOW(l->data))
      out = g_list_append(out, l->data);
  return out;
}

void lace_app_save_session(LaceApp *app, LaceWindow *except) {
  lace_session_save(app, except);
}

LaceWindow *lace_app_new_window(LaceApp *app, const char *name) {
  LaceWindow *w = lace_window_new(app, name);
  lace_window_add_form(w, -1); /* a new workspace starts with a connection form */
  gtk_window_present(GTK_WINDOW(w));
  return w;
}

/* ==========================================================================
 * Connecting (a thread per attempt, prompts between attempts)
 * ========================================================================== */

typedef struct {
  LaceApp *app;
  GtkWidget *parent;
  char *connstr;
  char *saved_id;
  char *name;
  LaceConnectCb cb;
  gpointer user_data;
  gboolean test_only;
  int attempts;
  /* Thread results */
  int conn_id;
  int code;
  char *error;
} ConnectReq;

static void connect_req_free(ConnectReq *r) {
  str_secure_free(r->connstr);
  g_free(r->saved_id);
  g_free(r->name);
  g_free(r->error);
  g_clear_object(&r->parent);
  g_free(r);
}

static void connect_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
  (void)src; (void)c;
  ConnectReq *r = data;
  lace_client_t *client = r->app->client;
  int id = -1;
  int rc = lace_connect(client, r->connstr, NULL, &id);
  r->code = rc;
  if (rc != LACE_OK) {
    r->error = g_strdup(lace_client_error(client));
  } else {
    r->conn_id = id;
    if (r->test_only)
      lace_disconnect(client, id);
  }
  g_task_return_boolean(task, rc == LACE_OK);
}

static void connect_start(ConnectReq *r);

static gboolean looks_like_ssh_auth(const char *err) {
  if (!err)
    return FALSE;
  char *l = g_ascii_strdown(err, -1);
  gboolean r = strstr(l, "permission denied") != NULL || strstr(l, "ssh") != NULL;
  g_free(l);
  return r;
}

static gboolean looks_like_db_auth(int code, const char *err) {
  if (lace_is_auth_error(code))
    return TRUE;
  if (!err)
    return FALSE;
  char *l = g_ascii_strdown(err, -1);
  gboolean r = strstr(l, "password") != NULL || strstr(l, "authentication") != NULL ||
               strstr(l, "access denied") != NULL;
  g_free(l);
  return r;
}

typedef struct {
  ConnectReq *req;
  gboolean ssh;
} PromptCtx;

static void password_entered(const char *text, gpointer data) {
  PromptCtx *pc = data;
  ConnectReq *r = pc->req;
  gboolean ssh = pc->ssh;
  g_free(pc);
  if (!text) {
    r->cb(NULL, NULL, r->user_data);
    connect_req_free(r);
    return;
  }
  /* Rebuild the string with the typed password, keeping any tunnel */
  LaceSavedConnection *sc = lace_saved_conn_from_connstr(r->connstr, NULL);
  if (!sc) {
    r->cb(NULL, "The connection string could not be parsed", r->user_data);
    connect_req_free(r);
    return;
  }
  char *built = lace_saved_conn_build_connstr(sc, ssh ? NULL : text, ssh ? text : NULL);
  lace_saved_conn_free(sc);
  str_secure_free(r->connstr);
  r->connstr = built;
  connect_start(r);
}

static void connect_finished(GObject *src, GAsyncResult *res, gpointer data) {
  (void)src; (void)res;
  ConnectReq *r = data;
  if (r->code == LACE_OK) {
    if (r->test_only) {
      r->cb(NULL, NULL, r->user_data);
    } else {
      LaceConnection *conn = lace_connection_new(r->app->client, r->conn_id, r->connstr,
                                                r->saved_id, r->name,
                                                r->app->prefs->history_mode,
                                                r->app->prefs->history_max_size);
      r->cb(conn, NULL, r->user_data);
      g_object_unref(conn);
    }
    connect_req_free(r);
    return;
  }
  gboolean ssh = looks_like_ssh_auth(r->error);
  gboolean db = looks_like_db_auth(r->code, r->error);
  if ((ssh || db) && r->attempts < 4 && r->parent) {
    PromptCtx *pc = g_new0(PromptCtx, 1);
    pc->req = r;
    pc->ssh = ssh && !lace_is_auth_error(r->code);
    char *display = connstr_mask_password(r->connstr);
    char *body = g_strdup_printf("%s\n\n%s", display ? display : "", r->error ? r->error : "");
    lace_prompt(r->parent, pc->ssh ? "SSH password" : "Database password", body, NULL,
                "_Connect", TRUE, password_entered, pc);
    g_free(body);
    free(display);
    return;
  }
  r->cb(NULL, r->error ? r->error : "Connection failed", r->user_data);
  connect_req_free(r);
}

static void connect_start(ConnectReq *r) {
  r->attempts++;
  GTask *task = g_task_new(NULL, NULL, connect_finished, r);
  g_task_set_task_data(task, r, NULL);
  g_task_run_in_thread(task, connect_thread);
  g_object_unref(task);
}

void lace_app_connect(LaceApp *app, GtkWidget *parent, const char *connstr,
                      const char *saved_id, const char *name, LaceConnectCb cb,
                      gpointer user_data) {
  ConnectReq *r = g_new0(ConnectReq, 1);
  r->app = app;
  r->parent = parent ? g_object_ref(parent) : NULL;
  r->connstr = g_strdup(connstr);
  r->saved_id = g_strdup(saved_id);
  r->name = g_strdup(name);
  r->cb = cb;
  r->user_data = user_data;
  if (!lace_client_connected(app->client)) {
    char *msg = g_strdup_printf("No daemon: %s", lace_client_error(app->client));
    cb(NULL, msg, user_data);
    g_free(msg);
    connect_req_free(r);
    return;
  }
  connect_start(r);
}

void lace_app_test_connection(LaceApp *app, GtkWidget *parent, const char *connstr,
                              LaceConnectCb cb, gpointer user_data) {
  ConnectReq *r = g_new0(ConnectReq, 1);
  r->app = app;
  r->parent = parent ? g_object_ref(parent) : NULL;
  r->connstr = g_strdup(connstr);
  r->cb = cb;
  r->user_data = user_data;
  r->test_only = TRUE;
  connect_start(r);
}

/* ==========================================================================
 * Open connections, shared by every window
 * ========================================================================== */

LaceConnection *lace_app_find_connection(LaceApp *app, const char *saved_id, const char *connstr) {
  return lace_conn_registry_find(app->conns, saved_id, connstr);
}

void lace_app_hold_connection(LaceApp *app, LaceConnection *c) { lace_conn_registry_hold(app->conns, c); }

void lace_app_release_connection(LaceApp *app, LaceConnection *c) {
  if (app->conns)
    lace_conn_registry_release(app->conns, c);
}

guint lace_app_connection_holders(LaceApp *app, LaceConnection *c) {
  return lace_conn_registry_holders(app->conns, c);
}

guint lace_app_n_connections(LaceApp *app) { return lace_conn_registry_count(app->conns); }

/* A connection string that is not a saved connection: to recent.json */
static void remember_recent(const char *connstr) {
  RecentList list = {0};
  recent_load(&list);
  if (recent_add(&list, connstr)) {
    char *err = NULL;
    if (!recent_save(&list, &err))
      g_warning("recent.json: %s", err ? err : "cannot save");
    free(err);
  }
  recent_free(&list);
}

typedef struct {
  LaceApp *app;
  char *saved_id;
  LaceConnectCb cb;
  gpointer user_data;
} OpenReq;

static void opened(LaceConnection *conn, const char *error, gpointer data) {
  OpenReq *o = data;
  /* Another open of the same database landed first (a session's windows
   * restore at once): that one is used, this one closed */
  LaceConnection *first = conn ? lace_conn_registry_find(o->app->conns, o->saved_id,
                                                         lace_connection_get_connstr(conn))
                               : NULL;
  if (first) {
    lace_connection_close(conn);
    o->cb(first, NULL, o->user_data);
    g_free(o->saved_id);
    g_free(o);
    return;
  }
  if (conn) {
    lace_conn_registry_add(o->app->conns, conn);
    if (!o->saved_id)
      remember_recent(lace_connection_get_connstr(conn));
  }
  o->cb(conn, error, o->user_data);
  g_free(o->saved_id);
  g_free(o);
}

void lace_app_open(LaceApp *app, GtkWidget *parent, const char *connstr, const char *saved_id,
                   const char *name, LaceConnectCb cb, gpointer user_data) {
  LaceConnection *open = lace_conn_registry_find(app->conns, saved_id, connstr);
  if (open) {
    if (!saved_id || !*saved_id)
      remember_recent(connstr);
    cb(open, NULL, user_data);
    return;
  }
  OpenReq *o = g_new0(OpenReq, 1);
  o->app = app;
  o->saved_id = saved_id && *saved_id ? g_strdup(saved_id) : NULL;
  o->cb = cb;
  o->user_data = user_data;
  lace_app_connect(app, parent, connstr, saved_id, name, opened, o);
}

/* ==========================================================================
 * Actions and lifecycle
 * ========================================================================== */

static void act_new_window(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  lace_app_new_window(LACE_APP(data), NULL);
}

static void act_quit(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceApp *app = data;
  lace_session_save(app, NULL);
  GList *wins = lace_app_get_lace_windows(app);
  for (GList *l = wins; l; l = l->next)
    gtk_window_destroy(GTK_WINDOW(l->data));
  g_list_free(wins);
  g_application_quit(G_APPLICATION(app));
}

static void act_preferences(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceApp *app = data;
  GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(app));
  lace_prefs_dialog_show(app, GTK_WIDGET(win));
}

static void act_about(GSimpleAction *a, GVariant *p, gpointer data) {
  (void)a; (void)p;
  LaceApp *app = data;
  GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(app));
  char *version = NULL;
  lace_version(app->client, &version);
  char *comments = g_strdup_printf("A database viewer and manager for SQLite, PostgreSQL, MySQL and MariaDB.%s%s",
                                   version ? "\nDaemon: " : "", version ? version : "");
  adw_show_about_dialog(GTK_WIDGET(win),
                        "application-name", "Lace",
                        "application-icon", "dev.hexed.lace",
                        "developer-name", "iloveyou",
                        "version", "0.1.0",
                        "website", "https://github.com/stychos/lace",
                        "issue-url", "https://github.com/stychos/lace/issues",
                        "license-type", GTK_LICENSE_MIT_X11,
                        "comments", comments,
                        NULL);
  g_free(comments);
  free(version);
}

static void lace_app_startup(GApplication *gapp) {
  LaceApp *app = LACE_APP(gapp);
  G_APPLICATION_CLASS(lace_app_parent_class)->startup(gapp);
  g_app = app;

  gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(gdk_display_get_default()),
                                   "/dev/hexed/lace/icons");
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/dev/hexed/lace/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);

  app->prefs = lace_prefs_load();
  app->conns = lace_conn_registry_new();
  lace_app_apply_color_scheme(app);

  char *err = NULL;
  app->saved = connmgr_load(&err);
  if (!app->saved) {
    app->saved = connmgr_new();
    app->saved_error = g_strdup(err ? err : "connections.json could not be read");
    g_warning("saved connections: %s", app->saved_error);
  }
  free(err);

  LaceClientConfig config = {0};
  config.mode = LACE_CONN_AUTO;
  config.spawn_mode = (LaceSpawnMode)app->prefs->daemon_spawn_mode;
  config.idle_timeout = app->prefs->daemon_idle_timeout;
  app->client = lace_client_create_with_config(&config);
  if (!lace_client_connected(app->client))
    g_warning("daemon: %s", lace_client_error(app->client));

  static const GActionEntry entries[] = {
      {"new-window", act_new_window, NULL, NULL, NULL, {0}},
      {"quit", act_quit, NULL, NULL, NULL, {0}},
      {"preferences", act_preferences, NULL, NULL, NULL, {0}},
      {"about", act_about, NULL, NULL, NULL, {0}},
  };
  g_action_map_add_action_entries(G_ACTION_MAP(app), entries, G_N_ELEMENTS(entries), app);
  struct { const char *action; const char *accel; } accels[] = {
      {"app.new-window", "<Control>n"}, {"app.quit", "<Control>q"},
      {"app.preferences", "<Control>comma"},
      {"win.connect", "<Control>t"}, {"win.new-query", "<Control>e"},
      {"win.close-tab", "<Control>w"}, {"win.close-connection", "<Control><Shift>w"},
      {"win.toggle-sidebar", "F9"}, {"win.search-tables", "<Control>k"},
      {"win.history", "<Control>h"}, {"win.structure", "<Control>i"},
      {"win.refresh", "F5"}, {"win.filter", "<Control>f"}, {"win.goto-row", "<Control>g"},
      {"win.run", "<Control>Return"}, {"win.run-all", "<Control><Shift>Return"},
      {"win.run-tx", "<Control><Shift>t"}, {"win.cancel", "<Control>period"},
      {"win.add-row", "<Control><Shift>n"}, {"win.rename", "<Control><Shift>r"},
      {"win.shortcuts", "<Control>question"}, {"win.next-tab", "<Control>Tab"},
      {"win.prev-tab", "<Control><Shift>Tab"},
  };
  for (gsize i = 0; i < G_N_ELEMENTS(accels); i++) {
    const char *a[] = {accels[i].accel, NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), accels[i].action, a);
  }
}

static void lace_app_activate(GApplication *gapp) {
  LaceApp *app = LACE_APP(gapp);
  GtkWindow *active = gtk_application_get_active_window(GTK_APPLICATION(app));
  if (active) {
    gtk_window_present(active);
    return;
  }
  gboolean restored = FALSE;
  if (!app->no_session && app->prefs->restore_session && !app->activated)
    restored = lace_session_restore(app);
  app->activated = TRUE;
  if (!restored) {
    LaceWindow *w = lace_app_new_window(app, NULL);
    if (app->saved_error)
      lace_error_dialog(GTK_WIDGET(w), "Saved connections are read only", app->saved_error);
  }
  lace_debug_script_start(app);
}

static int lace_app_command_line(GApplication *gapp, GApplicationCommandLine *cl) {
  LaceApp *app = LACE_APP(gapp);
  int argc = 0;
  char **argv = g_application_command_line_get_arguments(cl, &argc);
  GPtrArray *targets = g_ptr_array_new_with_free_func(g_free);
  gboolean new_window = FALSE;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (g_str_equal(a, "-s") || g_str_equal(a, "--no-session"))
      app->no_session = TRUE;
    else if (g_str_equal(a, "-n") || g_str_equal(a, "--new-window"))
      new_window = TRUE;
    else if (a[0] == '-')
      g_application_command_line_printerr(cl, "Unknown option: %s\n", a);
    else
      g_ptr_array_add(targets, g_strdup(a));
  }
  g_strfreev(argv);
  if (targets->len == 0) {
    if (new_window && gtk_application_get_active_window(GTK_APPLICATION(app)))
      lace_app_new_window(app, NULL);
    else
      g_application_activate(gapp);
  } else {
    app->activated = TRUE; /* Explicit targets: no session restore */
    LaceWindow *w = LACE_IS_WINDOW(gtk_application_get_active_window(GTK_APPLICATION(app)))
                        ? LACE_WINDOW(gtk_application_get_active_window(GTK_APPLICATION(app)))
                        : NULL;
    if (!w || new_window) {
      w = lace_window_new(app, NULL); /* the targets fill it (a failure leaves a form) */
      gtk_window_present(GTK_WINDOW(w));
    }
    const char *cwd = g_application_command_line_get_cwd(cl);
    for (guint i = 0; i < targets->len; i++) {
      const char *t = g_ptr_array_index(targets, i);
      char *connstr = NULL;
      if (strstr(t, "://")) {
        connstr = g_strdup(t);
      } else {
        char *path = g_path_is_absolute(t) ? g_strdup(t) : g_build_filename(cwd, t, NULL);
        char *err = NULL;
        char *c = connstr_from_path(path, &err);
        if (c) {
          connstr = g_strdup(c);
          free(c);
        } else {
          /* Not an existing SQLite file: let the daemon report it */
          char *enc = connstr_encode_path(path);
          connstr = g_strdup_printf("sqlite://%s", enc ? enc : path);
          free(enc);
        }
        free(err);
        g_free(path);
      }
      lace_window_connect_to(w, connstr, NULL, NULL);
      str_secure_free(connstr);
    }
    gtk_window_present(GTK_WINDOW(w));
    lace_debug_script_start(app);
  }
  g_ptr_array_unref(targets);
  return 0;
}

static void lace_app_shutdown(GApplication *gapp) {
  LaceApp *app = LACE_APP(gapp);
  if (app->client) {
    lace_client_destroy(app->client);
    app->client = NULL;
  }
  g_clear_pointer(&app->conns, lace_conn_registry_free);
  g_clear_pointer(&app->saved, connmgr_free);
  g_clear_pointer(&app->prefs, lace_prefs_free);
  g_clear_pointer(&app->saved_error, g_free);
  G_APPLICATION_CLASS(lace_app_parent_class)->shutdown(gapp);
}

static void lace_app_class_init(LaceAppClass *klass) {
  GApplicationClass *ac = G_APPLICATION_CLASS(klass);
  signals[SIG_SAVED_CHANGED] = g_signal_new("saved-changed", LACE_TYPE_APP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  ac->startup = lace_app_startup;
  ac->activate = lace_app_activate;
  ac->command_line = lace_app_command_line;
  ac->shutdown = lace_app_shutdown;
}

static void lace_app_init(LaceApp *app) { (void)app; }

LaceApp *lace_app_new(void) {
  return g_object_new(LACE_TYPE_APP, "application-id", "dev.hexed.lace",
                      "flags", G_APPLICATION_HANDLES_COMMAND_LINE, NULL);
}
