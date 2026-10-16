/* The query view: the SQL editor on top, a divider with the result summary,
 * and the results below (drawn and driven by the grid). */
#ifndef NC_QUERY_VIEW_H
#define NC_QUERY_VIEW_H
#include "app.h"
#include "model/view.h"
#include "ui/grid.h"

/* r is the whole content area; the editor takes editor_rows of it */
void query_view_draw(NcApp *app, View *v, Rect r);
bool query_view_handle_key(NcApp *app, View *v, const UiEvent *ev, Rect r);
/* The mouse (r is the whole content area); false: not ours, the grid takes it */
bool query_view_handle_mouse(NcApp *app, View *v, const UiEvent *ev, Rect r);
/* Where the terminal cursor goes after query_view_draw (editor focused) */
bool query_view_cursor(int *y, int *x);
/* The part of the content area below the divider (the grid's rectangle) */
Rect query_view_results_rect(const NcApp *app, const View *v);
/* Run one action on the view (executing, resizing the split); false when it is not one */
bool query_view_do_action(NcApp *app, View *v, HotkeyAction action);
#endif
