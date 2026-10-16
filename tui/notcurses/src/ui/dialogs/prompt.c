#include "ui/dialogs/prompt.h"
#include "core/utf8.h"
#include "ui/input.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include "views/text_input.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *label;
  TextInput in;
  bool done;
  PromptFn fn; void *data;
} Prompt;

static void prompt_layout(Overlay *o, NcApp *app) {
  o->cols = app->cols - 8 < 60 ? app->cols - 8 : 60;
  if (o->cols < 20) o->cols = 20;
  o->rows = 3;
}

static bool prompt_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  Prompt *p = o->data;
  if (ev->type != UI_EVENT_KEY) return true;
  if (ev->key.is_special && ev->key.key == UI_KEY_ENTER) {
    size_t cur;
    char *text = text_input_release(&p->in, &cur);
    if (!text) text = str_dup("");
    p->done = true;
    PromptFn fn = p->fn;
    void *data = p->data;
    overlay_pop_this(app, o);
    if (fn) fn(app, text, data);
    else free(text);
    return true;
  }
  if (ev->key.is_special && ev->key.key == UI_KEY_ESCAPE) return false;
  nc_text_input_key(&p->in, ev);
  return true;
}

static void prompt_draw(Overlay *o, NcApp *app) {
  Prompt *p = o->data;
  const Theme *t = &app->theme;
  struct ncplane *pl = o->plane;
  int w = o->cols - 2;
  nc_style(pl, t, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
  nc_put(pl, 1, 2, w, p->label);
  /* the shown text: masked when secret, scrolled so the cursor is visible */
  const char *buf = p->in.buf ? p->in.buf : "";
  size_t len = p->in.len, cur = p->in.cursor < len ? p->in.cursor : len;
  char *disp;
  size_t dcur;
  if (p->in.secret) {
    size_t n = utf8_count(buf, len);
    disp = safe_malloc(n + 1);
    memset(disp, '*', n);
    disp[n] = 0;
    dcur = utf8_count(buf, cur);
  } else {
    disp = str_dup(buf);
    dcur = cur;
  }
  size_t start = 0;
  while (start < dcur && utf8_width(disp + start, dcur - start) > w - 1)
    start = utf8_next(disp, strlen(disp), start);
  nc_style(pl, t, TC_OVERLAY_FG, TC_EDIT_BG, 0);
  nc_put(pl, 2, 2, w, disp + start);
  o->wants_cursor = true;
  o->cursor_y = 1;
  o->cursor_x = 1 + utf8_width(disp + start, dcur - start);
  free(disp);
}

static void prompt_closed(Overlay *o, NcApp *app) {
  Prompt *p = o->data;
  if (!p->done && p->fn) {
    p->done = true;
    p->fn(app, NULL, p->data);
  }
}

static void prompt_free(void *data) {
  Prompt *p = data;
  text_input_free(&p->in);
  free(p->label);
  free(p);
}

void prompt_open(NcApp *app, const char *title, const char *label, const char *initial, bool secret,
                 PromptFn fn, void *data) {
  Prompt *p = safe_calloc(1, sizeof *p);
  p->label = str_dup(label ? label : "");
  text_input_init(&p->in, initial, secret);
  p->fn = fn;
  p->data = data;
  Overlay *o = overlay_new(title);
  o->footer = "Enter accept  Esc cancel";
  o->data = p;
  o->free_data = prompt_free;
  o->layout = prompt_layout;
  o->draw = prompt_draw;
  o->key = prompt_key;
  o->closed = prompt_closed;
  overlay_push(app, o);
}
