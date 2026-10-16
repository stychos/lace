#include "app.h"
#include "ui/grid.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  ListWidget list;
  uint64_t uid;
  bool changed, took; /* took: the picker made the view's checkpoint */
} Picker;

static void set_details(Picker *p, const View *v) {
  for (size_t i = 0; i < p->list.num_rows; i++) {
    ListRow *r = &p->list.rows[i];
    free(r->detail);
    r->detail = NULL;
    for (size_t k = 0; k < v->num_sorts; k++) {
      if (strcmp(v->sorts[k].column, r->text) != 0) continue;
      r->detail = v->num_sorts > 1 ? str_printf("%s%zu", v->sorts[k].desc ? "\xe2\x86\x93" : "\xe2\x86\x91", k + 1)
                                   : str_dup(v->sorts[k].desc ? "\xe2\x86\x93" : "\xe2\x86\x91");
      break;
    }
  }
}

static bool picker_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  Picker *p = o->data;
  View *v = app_find_view(app, p->uid);
  ListAction a = ev->type == UI_EVENT_MOUSE ? list_mouse(&p->list, ev, 0, o->rows) : list_key(&p->list, ev);
  if (a == LIST_CANCEL) overlay_pop_this(app, o);
  else if (a == LIST_ACTIVATE && v) {
    ListRow *r = list_selected(&p->list);
    if (r) {
      view_cycle_sort(v, r->text);
      p->changed = true;
      set_details(p, v);
    }
  }
  return true;
}

static void picker_draw(Overlay *o, NcApp *app) {
  Picker *p = o->data;
  p->list.detail_cols = 4;
  list_draw(&p->list, o->plane, &app->theme, 1, 1, o->rows, o->cols, NULL, NULL);
}

static void picker_layout(Overlay *o, NcApp *app) {
  Picker *p = o->data;
  o->cols = app->cols - 8 < 44 ? app->cols - 8 : 44;
  o->rows = (int)p->list.num_rows;
  if (o->rows > app->rows - 8) o->rows = app->rows - 8;
}

/* Closing applies the sorts picked, once */
static void picker_closed(Overlay *o, NcApp *app) {
  Picker *p = o->data;
  View *v = app_find_view(app, p->uid);
  if (!v || app->quit) return;
  if (p->changed) grid_state_changed(app, v);
  else if (p->took) view_checkpoint_drop(v);
}

static void picker_free(void *data) {
  Picker *p = data;
  list_free(&p->list);
  free(p);
}

void sort_picker_open(NcApp *app, View *v) {
  if (!v->schema || !v->schema->num_columns) {
    app_set_status(app, false, "The table is still loading");
    return;
  }
  Picker *p = safe_calloc(1, sizeof *p);
  list_init(&p->list, false);
  p->uid = v->uid;
  size_t n = v->schema->num_columns;
  ListRow *rows = safe_calloc(n, sizeof *rows);
  for (size_t i = 0; i < n; i++)
    rows[i] = (ListRow){str_dup(v->schema->columns[i].name ? v->schema->columns[i].name : ""), NULL, NULL, 0,
                        (void *)(intptr_t)i, false};
  list_set_rows(&p->list, rows, n);
  set_details(p, v);
  p->took = view_checkpoint(v);
  Overlay *o = overlay_new("Sort by column");
  o->footer = "Enter cycle \xe2\x86\x91/\xe2\x86\x93/off  Esc apply";
  o->data = p;
  o->free_data = picker_free;
  o->draw = picker_draw;
  o->key = picker_key;
  o->layout = picker_layout;
  o->closed = picker_closed;
  overlay_push(app, o);
}
