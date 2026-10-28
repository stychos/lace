/*
 * Lace
 * The shared list (core/list_model.h) drawn in an overlay, as the
 * notcurses TUI draws it: the filter line ("> text"), headers in capitals,
 * the cursor row in the cursor colours, keys right-aligned, details beside them
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_LIST_DRAW_H
#define LACE_TUI_LIST_DRAW_H

#include "../core/list_model.h"
#include "overlay.h"

/* In the overlay's content at (y, x), rows x cols; *cursor_y and *cursor_x
 * (content coordinates) get the filter's cursor when the list has an input */
void tui_list_draw(ListWidget *l, TuiOverlay *o, int y, int x, int rows, int cols, int *cursor_y, int *cursor_x);
/* A mouse event in screen coordinates for a list drawn at content (y, rows):
 * a click picks (LIST_ACTIVATE), the wheel moves */
ListAction tui_list_mouse(ListWidget *l, const TuiOverlay *o, const UiEvent *ev, int y, int rows);

/* The list in the overlay until Esc, a click outside it, a resize, or an
 * activation that activate() answers true for (NULL: every activation
 * closes). Returns the row that closed it, or NULL. The overlay stays
 * open: the caller closes it. */
ListRow *tui_list_overlay_run(TuiOverlay *o, ListWidget *l, bool (*activate)(ListRow *row, void *ctx), void *ctx);

#endif /* LACE_TUI_LIST_DRAW_H */
