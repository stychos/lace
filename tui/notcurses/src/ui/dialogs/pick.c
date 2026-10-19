/* Pick one of a few items: the destination workspace, a folder to move to */
#include "ui/dialogs/pick.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdint.h>
#include <stdlib.h>

typedef struct {
  ListWidget list;
  PickFn fn;
  void *data;
  bool answered;
} Pick;

static bool pick_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  Pick *p = o->data;
  ListAction a = ev->type == UI_EVENT_MOUSE ? list_mouse(&p->list, ev, 0, o->rows) : list_key(&p->list, ev);
  if (a == LIST_CANCEL) {
    overlay_pop_this(app, o); /* closed() answers SIZE_MAX */
  } else if (a == LIST_ACTIVATE) {
    ListRow *r = list_selected(&p->list);
    size_t idx = r ? (size_t)(intptr_t)r->data : SIZE_MAX;
    PickFn fn = p->fn;
    void *data = p->data;
    p->answered = true;
    overlay_pop_this(app, o); /* frees p */
    fn(app, idx, data);
  }
  return true;
}

static void pick_draw(Overlay *o, NcApp *app) {
  Pick *p = o->data;
  list_draw(&p->list, o->plane, &app->theme, 1, 1, o->rows, o->cols, NULL, NULL);
}

static void pick_layout(Overlay *o, NcApp *app) {
  Pick *p = o->data;
  o->cols = app->cols - 8 < 50 ? app->cols - 8 : 50;
  o->rows = (int)p->list.num_rows < app->rows - 6 ? (int)p->list.num_rows : app->rows - 6;
  if (o->rows < 1) o->rows = 1;
}

static void pick_closed(Overlay *o, NcApp *app) {
  Pick *p = o->data;
  if (!p->answered) {
    p->answered = true;
    p->fn(app, SIZE_MAX, p->data);
  }
}

static void pick_free(void *data) {
  Pick *p = data;
  list_free(&p->list);
  free(p);
}

void pick_open(NcApp *app, const char *title, char **items, size_t n, PickFn fn, void *data) {
  Pick *p = safe_calloc(1, sizeof *p);
  p->fn = fn;
  p->data = data;
  list_init(&p->list, false);
  ListRow *rows = safe_calloc(n ? n : 1, sizeof *rows);
  for (size_t i = 0; i < n; i++) {
    rows[i].text = str_dup(items[i]);
    rows[i].data = (void *)(intptr_t)i;
  }
  list_set_rows(&p->list, rows, n);
  Overlay *o = overlay_new(title);
  o->footer = "Enter pick  Esc cancel";
  o->data = p;
  o->free_data = pick_free;
  o->layout = pick_layout;
  o->draw = pick_draw;
  o->key = pick_key;
  o->closed = pick_closed;
  overlay_push(app, o);
}
