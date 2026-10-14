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
void lace_window_set_name(LaceWindow *w, const char *name);

/* Add a tab for an open connection and make it current */
LaceConnectionPage *lace_window_add_connection(LaceWindow *w, LaceConnection *conn);
/* Connect (with prompts) and add the tab */
void lace_window_connect_to(LaceWindow *w, const char *connstr, const char *saved_id,
                            const char *name);
void lace_window_show_connect_dialog(LaceWindow *w);

LaceConnectionPage *lace_window_current_page(LaceWindow *w);
AdwTabView *lace_window_get_tab_view(LaceWindow *w);

G_END_DECLS

#endif /* LACE_GTK_WINDOW_H */
