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
/* The current list's own keys for the status line; NULL: none */
const char *subview_keys_hint(const View *v);
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
  /* A STRUCTURE-category action (add, edit, drop, rename, ...) on the row
   * with that tag (CATALOG_TAG_NONE: no row); false: not one of this list's */
  bool (*action)(NcApp *app, View *v, HotkeyAction a, int tag);
  const char *keys_hint; /* the list's own keys, for the status line ("+ add  x drop  r rename") */
} SvOps;
/* The driver of the view's connection (SQLite when there is none) */
LaceDriver subview_driver(NcApp *app, const View *v);
static inline bool subview_mysql(LaceDriver d) { return d == LACE_DRIVER_MYSQL || d == LACE_DRIVER_MARIADB; }
/* The column of the view's schema with that name, or NULL */
const LaceColumn *subview_column(const View *v, const char *name);
/* The table's column names (not copied: the schema's) into an array the caller frees */
size_t subview_column_names(const View *v, const char ***out);
/* The tag under the cursor of the current list, CATALOG_TAG_NONE when none */
int subview_cursor_tag(NcApp *app, View *v);
/* Notcurses-only tags of rows its lists add beyond the shared builders'
 * (the Information actions); above every shared range */
enum {
  SV_TAG_AUTOINC = 90001, SV_TAG_COMMENT, SV_TAG_ENGINE, SV_TAG_CHARSET, SV_TAG_ANALYZE, SV_TAG_VACUUM,
  SV_TAG_OPTIMIZE, SV_TAG_EDIT_DEFINITION
};
extern const SvOps sv_structure_ops, sv_definition_ops, sv_indexes_ops, sv_fks_ops, sv_info_ops;
#endif
