/* The sub-views of a table view: the strip naming them (Data, Structure,
 * Indexes, Foreign keys, Information; a database view's Definition in the
 * Structure slot; a routine's or trigger's Definition and Information) on
 * the first row of the view's area, and the read-only lists behind every one
 * but Data, which is the grid. Each list is built from the view's schema and
 * what ui/sv_*.c load (app_catalog.c) on every draw, so it always shows the
 * latest state, and is navigated with the list keys and Enter. */
#ifndef NC_SUBVIEWS_H
#define NC_SUBVIEWS_H
#include "app.h"
#include "model/view.h"
#include "ui/grid.h"
#include "core/catalog_rows.h"

/* The strip's height: 1 while the current view is a table view and the
 * terminal is tall enough, else 0 */
int subviews_rows(const NcApp *app);
/* The strip's rectangle: the first row right of the sidebar */
Rect subviews_rect(const NcApp *app);
const char *subview_label(const View *v, SubView sv); /* "Data", "Structure" or "Definition", ... */
/* Switch (when the object has that sub-view; else says so) */
void subview_set(NcApp *app, View *v, SubView sv);
/* The strip; records app->subview_spans (value: the SubView) */
void subviews_draw(NcApp *app, View *v);
/* The current sub-view's list (not Data) in r */
void subview_draw(NcApp *app, View *v, Rect r);
/* Keys of the list (the moves, Enter); false: not ours */
bool subview_handle_key(NcApp *app, View *v, const UiEvent *ev, Rect r);
/* A click on the strip switches; one in the list selects, the wheel scrolls */
bool subviews_handle_mouse(NcApp *app, View *v, const UiEvent *ev, Rect r);
/* The actions a sub-view takes: the switches (any sub-view), and in a list
 * the navigation hotkeys and refresh; false: not handled here */
bool subviews_do_action(NcApp *app, View *v, HotkeyAction a);
/* Whether an action makes sense in a list (a non-Data sub-view): the
 * switches, the navigation, refresh, the sidebar and filter keys */
bool subview_action_applies(const View *v, HotkeyAction a);
/* What Enter does in the current list, for the status line; NULL: nothing */
const char *subview_enter_hint(const View *v);
/* Lay out the cells of the strip in width columns: n labels of the given
 * widths with " " padding and a separator, scrolled so the current one shows;
 * returns the first cell shown (as tabstrip_layout) */
size_t subviews_layout(const int *widths, size_t n, size_t current, int width, int *x0, int *x1);

/* ---- shared by the sub-view lists (ui/sv_*.c): the rows come from the
 * shared builders of core/catalog_rows.c ---- */
typedef struct {
  /* Fill t for the view; false with *message (allocated) when there is only
   * a message to show (loading, an error, nothing) */
  bool (*build)(NcApp *app, View *v, CatalogRows *t, char **message);
  void (*activate)(NcApp *app, View *v, int tag); /* Enter on a row with that tag; NULL: none */
  const char *hint; /* what Enter does, for the status line; NULL: nothing */
} SvOps;
extern const SvOps sv_structure_ops, sv_definition_ops, sv_indexes_ops, sv_fks_ops, sv_info_ops;
#endif
