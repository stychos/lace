/*
 * Lace GTK frontend
 * LaceTableView: one table of a connection, paged, filtered, sorted, edited
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_TABLE_VIEW_H
#define LACE_GTK_TABLE_VIEW_H

#include "conn/connection.h"
#include "data_grid.h"
#include "prefs.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define LACE_TYPE_TABLE_VIEW (lace_table_view_get_type())
G_DECLARE_FINAL_TYPE(LaceTableView, lace_table_view, LACE, TABLE_VIEW, GtkWidget)

LaceTableView *lace_table_view_new(LaceConnection *conn, const char *table,
                                   const LacePrefs *prefs);

const char *lace_table_view_get_table(LaceTableView *v);
LaceConnection *lace_table_view_get_connection(LaceTableView *v);
LaceDataGrid *lace_table_view_get_grid(LaceTableView *v);

void lace_table_view_refresh(LaceTableView *v);
void lace_table_view_toggle_filters(LaceTableView *v);
void lace_table_view_goto_row(LaceTableView *v);
void lace_table_view_add_row(LaceTableView *v);
void lace_table_view_delete_selected(LaceTableView *v);
void lace_table_view_show_structure(LaceTableView *v);
void lace_table_view_focus_grid(LaceTableView *v);

/* Session: filters, search and sorts (sorts: caller frees) */
const LaceFilter *lace_table_view_get_filters(LaceTableView *v, gsize *n);
const char *lace_table_view_get_search(LaceTableView *v);
LaceSortSpec *lace_table_view_get_sorts(LaceTableView *v, gsize *n);
gboolean lace_table_view_filters_visible(LaceTableView *v);
void lace_table_view_restore(LaceTableView *v, const LaceFilter *filters, gsize n,
                             const char *search, const LaceSortSpec *sorts,
                             gsize num_sorts, gboolean filters_visible);

/* Signal: touched () when the user filtered, sorted or edited the view */

G_END_DECLS

#endif /* LACE_GTK_TABLE_VIEW_H */
