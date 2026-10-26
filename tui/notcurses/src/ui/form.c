#include "ui/form.h"
#include "edit_ops.h"
#include "ui/input.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <ctype.h>
#include <notcurses/notcurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_form_uid;

/* ==========================================================================
 * Building the fields
 * ========================================================================== */

static FormField *add(FormFields *ff, int id, const char *label, FormKind kind) {
  if (ff->n == ff->cap) {
    ff->cap = ff->cap ? ff->cap * 2 : 8;
    ff->v = safe_realloc(ff->v, ff->cap * sizeof *ff->v);
  }
  FormField *f = &ff->v[ff->n++];
  memset(f, 0, sizeof *f);
  f->id = id;
  f->label = str_dup(label ? label : "");
  f->kind = kind;
  return f;
}

static FormField *find(FormFields *ff, int id) {
  for (size_t i = 0; i < ff->n; i++)
    if (ff->v[i].id == id) return &ff->v[i];
  return NULL;
}

void form_fields_text(FormFields *ff, int id, const char *label, const char *text, const char *hint) {
  FormField *f = add(ff, id, label, FORM_TEXT);
  f->text = text && *text ? str_dup(text) : NULL;
  f->hint = hint ? str_dup(hint) : NULL;
}

void form_fields_toggle(FormFields *ff, int id, const char *label, bool on) {
  add(ff, id, label, FORM_TOGGLE)->on = on;
}

void form_fields_pick(FormFields *ff, int id, const char *label, const char *text, bool free_text) {
  FormField *f = add(ff, id, label, FORM_PICK);
  f->text = text && *text ? str_dup(text) : NULL;
  f->free_text = free_text;
}

void form_fields_list(FormFields *ff, int id, const char *label, const char *text) {
  FormField *f = add(ff, id, label, FORM_LIST);
  f->text = text && *text ? str_dup(text) : NULL;
  f->hint = str_dup("Enter picks, in order");
}

void form_fields_options(FormFields *ff, int id, const char *const *names, size_t n) {
  FormField *f = find(ff, id);
  if (!f) return;
  for (size_t i = 0; i < f->num_options; i++) free(f->options[i]);
  free(f->options);
  f->options = n ? safe_calloc(n, sizeof *f->options) : NULL;
  f->num_options = n;
  for (size_t i = 0; i < n; i++) f->options[i] = str_dup(names[i] ? names[i] : "");
}

void form_fields_disable(FormFields *ff, int id, const char *why) {
  FormField *f = find(ff, id);
  if (!f) return;
  f->disabled = true;
  free(f->hint);
  f->hint = str_dup(why ? why : "not changeable here");
}

static void field_free(FormField *f) {
  free(f->label);
  free(f->text);
  free(f->hint);
  for (size_t i = 0; i < f->num_options; i++) free(f->options[i]);
  free(f->options);
}

void form_fields_free(FormFields *ff) {
  for (size_t i = 0; i < ff->n; i++) field_free(&ff->v[i]);
  free(ff->v);
  memset(ff, 0, sizeof *ff);
}

/* ==========================================================================
 * Values
 * ========================================================================== */

static char *trimmed(const char *s) {
  if (!s) return str_dup("");
  while (*s && isspace((unsigned char)*s)) s++;
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) n--;
  return str_ndup(s, n);
}

size_t form_split_names(const char *text, char ***out) {
  *out = NULL;
  if (!text) return 0;
  size_t n = 0, cap = 0;
  const char *p = text;
  for (;;) {
    const char *e = strchr(p, ',');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char *part = str_ndup(p, len), *t = trimmed(part);
    free(part);
    if (*t) {
      if (n == cap) {
        cap = cap ? cap * 2 : 4;
        *out = safe_realloc(*out, cap * sizeof **out);
      }
      (*out)[n++] = t;
    } else {
      free(t);
    }
    if (!e) break;
    p = e + 1;
  }
  return n;
}

static FormField *field_mut(Form *f, int id) {
  for (size_t i = 0; i < f->n; i++)
    if (f->fields[i].id == id) return &f->fields[i];
  return NULL;
}

const FormField *form_field(const Form *f, int id) { return field_mut((Form *)f, id); }

const char *form_text(const Form *f, int id) {
  static char buf[4096]; /* the trimmed copy lives until the next call */
  const FormField *fl = form_field(f, id);
  if (!fl || !fl->text) return "";
  char *t = trimmed(fl->text);
  snprintf(buf, sizeof buf, "%s", t);
  free(t);
  return buf;
}

bool form_on(const Form *f, int id) {
  const FormField *fl = form_field(f, id);
  return fl && fl->on;
}

