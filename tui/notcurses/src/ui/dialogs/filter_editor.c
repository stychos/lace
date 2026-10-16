#include "app.h"
#include "sql_builder.h"
#include "ui/grid.h"
#include "ui/input.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <ctype.h>
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

#define LABEL_W 11
#define OPS (LACE_FILTER_RAW + 1) /* every operator in the enum's order, Raw SQL last */

typedef struct {
  uint64_t uid;
  char *column;
  int op;
  TextInput v1, v2;
  int focus; /* 0 operator, 1 value, 2 value 2 */
} Editor;

static const char *op_text(int op) { return op == LACE_FILTER_RAW ? "Raw SQL" : lace_filter_op_label((LaceFilterOp)op); }
static bool has_v1(const Editor *e) { return e->op == LACE_FILTER_RAW || lace_filter_op_needs_value((LaceFilterOp)e->op); }
static bool has_v2(const Editor *e) { return e->op != LACE_FILTER_RAW && lace_filter_op_two_values((LaceFilterOp)e->op); }
static bool field_ok(const Editor *e, int f) { return f == 0 || (f == 1 && has_v1(e)) || (f == 2 && has_v2(e)); }

static void move_focus(Editor *e, int dir) {
  int f = e->focus;
  for (int i = 0; i < 3; i++) {
    f = (f + dir + 3) % 3;
    if (field_ok(e, f)) { e->focus = f; return; }
  }
}

static void cycle_op(Editor *e, int dir) {
  e->op = (e->op + dir + OPS) % OPS;
  if (!field_ok(e, e->focus)) e->focus = 0;
}

static void apply(Overlay *o, NcApp *app, bool remove) {
  Editor *e = o->data;
  View *v = app_find_view(app, e->uid);
  char *col = str_dup(e->column);
  char *a = str_dup(e->v1.buf ? e->v1.buf : ""), *b = str_dup(e->v2.buf ? e->v2.buf : "");
  int op = e->op;
  bool needs = has_v1(e), two = has_v2(e);
  overlay_pop_this(app, o);
  if (v) {
    size_t at = (size_t)-1;
    for (size_t i = 0; i < v->num_filters; i++)
      if (v->filters[i].column && !strcmp(v->filters[i].column, col)) at = i;
    if (op == LACE_FILTER_RAW) {
      if (*a) {
        view_checkpoint(v);
        view_add_raw_filter(v, a);
        grid_state_changed(app, v);
      }
    } else if (remove || (needs && !*a) || (two && !*b)) {
      /* No value, no filter: an empty field removes the one on this column */
      if (at != (size_t)-1) {
        view_checkpoint(v);
        view_remove_filter(v, at);
        grid_state_changed(app, v);
      }
    } else {
      view_checkpoint(v);
      view_set_filter(v, col, (LaceFilterOp)op, needs ? a : NULL, two ? b : NULL);
      grid_state_changed(app, v);
    }
  }
  free(col);
  free(a);
  free(b);
}

static bool editor_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  Editor *e = o->data;
  if (ev->type != UI_EVENT_KEY) return true;
  const bool special = ev->key.is_special;
  if (special && ev->key.key == UI_KEY_ESCAPE) return false;
  if (special && ev->key.key == UI_KEY_ENTER) {
    apply(o, app, false);
    return true;
  }
  if (!special && (ev->key.mods & UI_MOD_CTRL) && toupper(ev->key.key) == 'D') {
    apply(o, app, true);
    return true;
  }
  if (special && (ev->key.key == UI_KEY_TAB || ev->key.key == UI_KEY_DOWN)) { move_focus(e, 1); return true; }
  if (special && (ev->key.key == UI_KEY_BTAB || ev->key.key == UI_KEY_UP)) { move_focus(e, -1); return true; }
  if (special && (ev->key.mods & UI_MOD_ALT) && (ev->key.key == UI_KEY_LEFT || ev->key.key == UI_KEY_RIGHT)) {
    cycle_op(e, ev->key.key == UI_KEY_LEFT ? -1 : 1); /* from any field */
    return true;
  }
  if (e->focus == 0) {
    if (special && ev->key.key == UI_KEY_LEFT) { cycle_op(e, -1); return true; }
    if (special && ev->key.key == UI_KEY_RIGHT) { cycle_op(e, 1); return true; }
    /* typing from the operator row goes to the value */
    bool text = !special && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT)) && (ev->key.key >= 32 || ev->key.text[0]);
    if (text && has_v1(e)) e->focus = 1;
    else return true;
  }
  nc_text_input_key(e->focus == 2 ? &e->v2 : &e->v1, ev);
  return true;
}

