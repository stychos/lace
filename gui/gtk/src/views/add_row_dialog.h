/*
 * Lace GTK frontend
 * Add-row dialog: one entry per column, inserted through lace_insert
 *
 * Columns start as the TUI's placeholders: auto-increment and generated
 * columns, columns with a default and required columns are left out of the
 * INSERT unless given a value, so the database fills them in or reports the
 * missing value; nullable columns without a default start as NULL. The
 * NULL and empty toggles write NULL or '' explicitly.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_ADD_ROW_DIALOG_H
#define LACE_GTK_ADD_ROW_DIALOG_H

#include "conn/connection.h"
#include <adwaita.h>

G_BEGIN_DECLS

typedef void (*LaceAddRowDone)(gboolean inserted, gpointer user_data);

void lace_add_row_dialog_show(GtkWidget *parent, LaceConnection *conn,
                              const char *table, const LaceSchema *schema,
                              LaceAddRowDone done, gpointer user_data);

G_END_DECLS

#endif /* LACE_GTK_ADD_ROW_DIALOG_H */