size_t form_names(const Form *f, int id, char ***out) {
  const FormField *fl = form_field(f, id);
  return form_split_names(fl ? fl->text : NULL, out);
}

void form_set_options(Form *f, int id, char **names, size_t n) {
  FormField *fl = field_mut(f, id);
  if (!fl) {
    lace_tables_free(names, n);
    return;
  }
  for (size_t i = 0; i < fl->num_options; i++) free(fl->options[i]);
  free(fl->options);
  fl->options = names;
  fl->num_options = n;
}

void form_set_message(Form *f, const char *msg, bool error) {
  free(f->message);
  f->message = msg ? str_dup(msg) : NULL;
  f->error = error;
}

void form_set_busy(Form *f, bool busy, const char *msg) {
  f->busy = busy;
  form_set_message(f, msg, false);
}

/* ==========================================================================
 * The overlay
 * ========================================================================== */

static void end_edit(Form *f, bool commit) {
  if (!f->editing) return;
  if (commit) {
    size_t cur;
    char *text = text_input_release(&f->in, &cur);
    FormField *fl = &f->fields[f->cur];
    free(fl->text);
    fl->text = text && *text ? text : (free(text), NULL);
  } else {
    text_input_free(&f->in);
  }
  f->editing = false;
}

static void begin_edit(Form *f, const char *typed) {
  FormField *fl = &f->fields[f->cur];
  if (fl->disabled || fl->kind == FORM_TOGGLE || (fl->kind == FORM_PICK && !fl->free_text)) return;
  text_input_init(&f->in, fl->text ? fl->text : "", false);
  if (typed) text_input_insert(&f->in, typed);
  f->editing = true;
}

static void move(Form *f, int dir) {
  end_edit(f, true);
  if (dir < 0 && f->cur > 0) f->cur--;
  else if (dir > 0 && f->cur + 1 < f->n) f->cur++;
}

static void form_layout(Overlay *o, NcApp *app) {
  Form *f = o->data;
  o->cols = app->cols - 8 < 72 ? app->cols - 8 : 72;
  if (o->cols < 30) o->cols = 30;
  o->rows = (int)f->n + 1;
  if (o->rows > app->rows - 6) o->rows = app->rows - 6;
  if (o->rows < 2) o->rows = 2;
}

static void form_draw(Overlay *o, NcApp *app) {
  Form *f = o->data;
  struct ncplane *p = o->plane;
  const Theme *t = &app->theme;
  int vis = o->rows - 1;
  if (f->cur < f->scroll) f->scroll = f->cur;
  if (f->cur >= f->scroll + (size_t)vis) f->scroll = f->cur - (size_t)vis + 1;
  int vx = 2 + f->label_w + 2, vw = o->cols - vx - 1;
  o->wants_cursor = false;
  for (int i = 0; i < vis; i++) {
    size_t k = f->scroll + (size_t)i;
    if (k >= f->n) break;
    const FormField *fl = &f->fields[k];
    int y = 1 + i;
    bool cur = k == f->cur;
    ThemeColor bg = cur ? TC_CURSOR_BG : TC_OVERLAY_BG;
    nc_style(p, t, cur ? TC_CURSOR_FG : fl->disabled ? TC_DIM : TC_OVERLAY_FG, bg, cur ? NCSTYLE_BOLD : 0);
    nc_put(p, y, 1, o->cols, "");
    nc_put(p, y, 2, f->label_w, fl->label);
    if (cur && f->editing) {
      int cx = nc_text_input_draw(p, t, &f->in, y, vx, vw, true);
      o->wants_cursor = true;
      o->cursor_y = i;
      o->cursor_x = vx - 1 + cx;
      continue;
    }
    char *shown;
    ThemeColor fg = TC_OVERLAY_FG;
    if (fl->kind == FORM_TOGGLE) {
      shown = str_dup(fl->on ? "[x] yes" : "[ ] no");
    } else if (fl->text && *fl->text) {
      shown = str_dup(fl->text);
      for (char *c = shown; *c; c++)
        if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    } else {
      shown = str_dup(fl->hint ? fl->hint : fl->kind == FORM_PICK ? "Enter picks" : "");
      fg = TC_DIM;
    }
    if (fl->disabled) fg = TC_DIM;
    nc_style(p, t, cur ? TC_CURSOR_FG : fg, bg, 0);
    int used = nc_put(p, y, vx, vw, shown);
    if ((fl->kind == FORM_PICK || fl->kind == FORM_LIST) && !fl->disabled && used + 2 <= vw)
      nc_put(p, y, vx + vw - 1, 1, "\xe2\x96\xbe"); /* ▾: a list opens here */
    free(shown);
  }
  nc_style(p, t, f->error ? TC_ERROR_FG : TC_DIM, TC_OVERLAY_BG, 0);
  nc_put(p, o->rows, 2, o->cols - 2, f->message ? f->message : "");
}

