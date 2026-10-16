/* The tabs strip: one line under the header with a tab per view (table or
 * query) of every connection tab of the workspace, in one flat row, the
 * current one marked, the preview view's label in italics. Optional
 * (preference notcurses.tabs_strip). */
#ifndef NC_TABSTRIP_H
#define NC_TABSTRIP_H
#include "app.h"
#include <stddef.h>

#define TABSTRIP_LABEL_MAX 24 /* display columns of a label before it is cut */

/* One cell of the strip: view `view` of connection tab `tab`, or the tab
 * itself (view SIZE_MAX) when it has no views, so every connection tab has a
 * cell and the tab keys, Alt+1..9 and a click reach it */
typedef struct { size_t tab, view; } StripEntry;
/* The cells of the workspace's strip in order: the views of every connection
 * tab, tab by tab, one cell for a tab without views. *current gets the cell
 * of the current tab's current view (or the tab's own cell), SIZE_MAX with no
 * tab. The caller frees the array. */
size_t tabstrip_entries(const NcWorkspace *ws, StripEntry **out, size_t *current);

/* The strip's height: 1 when it is on and a connection tab is open, else 0 */
int tabstrip_rows(const NcApp *app);
/* Lay out n tabs of the given label widths (cells are " label " plus one
 * separator column) in width columns, scrolled so that the current one shows
 * whole; x0/x1 (n entries) get each tab's cell [x0, x1), 0/0 when it is off
 * the strip. Returns the first tab shown. */
size_t tabstrip_layout(const int *widths, size_t n, size_t current, int width, int *x0, int *x1);
void tabstrip_draw(NcApp *app); /* row 1; records app->strip_spans and app->strip_close_spans (cell indexes) */
/* A mouse event on the strip's row: a left press switches to the view, a
 * middle press or one on the current tab's close mark closes; false when not
 * on the strip */
bool tabstrip_handle_mouse(NcApp *app, const UiEvent *ev);
#endif
