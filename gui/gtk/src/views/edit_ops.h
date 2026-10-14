/*
 * Lace GTK frontend
 * The one write path for grids: verified cell updates and row deletes
 *
 * Table tabs and query results both edit through a LaceEditContext. It
 * builds the row's primary key by column name, snapshots the columns to
 * verify, runs the update or delete as a job (the worker re-reads the row
 * with lace_select_by_pk before writing) and reflects the result in the
 * row model.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_EDIT_OPS_H
#define LACE_GTK_EDIT_OPS_H

#include "conn/connection.h"
#include "data_grid.h"
#include "model/row_model.h"

G_BEGIN_DECLS

typedef void (*LaceEditAfterDelete)(gsize deleted, gpointer user_data);

typedef struct {
  /* Set by the owner */
  GObject *owner;           /* Jobs are dropped once it is gone */
  GtkWidget *widget;        /* For toasts and dialogs */
  LaceConnection *conn;
  LaceRowModel *model;
  LaceDataGrid *grid;
  const LaceSchema *schema; /* Borrowed */
  const char *table;        /* Borrowed */
  gboolean unique_names;    /* Key columns must appear once (query results) */
  gboolean snapshot_all;    /* Verify every loaded table column, not only the edited one */
  gboolean confirm_delete;
  LaceEditAfterDelete after_delete;
  gpointer user_data;

  /* Private */
  int pending;
  int ok;
  gsize min_row;
  char *error;
} LaceEditContext;

/* Write one cell (text NULL with set_null: explicit NULL) */
void lace_edit_cell(LaceEditContext *ctx, gsize row, gsize col,
                    const char *text, gboolean set_null);

/* Delete the grid's selected rows (or the focused row), asking first when
 * confirm_delete is set */
void lace_edit_delete_selected(LaceEditContext *ctx);

void lace_edit_context_clear(LaceEditContext *ctx);

G_END_DECLS

#endif /* LACE_GTK_EDIT_OPS_H */