/* ---- the picker: a list overlay of the field's options ---- */

typedef struct {
  ListWidget list;
  uint64_t form_uid;
  int field_id;
} FormPicker;

static void picker_apply(NcApp *app, FormPicker *pk, const char *name) {
  Form *f = form_find(app, pk->form_uid);
  FormField *fl = f ? field_mut(f, pk->field_id) : NULL;
  if (!fl) return;
  if (fl->kind == FORM_LIST && fl->text && *fl->text) {
    char *t = str_printf("%s, %s", fl->text, name);
    free(fl->text);
    fl->text = t;
  } else {
    free(fl->text);
    fl->text = str_dup(name);
  }
}

static bool picker_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  FormPicker *pk = o->data;
  ListAction a = ev->type == UI_EVENT_MOUSE ? list_mouse(&pk->list, ev, 0, o->rows) : list_key(&pk->list, ev);
  if (a == LIST_CANCEL) {
    overlay_pop_this(app, o);
  } else if (a == LIST_ACTIVATE) {
    ListRow *r = list_selected(&pk->list);
    char *name = r ? str_dup(r->text) : NULL;
    if (!name && pk->list.has_input && pk->list.input.len) name = str_dup(pk->list.input.buf); /* typed */
    FormPicker copy = *pk; /* pk is freed with o */
    overlay_pop_this(app, o);
    if (name) picker_apply(app, &copy, name);
    free(name);
  }
  return true;
}

static void picker_draw(Overlay *o, NcApp *app) {
  FormPicker *pk = o->data;
  list_draw(&pk->list, o->plane, &app->theme, 1, 1, o->rows, o->cols, NULL, NULL);
}

static void picker_layout(Overlay *o, NcApp *app) {
  FormPicker *pk = o->data;
  o->cols = app->cols - 12 < 44 ? app->cols - 12 : 44;
  if (o->cols < 20) o->cols = 20;
  o->rows = (int)pk->list.num_rows + 1;
  if (o->rows > app->rows - 8) o->rows = app->rows - 8;
  if (o->rows < 2) o->rows = 2;
}

static void picker_free(void *data) {
  FormPicker *pk = data;
  list_free(&pk->list);
  free(pk);
}

static void open_picker(NcApp *app, Form *f) {
  FormField *fl = &f->fields[f->cur];
  if (!fl->num_options) {
    form_set_message(f, fl->free_text ? "No list to pick from: type the value" : "The list is still loading", false);
    return;
  }
  FormPicker *pk = safe_calloc(1, sizeof *pk);
  pk->form_uid = f->uid;
  pk->field_id = fl->id;
  list_init(&pk->list, true);
  ListRow *rows = safe_calloc(fl->num_options, sizeof *rows);
  size_t at = 0;
  for (size_t i = 0; i < fl->num_options; i++) {
    rows[i] = (ListRow){str_dup(fl->options[i]), NULL, NULL, (int)i, NULL, false};
    if (fl->text && fl->kind == FORM_PICK && !strcmp(fl->text, fl->options[i])) at = i;
  }
  list_set_rows(&pk->list, rows, fl->num_options);
  pk->list.cursor = at;
  Overlay *o = overlay_new(fl->label);
  o->footer = "Enter pick  type to narrow  Esc cancel";
  o->data = pk;
  o->free_data = picker_free;
  o->draw = picker_draw;
  o->key = picker_key;
  o->layout = picker_layout;
  overlay_push(app, o);
}

static void activate(NcApp *app, Form *f) {
  FormField *fl = &f->fields[f->cur];
  if (fl->disabled) {
    form_set_message(f, fl->hint, false);
    return;
  }
  switch (fl->kind) {
  case FORM_TOGGLE: fl->on = !fl->on; break;
  case FORM_TEXT: begin_edit(f, NULL); break;
  case FORM_PICK:
  case FORM_LIST: open_picker(app, f); break;
  }
}

