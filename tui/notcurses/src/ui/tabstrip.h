/* The tabs strip: one line under the header with a tab per view (table or
 * query) of the current connection, the current one marked, the preview
 * view's label in italics. Optional
 * (preference notcurses.tabs_strip). */
#ifndef NC_TABSTRIP_H
#define NC_TABSTRIP_H
#include "app.h"
#include <stddef.h>

#define TABSTRIP_LABEL_MAX 24 /* display columns of a label before it is cut */

/* The strip's height: 1 when it is on and a connection tab is open, else 0 */
int tabstrip_rows(const NcApp *app);
/* Lay out n tabs of the given label widths (cells are " label " plus one
 * separator column) in width columns, scrolled so that the current one shows
 * whole; x0/x1 (n entries) get each tab's cell [x0, x1), 0/0 when it is off
 * the strip. Returns the first tab shown. */
size_t tabstrip_layout(const int *widths, size_t n, size_t current, int width, int *x0, int *x1);
void tabstrip_draw(NcApp *app); /* row 1; records app->strip_spans and app->strip_close_spans */
/* A mouse event on the strip's row: a left press switches, a middle press or
 * one on the current tab's close mark closes; false when not on the strip */
bool tabstrip_handle_mouse(NcApp *app, const UiEvent *ev);
#endif
