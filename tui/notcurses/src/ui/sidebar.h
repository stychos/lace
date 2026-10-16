/* The docked sidebar: the connection's tables and the tab's query views, in
 * the left columns of the content area, with a search field above them that
 * is hidden until the sidebar_filter key calls it. */
#ifndef NC_SIDEBAR_H
#define NC_SIDEBAR_H
#include "app.h"
#include "ui/grid.h"

/* 0 when hidden (or the terminal is too narrow); else min(28, cols / 3) */
int sidebar_width(const NcApp *app);
void sidebar_draw(NcApp *app, ConnTab *t, Rect r);
/* Show and focus the search field (the sidebar_filter action) */
void sidebar_show_search(ConnTab *t);
/* Keys while the sidebar has the focus (and the list keys while no view is
 * open); false: not ours, the hotkeys may have it */
bool sidebar_handle_key(NcApp *app, ConnTab *t, const UiEvent *ev);
/* The name of the table under the sidebar's cursor (freed by the caller), NULL
 * when the cursor is on a query view or the list is empty */
char *sidebar_highlighted_table(ConnTab *t);
/* A click inside the sidebar (a single click on a table previews it, a
 * double click opens it for keeps); false: outside it */
bool sidebar_handle_mouse(NcApp *app, ConnTab *t, const UiEvent *ev);
/* The last draw scrolled a long name in place: the loop must tick for the next step */
bool sidebar_animating(void);
/* The display columns a name wider than its row by overflow is shifted left
 * at now_ms: a rest at the start, one column per step, a rest at the end */
int sidebar_marquee_offset(int overflow, int64_t now_ms);
/* Where the terminal cursor goes after sidebar_draw (search field focused) */
bool sidebar_cursor(int *y, int *x);
#endif
