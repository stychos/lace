/*
 * Lace GTK frontend
 * LaceApp: the application, its liblace client, saved connections, prefs
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_APP_H
#define LACE_GTK_APP_H

#include "config/connections.h"
#include "conn/connection.h"
#include "prefs.h"
#include <adwaita.h>
#include <lace.h>

G_BEGIN_DECLS

#define LACE_TYPE_APP (lace_app_get_type())
G_DECLARE_FINAL_TYPE(LaceApp, lace_app, LACE, APP, AdwApplication)

typedef struct _LaceWindow LaceWindow;

LaceApp *lace_app_new(void);
LaceApp *lace_app_get_default(void);

lace_client_t *lace_app_get_client(LaceApp *app);
LacePrefs *lace_app_get_prefs(LaceApp *app);
ConnectionManager *lace_app_get_saved(LaceApp *app);
/* Write connections.json (emits "saved-changed"); reports failure in a
 * dialog over parent */
gboolean lace_app_save_saved(LaceApp *app, GtkWidget *parent);
void lace_app_apply_color_scheme(LaceApp *app);
void lace_app_save_prefs(LaceApp *app, GtkWidget *parent);

/* A new window (workspace) with one connection form, presented */
LaceWindow *lace_app_new_window(LaceApp *app, const char *name);

GList *lace_app_get_lace_windows(LaceApp *app); /* g_list_free */
void lace_app_save_session(LaceApp *app, LaceWindow *except);

/* Connect with password prompts over parent. cb gets the connection, or
 * NULL with an error (NULL error: the user cancelled). name/saved_id may
 * be NULL. */
typedef void (*LaceConnectCb)(LaceConnection *conn, const char *error, gpointer user_data);
void lace_app_connect(LaceApp *app, GtkWidget *parent, const char *connstr,
                      const char *saved_id, const char *name, LaceConnectCb cb,
                      gpointer user_data);
/* Open and close again: cb(NULL, NULL) on success, cb(NULL, error) otherwise */
void lace_app_test_connection(LaceApp *app, GtkWidget *parent, const char *connstr,
                              LaceConnectCb cb, gpointer user_data);

/* The open connections (conn/conn_registry.c): one per saved connection or
 * password-less string, in any window. lace_app_open uses an open one when
 * there is one (cb at once), else connects (lace_app_connect) and registers
 * it; a URL goes to recent.json. Whoever places the connection holds it;
 * the last release closes it. */
void lace_app_open(LaceApp *app, GtkWidget *parent, const char *connstr, const char *saved_id,
                   const char *name, LaceConnectCb cb, gpointer user_data);
LaceConnection *lace_app_find_connection(LaceApp *app, const char *saved_id, const char *connstr);
void lace_app_hold_connection(LaceApp *app, LaceConnection *c);
void lace_app_release_connection(LaceApp *app, LaceConnection *c);
guint lace_app_connection_holders(LaceApp *app, LaceConnection *c);
guint lace_app_n_connections(LaceApp *app);

/* debug_script.c: LACE_GTK_SCRIPT driving for headless tests */
void lace_debug_script_start(LaceApp *app);
/* An expect-* command failed: main() exits with status 3 */
gboolean lace_debug_script_failed(void);

G_END_DECLS

#endif /* LACE_GTK_APP_H */
