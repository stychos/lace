/*
 * Lace GTK frontend
 * Session: windows, their connection tabs and inner tabs (gtk-session.json)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_SESSION_H
#define LACE_GTK_SESSION_H

#include "app.h"

G_BEGIN_DECLS

/* Write the session of every open window (except one being closed) */
void lace_session_save(LaceApp *app, LaceWindow *except);

/* Recreate the saved windows; TRUE if at least one window was created.
 * Connections are opened one after another, prompting for passwords. */
gboolean lace_session_restore(LaceApp *app);

char *lace_session_path(void);

G_END_DECLS

#endif /* LACE_GTK_SESSION_H */
