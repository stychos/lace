/* The grid: the header row and rows of a table (or query) view, drawn from
 * the view's page cache, and the keys and mouse that move around in it. */
#ifndef LACE_NC_GRID_H
#define LACE_NC_GRID_H
#include "app.h"
#include "model/view.h"

typedef struct { int y, x, rows, cols; } Rect;

/* The content area: between the header (and the tabs strip) and the status line */
Rect content_rect(const NcApp *app);
/* Where the current view's grid goes: the content area less the sidebar and
 * the current table view's filter panel */
Rect grid_content_rect(const NcApp *app);
/* The grid's rectangle for this view: the content area, or the results part of a query view */
Rect grid_view_rect(const NcApp *app, const View *v);
/* Header row + rows; requests the missing pages of the visible range (a page
 * that failed is not asked for again until the cursor or the scroll moves) */
void grid_draw(NcApp *app, View *v, Rect r);
bool grid_handle_key(NcApp *app, View *v, const UiEvent *ev, Rect r);
bool grid_handle_mouse(NcApp *app, View *v, const UiEvent *ev, Rect r);
/* Run one hotkey action on the view; false when it is not a grid action */
bool grid_do_action(NcApp *app, View *v, HotkeyAction action);
size_t grid_rows_visible(Rect r); /* r.rows - 1 (header) */
int grid_gutter(const View *v, const NcApp *app); /* row-number column incl. its space, 0 hidden */
/* Make the cursor visible; a far jump centres it */
void grid_follow(NcApp *app, View *v, bool jumped);
/* Move to a 0-based row; with the total unknown the move waits for the count */
void grid_goto_row(NcApp *app, View *v, size_t row);
/* The pending jump (last row, go to row) once the total is known */
void grid_resolve_jump(NcApp *app, View *v);
/* A sort/filter/clear changed the view: back to the top, then reload */
void grid_state_changed(NcApp *app, View *v);

/* ui/grid_edit.c: inline editing of the cursor cell */
bool grid_editing(const View *v);
void grid_edit_start(NcApp *app, View *v);
void grid_edit_cancel(View *v);
bool grid_edit_key(NcApp *app, View *v, const UiEvent *ev); /* while editing: always consumes */
void grid_edit_draw(NcApp *app, View *v, int y, int x, int w, int right_edge);
void grid_edit_begin_frame(void);
bool grid_edit_cursor(int *y, int *x); /* where the terminal cursor goes, after grid_draw */
void grid_edit_report(NcApp *app, const char *why); /* "No change" is a plain message, the rest errors */

/* ui/dialogs/{filter_editor,sort_picker,goto,structure}.c */
void filter_editor_open(NcApp *app, View *v); /* the overlay, used when the filter panel is off */
void sort_picker_open(NcApp *app, View *v);
void goto_open(NcApp *app, View *v);
void structure_open(NcApp *app, View *v);
#endif
