/*
 * Lace GTK frontend
 * LaceQueryView: SQL editor with paged, editable results
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_QUERY_VIEW_H
#define LACE_GTK_QUERY_VIEW_H

#include "conn/connection.h"
#include "data_grid.h"
#include "prefs.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define LACE_TYPE_QUERY_VIEW (lace_query_view_get_type())
G_DECLARE_FINAL_TYPE(LaceQueryView, lace_query_view, LACE, QUERY_VIEW, GtkWidget)

typedef enum {
  LACE_RUN_AT_CURSOR,   /* The statement at the cursor (or the selection) */
  LACE_RUN_ALL,         /* Every statement; an error stops the run */
  LACE_RUN_TRANSACTION, /* Every statement inside BEGIN; an error or a
                           cancel rolls back, otherwise COMMIT */
} LaceRunMode;

LaceQueryView *lace_query_view_new(LaceConnection *conn, const LacePrefs *prefs);

LaceConnection *lace_query_view_get_connection(LaceQueryView *v);
LaceDataGrid *lace_query_view_get_grid(LaceQueryView *v);

void lace_query_view_set_text(LaceQueryView *v, const char *sql);
char *lace_query_view_get_text(LaceQueryView *v); /* g_free */
void lace_query_view_insert_text(LaceQueryView *v, const char *sql);
void lace_query_view_focus_editor(LaceQueryView *v);
void lace_query_view_focus_results(LaceQueryView *v);

void lace_query_view_run(LaceQueryView *v, LaceRunMode mode);
void lace_query_view_cancel(LaceQueryView *v);
gboolean lace_query_view_is_running(LaceQueryView *v);
void lace_query_view_show_history(LaceQueryView *v);
void lace_query_view_goto_row(LaceQueryView *v);
void lace_query_view_delete_selected(LaceQueryView *v);
void lace_query_view_refresh(LaceQueryView *v);
/* A view opened from "New view/function": the sidebar reloads once a statement ran */
void lace_query_view_set_reload_tables(LaceQueryView *v, gboolean reload);
/* The lock banner shows (a user statement runs on the connection) */
gboolean lace_query_view_is_locked(LaceQueryView *v);
const char *lace_query_view_get_status_text(LaceQueryView *v); /* the toolbar's status (scripts) */

/* Signal: title-changed () when the tab title (first words) changes */
char *lace_query_view_get_title(LaceQueryView *v); /* g_free */

/* Signal: open-filtered (const char *table, gpointer filters, guint n): a
 * followed key of the results (one table's rows): LaceFilter[n] */
G_END_DECLS

#endif /* LACE_GTK_QUERY_VIEW_H */
