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

/* The sub-views of a table tab (core/catalog_rows.c builds their rows): a
 * table has all five; a database view Data, Definition (the Structure slot)
 * and Information; a routine or trigger Definition and Information */
typedef enum { LACE_SUBVIEW_DATA, LACE_SUBVIEW_STRUCTURE, LACE_SUBVIEW_INDEXES, LACE_SUBVIEW_FOREIGN_KEYS,
               LACE_SUBVIEW_INFORMATION, LACE_SUBVIEW_COUNT } LaceSubView;
/* The session's names ("data", "structure" or "definition", ...) */
const char *lace_subview_key(LaceSubView sv, LaceObjectKind kind);
gboolean lace_subview_from_key(const char *key, LaceSubView *out);
const char *lace_object_kind_key(LaceObjectKind kind); /* "table", "view", "routine", "trigger" */
gboolean lace_object_kind_from_key(const char *key, LaceObjectKind *out);
gboolean lace_object_has_subview(LaceObjectKind kind, LaceSubView sv);

LaceTableView *lace_table_view_new(LaceConnection *conn, const char *table,
                                   const LacePrefs *prefs);
/* A view of a database view, routine or trigger (as lace_list_objects names
 * it; table: a trigger's, detail: a routine's kind); a routine or trigger
 * starts on Definition */
LaceTableView *lace_table_view_new_object(LaceConnection *conn, LaceObjectKind kind, const char *name,
                                          const char *table, const char *detail, const LacePrefs *prefs);

const char *lace_table_view_get_table(LaceTableView *v);
LaceObjectKind lace_table_view_get_object_kind(LaceTableView *v);
const char *lace_table_view_get_object_table(LaceTableView *v);
const char *lace_table_view_get_object_detail(LaceTableView *v);
LaceSubView lace_table_view_get_subview(LaceTableView *v);
void lace_table_view_set_subview(LaceTableView *v, LaceSubView sv);
/* The current sub-view's rows as text (the script driver); caller g_strfreev's; NULL on Data */
char **lace_table_view_get_subview_texts(LaceTableView *v);
/* Show the rows filtered by a followed key (the filters replace the tab's) */
void lace_table_view_show_filtered(LaceTableView *v, const LaceFilter *filters, gsize n);
/* The grid's focused cell: follow its foreign key / list the referencing tables */
void lace_table_view_follow_fk(LaceTableView *v);
void lace_table_view_referenced_by(LaceTableView *v);
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

/* Signals:
 *   touched ()                                   the user filtered, sorted or edited the view
 *   open-filtered (const char *table, gpointer filters, guint n)   a followed key: LaceFilter[n]
 *   open-object (int kind, const char *name, const char *table, int subview)  a trigger, a referencing table */

G_END_DECLS

#endif /* LACE_GTK_TABLE_VIEW_H */