static bool form_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  Form *f = o->data;
  if (ev->type == UI_EVENT_MOUSE) {
    if (ev->mouse.action == UI_MOUSE_PRESS && ev->mouse.button == UI_MOUSE_LEFT && ev->mouse.y >= 0 &&
        ev->mouse.y < o->rows - 1) {
      size_t k = f->scroll + (size_t)ev->mouse.y;
      if (k < f->n && !f->busy) {
        end_edit(f, true);
        bool again = k == f->cur;
        f->cur = k;
        if (again) activate(app, f);
      }
    }
    return true;
  }
  if (ev->type != UI_EVENT_KEY) return true;
  if (f->busy) return !nc_key_special(ev, UI_KEY_ESCAPE);
  if (f->editing && nc_key_special(ev, UI_KEY_ESCAPE)) {
    end_edit(f, false);
    return true;
  }
  if (nc_key_special(ev, UI_KEY_ESCAPE)) return false;
  if (hotkey_matches(app->config, ev, HOTKEY_ROW_SAVE) || nc_key_ctrl(ev, 'S')) {
    end_edit(f, true);
    if (f->submit) f->submit(app, f, f->data);
    return true;
  }
  if (nc_key_special(ev, UI_KEY_UP) || nc_key_special(ev, UI_KEY_BTAB)) { move(f, -1); return true; }
  if (nc_key_special(ev, UI_KEY_DOWN) || nc_key_special(ev, UI_KEY_TAB)) { move(f, 1); return true; }
  if (f->editing) {
    if (nc_key_special(ev, UI_KEY_ENTER)) { end_edit(f, true); return true; }
    if (nc_key_ctrl(ev, 'U')) {
      const char *clip = app_clipboard_get(app);
      if (clip && *clip) text_input_insert(&f->in, clip);
      return true;
    }
    nc_text_input_key(&f->in, ev);
    return true;
  }
  if (nc_key_special(ev, UI_KEY_ENTER)) { activate(app, f); return true; }
  if (nc_key_special(ev, UI_KEY_PAGEUP) || nc_key_special(ev, UI_KEY_HOME)) { f->cur = 0; return true; }
  if (nc_key_special(ev, UI_KEY_PAGEDOWN) || nc_key_special(ev, UI_KEY_END)) { f->cur = f->n ? f->n - 1 : 0; return true; }
  FormField *fl = &f->fields[f->cur];
  if (nc_key_special(ev, UI_KEY_DELETE) || nc_key_special(ev, UI_KEY_BACKSPACE)) {
    if (!fl->disabled && fl->kind != FORM_TOGGLE) {
      free(fl->text);
      fl->text = NULL;
    }
    return true;
  }
  char buf[8];
  const char *typed = nc_key_typed(ev, buf);
  if (!typed) return true;
  if (fl->kind == FORM_TOGGLE && !fl->disabled && !strcmp(typed, " ")) {
    fl->on = !fl->on;
    return true;
  }
  if (fl->kind == FORM_TOGGLE || fl->disabled) return true;
  if (fl->kind == FORM_PICK && !fl->free_text) {
    open_picker(app, f);
    return true;
  }
  begin_edit(f, typed);
  return true;
}

static void form_free_cb(void *data) {
  Form *f = data;
  if (f->editing) text_input_free(&f->in);
  for (size_t i = 0; i < f->n; i++) field_free(&f->fields[i]);
  free(f->fields);
  free(f->message);
  if (f->free_data) f->free_data(f->data);
  free(f);
}

Form *form_open(NcApp *app, const char *title, FormFields *ff, FormSubmitFn submit, void *data,
                void (*free_data)(void *)) {
  Form *f = safe_calloc(1, sizeof *f);
  f->uid = ++g_form_uid;
  f->fields = ff->v;
  f->n = ff->n;
  memset(ff, 0, sizeof *ff);
  f->submit = submit;
  f->data = data;
  f->free_data = free_data;
  for (size_t i = 0; i < f->n; i++) {
    int w = nc_text_width(f->fields[i].label);
    if (w > f->label_w) f->label_w = w;
  }
  if (f->label_w > 18) f->label_w = 18;
  Overlay *o = overlay_new(title);
  o->footer = "\xe2\x86\x91\xe2\x86\x93 field  Enter edit/pick  F2 save  Esc cancel";
  o->data = f;
  o->free_data = form_free_cb;
  o->layout = form_layout;
  o->draw = form_draw;
  o->key = form_key;
  f->overlay = o;
  overlay_push(app, o);
  return f;
}

Form *form_find(NcApp *app, uint64_t uid) {
  for (size_t i = 0; i < app->num_overlays; i++) {
    Overlay *o = app->overlays[i];
    if (o->free_data == form_free_cb && ((Form *)o->data)->uid == uid) return o->data;
  }
  return NULL;
}

void form_close(NcApp *app, Form *f) {
  if (f && f->overlay) overlay_pop_this(app, f->overlay);
}
