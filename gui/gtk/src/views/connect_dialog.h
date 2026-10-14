/*
 * Lace GTK frontend
 * Connect dialog: saved connections, a URL entry and the connection form
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_CONNECT_DIALOG_H
#define LACE_GTK_CONNECT_DIALOG_H

#include "window.h"

G_BEGIN_DECLS

/* select_id: a saved connection to show first (may be NULL) */
void lace_connect_dialog_show(LaceWindow *window, const char *select_id);

G_END_DECLS

#endif /* LACE_GTK_CONNECT_DIALOG_H */
