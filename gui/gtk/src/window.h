/*
 * Lace GTK frontend
 * LaceWindow: one workspace, holding connection tabs
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_WINDOW_H
#define LACE_GTK_WINDOW_H

#include "app.h"
#include "views/connection_page.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define LACE_TYPE_WINDOW (lace_window_get_type())
G_DECLARE_FINAL_TYPE(LaceWindow, lace_window, LACE, WINDOW, AdwApplicationWindow)

LaceWindow *lace_window_new(LaceApp *app, const char *name);

const char *lace_window_get_name(LaceWindow *w);
/* 1, 2, ...: the order windows were created in (the script driver's "window N") */
guint lace_window_get_number(LaceWindow *w);
void lace_window_set_name(LaceWindow *w, const char *name);

/* Add a tab for an open connection at the end and make it current (it
 * holds the connection; with auto_open_first_table the first table opens) */
LaceConnectionPage *lace_window_add_connection(LaceWindow *w, LaceConnection *conn);
/* The connection takes the form's place (Open here); the form goes */
LaceConnectionPage *lace_window_place_here(LaceWindow *w, GtkWidget *form, LaceConnection *conn);
/* The connection in a tab right after the form, which stays current */
LaceConnectionPage *lace_window_place_after(LaceWindow *w, GtkWidget *form, LaceConnection *conn);
/* A connection form at position (-1: after the current tab), made current */
typedef struct _LaceFormPage LaceFormPage;
LaceFormPage *lace_window_add_form(LaceWindow *w, int position);
/* The current tab's form, or NULL when it shows a connection */
LaceFormPage *lace_window_current_form(LaceWindow *w);
/* Connect (with prompts) and add the tab */
void lace_window_connect_to(LaceWindow *w, const char *connstr, const char *saved_id,
                            const char *name);

/* The current tab's connection page, or NULL (a form, or no tab) */
LaceConnectionPage *lace_window_current_page(LaceWindow *w);
AdwTabView *lace_window_get_tab_view(LaceWindow *w);

G_END_DECLS

#endif /* LACE_GTK_WINDOW_H */
