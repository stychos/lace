/*
 * Lace GTK frontend
 * LaceDataGrid: a GtkColumnView over a LaceRowModel
 *
 * Shows typed cells (NULL, numbers right-aligned, BLOB previews, primary
 * keys marked), sorts by header click, keeps a focused cell for keyboard
 * work (arrows, Enter/F2 to edit, Delete, Ctrl+C), and offers context menus
 * on cells and headers. It never writes: edits, deletes and sorts are
 * signalled to the owning view, which runs the jobs.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_DATA_GRID_H
#define LACE_GTK_DATA_GRID_H

#include "model/row_model.h"
#include <lace.h>
#include <adwaita.h>

G_BEGIN_DECLS

#define LACE_TYPE_DATA_GRID (lace_data_grid_get_type())
G_DECLARE_FINAL_TYPE(LaceDataGrid, lace_data_grid, LACE, DATA_GRID, GtkWidget)

LaceDataGrid *lace_data_grid_new(void);

void lace_data_grid_set_model(LaceDataGrid *g, LaceRowModel *model);
LaceRowModel *lace_data_grid_get_model(LaceDataGrid *g);

/* Borrowed: the owner keeps it alive and clears it (NULL) before freeing.
 * Marks primary-key columns and names types in the editor. */
void lace_data_grid_set_schema(LaceDataGrid *g, const LaceSchema *schema);

void lace_data_grid_set_editable(LaceDataGrid *g, gboolean editable);
gboolean lace_data_grid_get_editable(LaceDataGrid *g);
/* Why edits are off, shown in the editor and status ("no primary key") */
void lace_data_grid_set_readonly_reason(LaceDataGrid *g, const char *reason);
const char *lace_data_grid_get_readonly_reason(LaceDataGrid *g);
void lace_data_grid_set_sortable(LaceDataGrid *g, gboolean sortable);
void lace_data_grid_set_row_numbers(LaceDataGrid *g, gboolean show);

/* Sorting as the headers show it (names, primary first). get: caller frees
 * the array and each column with g_free. set: reflect a restored sort. */
LaceSortSpec *lace_data_grid_get_sorts(LaceDataGrid *g, gsize *n);
void lace_data_grid_set_sorts(LaceDataGrid *g, const LaceSortSpec *sorts, gsize n);

/* Selected row positions, ascending (caller g_frees) */
gsize *lace_data_grid_get_selected_rows(LaceDataGrid *g, gsize *n);
gsize lace_data_grid_get_selected_count(LaceDataGrid *g);
void lace_data_grid_unselect_all(LaceDataGrid *g);

gboolean lace_data_grid_get_focus_cell(LaceDataGrid *g, gsize *row, gsize *col);
void lace_data_grid_focus_cell(LaceDataGrid *g, gsize row, gsize col);
void lace_data_grid_scroll_to_row(LaceDataGrid *g, gsize row);

/* Open the editor on the focused cell; copy the focused cell or its row */
void lace_data_grid_edit_focused(LaceDataGrid *g);
void lace_data_grid_copy_focused(LaceDataGrid *g, gboolean whole_row);

/* Size columns from the rows loaded so far (call after the first page) */
void lace_data_grid_measure_columns(LaceDataGrid *g);

GtkColumnView *lace_data_grid_get_column_view(LaceDataGrid *g);
/* Diagnostics: bound cell widgets and row widgets alive */
void lace_data_grid_debug_counts(LaceDataGrid *g, guint *bound_cells, guint *row_widgets);

/* Signals:
 *   sort-changed ()                       read lace_data_grid_get_sorts()
 *   edit-cell (guint row, guint col, const char *text, gboolean set_null)
 *   delete-rows ()                        read the selection
 *   cell-focused (guint row, guint col)
 *   add-row ()
 *   refresh ()
 *   filter-column (const char *column, const char *value)
 */

G_END_DECLS

#endif /* LACE_GTK_DATA_GRID_H */