static void editor_draw(Overlay *o, NcApp *app) {
  Editor *e = o->data;
  struct ncplane *pl = o->plane;
  const Theme *t = &app->theme;
  int fw = o->cols - LABEL_W - 3;
  o->wants_cursor = false;
  nc_style(pl, t, TC_DIM, TC_OVERLAY_BG, 0);
  nc_put(pl, 1, 2, LABEL_W, "Column:");
  nc_put(pl, 2, 2, LABEL_W, "Operator:");
  nc_put(pl, 3, 2, LABEL_W, has_v1(e) ? (e->op == LACE_FILTER_RAW ? "Condition:" : "Value:") : "");
  nc_put(pl, 4, 2, LABEL_W, has_v2(e) ? "Value 2:" : "");
  nc_style(pl, t, TC_OVERLAY_FG, TC_OVERLAY_BG, NCSTYLE_BOLD);
  nc_put(pl, 1, 2 + LABEL_W, fw, e->op == LACE_FILTER_RAW ? "(any)" : e->column);
  char *op = str_printf("\xe2\x97\x82 %s \xe2\x96\xb8", op_text(e->op));
  nc_style(pl, t, e->focus == 0 ? TC_ACCENT : TC_OVERLAY_FG, TC_OVERLAY_BG, e->focus == 0 ? NCSTYLE_BOLD : 0);
  nc_put(pl, 2, 2 + LABEL_W, fw, op);
  free(op);
  if (has_v1(e)) {
    int cx = nc_text_input_draw(pl, t, &e->v1, 3, 2 + LABEL_W, fw, e->focus == 1);
    if (e->focus == 1) { o->wants_cursor = true; o->cursor_y = 2; o->cursor_x = 1 + LABEL_W + cx; }
  }
  if (has_v2(e)) {
    int cx = nc_text_input_draw(pl, t, &e->v2, 4, 2 + LABEL_W, fw, e->focus == 2);
    if (e->focus == 2) { o->wants_cursor = true; o->cursor_y = 3; o->cursor_x = 1 + LABEL_W + cx; }
  }
}

static void editor_layout(Overlay *o, NcApp *app) {
  o->cols = app->cols - 8 < 64 ? app->cols - 8 : 64;
  if (o->cols < 30) o->cols = 30;
  o->rows = 5;
}

static void editor_free(void *data) {
  Editor *e = data;
  text_input_free(&e->v1);
  text_input_free(&e->v2);
  free(e->column);
  free(e);
}

void filter_editor_open(NcApp *app, View *v) {
  const LaceColumn *c = page_cache_column(v->cache, v->cursor_col);
  if (!c || !c->name) {
    app_set_status(app, false, "No column to filter");
    return;
  }
  Editor *e = safe_calloc(1, sizeof *e);
  e->uid = v->uid;
  e->column = str_dup(c->name);
  e->op = LACE_FILTER_EQ;
  text_input_init(&e->v1, NULL, false);
  text_input_init(&e->v2, NULL, false);
  for (size_t i = 0; i < v->num_filters; i++) { /* edit the filter that is there */
    const LaceFilter *f = &v->filters[i];
    if (!f->column || strcmp(f->column, e->column) != 0) continue;
    e->op = f->op;
    if (f->value) text_input_set(&e->v1, f->value);
    if (f->value2) text_input_set(&e->v2, f->value2);
  }
  e->focus = has_v1(e) ? 1 : 0;
  Overlay *o = overlay_new("Filter");
  o->footer = "Enter apply  Tab next  Alt+\xe2\x86\x90\xe2\x86\x92 operator  ^D remove";
  o->data = e;
  o->free_data = editor_free;
  o->draw = editor_draw;
  o->key = editor_key;
  o->layout = editor_layout;
  overlay_push(app, o);
}
