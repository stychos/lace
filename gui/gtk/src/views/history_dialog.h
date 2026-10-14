/*
 * Lace GTK frontend
 * Query history dialog for one connection
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_HISTORY_DIALOG_H
#define LACE_GTK_HISTORY_DIALOG_H

#include "conn/connection.h"
#include <adwaita.h>

G_BEGIN_DECLS

/* use_sql (may be NULL) is called with a statement the user picked */
typedef void (*LaceHistoryUse)(const char *sql, gpointer user_data);

void lace_history_dialog_show(GtkWidget *parent, LaceConnection *conn,
                              LaceHistoryUse use_sql, gpointer user_data);

G_END_DECLS

#endif /* LACE_GTK_HISTORY_DIALOG_H */
