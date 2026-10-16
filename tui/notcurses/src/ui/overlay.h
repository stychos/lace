/* The overlay stack: modal frames drawn above the content. The top overlay
 * gets every event; Esc and a click outside its frame close it. */
#ifndef NC_OVERLAY_H
#define NC_OVERLAY_H
#include "app.h"
#include "core/ui_types.h"

struct ncplane;

struct Overlay {
  char *title;
  const char *footer;                 /* key hints, a static string */
  int rows, cols;                     /* content size, excluding the frame */
  int y, x;                           /* requested frame origin; -1 = centered */
  int fy, fx;                         /* frame origin in use (set by layout) */
  struct ncplane *plane;              /* frame plane; content starts at (1,1) */
  struct ncplane *shadow;
  void *data;
  void (*free_data)(void *);
  void (*draw)(Overlay *o, NcApp *app);                    /* draws the content area of o->plane */
  bool (*key)(Overlay *o, NcApp *app, const UiEvent *ev);  /* true: consumed; mouse events arrive in content coordinates */
  void (*layout)(Overlay *o, NcApp *app);                  /* sets rows/cols (and y/x) from the screen size */
  void (*closed)(Overlay *o, NcApp *app);                  /* runs exactly once: after pop (also at exit), before free */
  bool wants_cursor;
  int cursor_y, cursor_x;             /* content coordinates */
};

Overlay *overlay_new(const char *title);        /* zeroed, y = x = -1 */
void overlay_push(NcApp *app, Overlay *o);      /* layout, create planes, show */
void overlay_pop(NcApp *app);                   /* top: closed(), planes destroyed, freed */
void overlay_pop_this(NcApp *app, Overlay *o);  /* any; used by a done callback */
Overlay *overlay_top(NcApp *app);
void overlay_layout_all(NcApp *app);
void overlay_hide_all(NcApp *app); /* move the planes off the screen: too small to show them */
void overlay_render_all(NcApp *app);
/* Esc pops; clicks outside pop; else o->key. Always consumes (modal). */
bool overlay_handle_event(NcApp *app, const UiEvent *ev);
/* At exit: close every overlay top down (closed() runs) and free it */
void overlay_free_all(NcApp *app);
#endif
