#include "ui/dialogs/confirm.h"
#include "core/utf8.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *message, *yes_label;
  bool destructive, done;
  int focus;               /* 0 Cancel, 1 Yes */
  char **lines; size_t num_lines;
  int wrapped_cols;
  ConfirmFn fn; void *data;
} Confirm;

static void clear_lines(Confirm *c) {
  for (size_t i = 0; i < c->num_lines; i++) free(c->lines[i]);
  free(c->lines);
  c->lines = NULL;
  c->num_lines = 0;
}

static void push_line(Confirm *c, const char *s, size_t n) {
  c->lines = safe_realloc(c->lines, (c->num_lines + 1) * sizeof *c->lines);
  c->lines[c->num_lines++] = strndup(s, n);
}

/* Greedy word wrap by display width; a word wider than a line is cut */
static void wrap(Confirm *c, int width) {
  clear_lines(c);
  c->wrapped_cols = width;
  const char *p = c->message;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t seg = eol ? (size_t)(eol - p) : strlen(p);
    size_t pos = 0;
    while (pos < seg) {
      size_t start = pos, last_break = 0;
      int w = 0;
      size_t i = pos;
      while (i < seg) {
        size_t nx = utf8_next(p, seg, i);
        if (nx <= i) nx = i + 1;
        int cw = utf8_width(p + i, nx - i);
        if (w + cw > width) break;
        w += cw;
        if (p[i] == ' ') last_break = i;
        i = nx;
      }
      if (i >= seg) { push_line(c, p + start, seg - start); pos = seg; break; }
      size_t cut = last_break > start ? last_break : (i > start ? i : utf8_next(p, seg, start));
      push_line(c, p + start, cut - start);
      pos = cut;
      while (pos < seg && p[pos] == ' ' && last_break > start) pos++;
    }
    if (seg == 0) push_line(c, "", 0);
    p += seg;
    if (*p == '\n') p++;
  }
}

static void buttons(const Confirm *c, int cols, int *cx, int *cw, int *yx, int *yw) {
  char *yes = str_printf("[ %s ]", c->yes_label);
  *cw = nc_text_width("[ Cancel ]");
  *yw = nc_text_width(yes);
  free(yes);
  int total = *cw + 2 + *yw;
  *cx = (cols - total) / 2;
  if (*cx < 0) *cx = 0;
  *yx = *cx + *cw + 2;
}

static void confirm_layout(Overlay *o, NcApp *app) {
  Confirm *c = o->data;
  o->cols = app->cols - 8 < 60 ? app->cols - 8 : 60;
  if (o->cols < 20) o->cols = 20;
  if (o->cols > app->cols - 4) o->cols = app->cols - 4; /* as overlay_push clamps */
  if (o->cols < 1) o->cols = 1;
  wrap(c, o->cols > 2 ? o->cols - 2 : 1);
  o->rows = (int)c->num_lines + 2;
}

static void finish(Overlay *o, NcApp *app, bool yes) {
  Confirm *c = o->data;
  c->done = true;
  ConfirmFn fn = c->fn;
  void *data = c->data;
  overlay_pop_this(app, o);
  if (fn) fn(app, yes, data);
}

static bool confirm_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  Confirm *c = o->data;
  if (ev->type == UI_EVENT_MOUSE) {
    if (ev->mouse.button != UI_MOUSE_LEFT || ev->mouse.action != UI_MOUSE_PRESS) return true;
    if (ev->mouse.y != o->rows - 1) return true;
    int cx, cw, yx, yw;
    buttons(c, o->cols, &cx, &cw, &yx, &yw);
    if (ev->mouse.x >= cx && ev->mouse.x < cx + cw) finish(o, app, false);
    else if (ev->mouse.x >= yx && ev->mouse.x < yx + yw) finish(o, app, true);
    return true;
  }
  if (ev->type != UI_EVENT_KEY) return true;
  if (ev->key.is_special) {
    switch (ev->key.key) {
    case UI_KEY_LEFT: case UI_KEY_RIGHT: case UI_KEY_TAB: case UI_KEY_BTAB: c->focus = !c->focus; return true;
    case UI_KEY_ENTER: finish(o, app, c->focus == 1); return true;
    default: return false;
    }
  }
  if (ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT)) return true;
  if (ev->key.key == 'y' || ev->key.key == 'Y') finish(o, app, true);
  else if (ev->key.key == 'n' || ev->key.key == 'N') finish(o, app, false);
  return true;
}

static void confirm_draw(Overlay *o, NcApp *app) {
  Confirm *c = o->data;
  struct ncplane *p = o->plane;
  const Theme *t = &app->theme;
  nc_style(p, t, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
  for (size_t i = 0; i < c->num_lines && (int)i < o->rows - 2; i++) nc_put(p, 1 + (int)i, 2, o->cols - 2, c->lines[i]);
  int cx, cw, yx, yw;
  buttons(c, o->cols, &cx, &cw, &yx, &yw);
  int y = o->rows;
  char *yes = str_printf("[ %s ]", c->yes_label);
  nc_style(p, t, c->focus == 0 ? TC_CURSOR_FG : TC_OVERLAY_FG, c->focus == 0 ? TC_CURSOR_BG : TC_OVERLAY_BG, 0);
  nc_put(p, y, 1 + cx, cw, "[ Cancel ]");
  ThemeColor yfg = c->focus == 1 ? TC_CURSOR_FG : (c->destructive ? TC_ERROR_FG : TC_OVERLAY_FG);
  nc_style(p, t, yfg, c->focus == 1 ? TC_CURSOR_BG : TC_OVERLAY_BG, c->destructive ? NCSTYLE_BOLD : 0);
  nc_put(p, y, 1 + yx, yw, yes);
  free(yes);
}

static void confirm_closed(Overlay *o, NcApp *app) {
  Confirm *c = o->data;
  if (!c->done && c->fn) {
    c->done = true;
    c->fn(app, false, c->data);
  }
}

static void confirm_free(void *data) {
  Confirm *c = data;
  clear_lines(c);
  free(c->message);
  free(c->yes_label);
  free(c);
}

void confirm_open(NcApp *app, const char *title, const char *message, const char *yes_label,
                  bool destructive, ConfirmFn fn, void *data) {
  Confirm *c = safe_calloc(1, sizeof *c);
  c->message = str_dup(message ? message : "");
  c->yes_label = str_dup(yes_label && *yes_label ? yes_label : "Yes");
  c->destructive = destructive;
  c->fn = fn;
  c->data = data;
  Overlay *o = overlay_new(title);
  o->footer = "y yes  n no  Esc cancel";
  o->data = c;
  o->free_data = confirm_free;
  o->layout = confirm_layout;
  o->draw = confirm_draw;
  o->key = confirm_key;
  o->closed = confirm_closed;
  overlay_push(app, o);
}
