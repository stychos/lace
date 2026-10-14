/*
 * Lace GTK frontend
 * LaceRowModel: a GListModel over a sparse cache of result pages
 *
 * The model reports the whole table's row count, so a GtkColumnView scrolls
 * it natively, but holds only the pages near what is shown. An item for a
 * row whose page is missing asks for that page (the fetch callback) and
 * shows as a placeholder until lace_row_model_page_loaded() lands the page;
 * the item then emits "changed" and its bound cells redraw, without the
 * view rebuilding widgets. Pages are evicted least recently used.
 *
 * Every reset starts a new generation; pages delivered for an older
 * generation are dropped, so rows of a previous sort, filter or table are
 * never merged into the current view.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_ROW_MODEL_H
#define LACE_GTK_ROW_MODEL_H

#include <gio/gio.h>
#include <lace.h>

G_BEGIN_DECLS

#define LACE_TYPE_ROW_MODEL (lace_row_model_get_type())
G_DECLARE_FINAL_TYPE(LaceRowModel, lace_row_model, LACE, ROW_MODEL, GObject)

#define LACE_TYPE_ROW_ITEM (lace_row_item_get_type())
G_DECLARE_FINAL_TYPE(LaceRowItem, lace_row_item, LACE, ROW_ITEM, GObject)

typedef void (*LaceRowModelFetch)(LaceRowModel *model, gsize page,
                                  gpointer user_data);

LaceRowModel *lace_row_model_new(gsize page_size);
gsize lace_row_model_get_page_size(LaceRowModel *m);
/* Only right after a reset (a whole result shown as one page) */
void lace_row_model_set_page_size(LaceRowModel *m, gsize page_size);
void lace_row_model_set_fetch(LaceRowModel *m, LaceRowModelFetch fetch,
                              gpointer user_data);

/* Forget every page and column; a new generation. The row count is unknown
 * again (0 items until the first page lands). */
void lace_row_model_reset(LaceRowModel *m);
guint64 lace_row_model_get_generation(LaceRowModel *m);

/* Columns, copied from the first page (names and types) */
gsize lace_row_model_n_columns(LaceRowModel *m);
const LaceColumn *lace_row_model_get_column(LaceRowModel *m, gsize col);

/* The total row count: known (exact or approximate) or still growing by
 * one page as full pages come back */
void lace_row_model_set_total(LaceRowModel *m, gsize total, gboolean known,
                              gboolean approximate);
gsize lace_row_model_get_total(LaceRowModel *m);
gboolean lace_row_model_total_known(LaceRowModel *m);
gboolean lace_row_model_total_approximate(LaceRowModel *m);
/* Rows a page has actually shown (max loaded end) */
gsize lace_row_model_get_extent(LaceRowModel *m);

/* Land a page (takes the result). A short page makes the total known.
 * Ignored when generation is not the current one. */
void lace_row_model_page_loaded(LaceRowModel *m, guint64 generation,
                                gsize page, LaceResult *result);
void lace_row_model_page_failed(LaceRowModel *m, guint64 generation, gsize page);

/* Ask for the page holding row (no-op if loaded or pending) */
void lace_row_model_request(LaceRowModel *m, gsize row);
gboolean lace_row_model_page_loaded_p(LaceRowModel *m, gsize page);
gboolean lace_row_model_page_pending(LaceRowModel *m, gsize page);
gboolean lace_row_model_row_loaded(LaceRowModel *m, gsize row);

/* The page result holding row, and the row's index in it; NULL if missing */
LaceResult *lace_row_model_row_result(LaceRowModel *m, gsize row, gsize *in_page);
const LaceValue *lace_row_model_get_cell(LaceRowModel *m, gsize row, gsize col);

/* Replace one cell (takes value); the row's item emits "changed" */
void lace_row_model_set_cell(LaceRowModel *m, gsize row, gsize col, LaceValue value);

/* Rows were deleted from the table: drop every page, lower the total by
 * count and tell the view the rows at `from` are gone; pages reload lazily */
void lace_row_model_rows_removed(LaceRowModel *m, gsize from, gsize count);

/* Drop every page but keep total and columns, so what is shown reloads */
void lace_row_model_invalidate(LaceRowModel *m);

/* Signals: "columns-changed" (after a reset or the first page), and
 * "total-changed" whenever the row count or its certainty changes */

/* Item: position in the model; "changed" when its data appears or changes */
gsize lace_row_item_get_position(LaceRowItem *item);
LaceRowModel *lace_row_item_get_model(LaceRowItem *item);
const LaceValue *lace_row_item_get_cell(LaceRowItem *item, gsize col);
gboolean lace_row_item_is_loaded(LaceRowItem *item);

G_END_DECLS

#endif /* LACE_GTK_ROW_MODEL_H */
