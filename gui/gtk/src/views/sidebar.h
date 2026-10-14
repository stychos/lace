/*
 * Lace GTK frontend
 * LaceSidebar: a connection's tables, searchable
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_SIDEBAR_H
#define LACE_GTK_SIDEBAR_H

#include "conn/connection.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define LACE_TYPE_SIDEBAR (lace_sidebar_get_type())
G_DECLARE_FINAL_TYPE(LaceSidebar, lace_sidebar, LACE, SIDEBAR, GtkWidget)

LaceSidebar *lace_sidebar_new(LaceConnection *conn);
void lace_sidebar_focus_search(LaceSidebar *s);
void lace_sidebar_focus_list(LaceSidebar *s);

/* Signals:
 *   preview-table (const char *table)   a single click: show in the preview tab
 *   open-table (const char *table)      Enter or a double click: keep it
 *   show-structure (const char *table)
 *   new-query ()
 */

G_END_DECLS

#endif /* LACE_GTK_SIDEBAR_H */
