/*
 * Lace GTK frontend
 * LaceConnectionPage: the content of one connection tab
 *
 * A sidebar of tables next to the connection's own tabs: table views and
 * SQL editors.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_CONNECTION_PAGE_H
#define LACE_GTK_CONNECTION_PAGE_H

#include "conn/connection.h"
#include "prefs.h"
#include "query_view.h"
#include "sidebar.h"
#include "table_view.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define LACE_TYPE_CONNECTION_PAGE (lace_connection_page_get_type())
G_DECLARE_FINAL_TYPE(LaceConnectionPage, lace_connection_page, LACE, CONNECTION_PAGE, GtkWidget)

LaceConnectionPage *lace_connection_page_new(LaceConnection *conn, const LacePrefs *prefs);
LaceConnection *lace_connection_page_get_connection(LaceConnectionPage *p);

/* Open (or switch to) a table; returns its view. A preview goes into the
 * one preview tab, replacing what it showed; opening the same table again
 * without preview, or changing its view, keeps it. */
LaceTableView *lace_connection_page_open_table(LaceConnectionPage *p, const char *table);
LaceTableView *lace_connection_page_preview_table(LaceConnectionPage *p, const char *table);
/* The same for a database view, routine or trigger (as lace_list_objects
 * names it; table: a trigger's, detail: a routine's kind) */
LaceTableView *lace_connection_page_open_object(LaceConnectionPage *p, LaceObjectKind kind, const char *name,
                                                const char *table, const char *detail, gboolean preview);
/* Open a table filtered by a followed key: an open tab of it gets the filters, else a new one */
LaceTableView *lace_connection_page_open_filtered(LaceConnectionPage *p, const char *table,
                                                  const LaceFilter *filters, gsize n);
LaceSidebar *lace_connection_page_get_sidebar(LaceConnectionPage *p);
LaceQueryView *lace_connection_page_new_query(LaceConnectionPage *p, const char *text);
/* The current inner tab's widget: a LaceTableView, a LaceQueryView or NULL */
GtkWidget *lace_connection_page_current_view(LaceConnectionPage *p);
gboolean lace_connection_page_close_current(LaceConnectionPage *p);
/* Close the tab showing that view (a dropped table's; no "last-tab-closed"
 * for it: the connection stays); false when it is not here */
gboolean lace_connection_page_close_view(LaceConnectionPage *p, GtkWidget *view);
void lace_connection_page_select_tab(LaceConnectionPage *p, int offset);
AdwTabView *lace_connection_page_get_tab_view(LaceConnectionPage *p);
/* The view a page of that tab view shows (a preview page wraps its view) */
GtkWidget *lace_connection_page_view_of(AdwTabPage *page);
/* The page showing that view, or NULL */
AdwTabPage *lace_connection_page_page_of(LaceConnectionPage *p, GtkWidget *view);

gboolean lace_connection_page_sidebar_visible(LaceConnectionPage *p);
void lace_connection_page_set_sidebar_visible(LaceConnectionPage *p, gboolean visible);
void lace_connection_page_focus_sidebar(LaceConnectionPage *p);
void lace_connection_page_focus_content(LaceConnectionPage *p);

/* Signal "last-tab-closed": the user closed its last table or SQL editor */

G_END_DECLS

#endif /* LACE_GTK_CONNECTION_PAGE_H */
