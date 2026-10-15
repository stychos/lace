/*
 * Lace GTK frontend
 * LaceFilterBar: search box plus filter chips above a table
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_FILTER_BAR_H
#define LACE_GTK_FILTER_BAR_H

#include <lace.h>
#include <adwaita.h>

G_BEGIN_DECLS

#define LACE_TYPE_FILTER_BAR (lace_filter_bar_get_type())
G_DECLARE_FINAL_TYPE(LaceFilterBar, lace_filter_bar, LACE, FILTER_BAR, GtkWidget)

LaceFilterBar *lace_filter_bar_new(void);

/* Borrowed; set NULL before the schema is freed. Names the columns offered. */
void lace_filter_bar_set_schema(LaceFilterBar *bar, const LaceSchema *schema);

const LaceFilter *lace_filter_bar_get_filters(LaceFilterBar *bar, gsize *n);
const char *lace_filter_bar_get_search(LaceFilterBar *bar);
gboolean lace_filter_bar_is_empty(LaceFilterBar *bar);

/* Replace everything without emitting "changed" (session restore) */
void lace_filter_bar_set_filters(LaceFilterBar *bar, const LaceFilter *filters,
                                 gsize n, const char *search);
void lace_filter_bar_clear(LaceFilterBar *bar);

/* Add a filter and apply it (value NULL: an IS NULL filter) */
void lace_filter_bar_add(LaceFilterBar *bar, const char *column, LaceFilterOp op,
                         const char *value);
/* Open the editor for a new filter on column (NULL: first column) */
void lace_filter_bar_open_add(LaceFilterBar *bar, const char *column);
void lace_filter_bar_focus_search(LaceFilterBar *bar);

/* Signal: changed () */

G_END_DECLS

#endif /* LACE_GTK_FILTER_BAR_H */
