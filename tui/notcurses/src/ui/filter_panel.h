/* The docked filter panel of a table view: a title row and one row per
 * filter (column, operator, value), edited in place and applied at once, above
 * the grid. Optional (preference notcurses.filters_strip); with it off the f
 * key opens the filter editor overlay instead. */
#ifndef NC_FILTER_PANEL_H
#define NC_FILTER_PANEL_H
#include "app.h"
#include "model/view.h"
#include "ui/grid.h"

#define FP_MAX_ROWS 5   /* filter rows shown at once; more scroll */
#define FP_GRID_KEEP 4  /* grid rows the panel always leaves */

bool filter_panel_enabled(const NcApp *app);
/* The panel's height for this view (title + rows), 0 when hidden, off, or
 * there is no room: avail is the content area's height */
int filter_panel_height(size_t num_filters, int avail);
int filter_panel_rows(const NcApp *app, const View *v);
Rect filter_panel_rect(const NcApp *app, const View *v);
void filter_panel_draw(NcApp *app, View *v, Rect r);
/* Keys while the panel has the focus; false: not ours */
bool filter_panel_handle_key(NcApp *app, View *v, const UiEvent *ev);
/* A mouse event inside the panel; false: outside it */
bool filter_panel_handle_mouse(NcApp *app, View *v, const UiEvent *ev, Rect r);
bool filter_panel_cursor(int *y, int *x); /* the terminal cursor while a value is edited */
/* The f key: show and focus (on the cursor column's filter), focus, or hide */
void filter_panel_toggle(NcApp *app, View *v);
void filter_panel_focus(NcApp *app, View *v, bool last); /* Tab in: first (or last) field */
void filter_panel_blur(NcApp *app, View *v);             /* focus leaves: a running edit is committed */
void filter_panel_add(NcApp *app, View *v);              /* a row for the cursor column, ready to type */
void filter_panel_remove(NcApp *app, View *v);           /* the cursor row */
/* The field columns of a panel width cols: [x, x + w) per field, del_x the
 * remove mark; value2 is the second half of the value field (BETWEEN) */
#include "core/chrome.h"
typedef ChromeFilterLayout FpLayout; /* the shared layout (core/chrome.h) */
void filter_panel_layout(int cols, bool two_values, FpLayout *L);
#endif
