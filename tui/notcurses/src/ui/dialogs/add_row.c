/* The add-row form: one line per column. A column starts as a placeholder (AI,
 * GENERATED, its default, required) that is left out of the INSERT so the
 * database fills it in, or as NULL; typing, Ctrl+N and Ctrl+D make it part of
 * the INSERT. Clearing a field restores its initial state, except for a
 * required column, where a confirmed empty value means ''. */
#include "core/utf8.h"
#include "edit_ops.h"
#include "model/workspace.h"
#include "ui/grid.h"
#include "ui/input.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

enum { F_PLACEHOLDER, F_NULL, F_TEXT };

typedef struct {
  char *name;
  LaceValueType type;
  bool required;
  char *label;      /* the placeholder text */
  int state;
  char *text;       /* F_TEXT */
  int init_state;
} Field;

typedef struct {
  NcApp *app;
  uint64_t uid;
  int refs;          /* the form and the job in flight */
  Overlay *form;     /* NULL once the form is closed */
} AddReq;

typedef struct {
  AddReq *req;
  char *table;
  Field *f;
  size_t n, cur, scroll;
  int name_w;
  bool editing, busy;
  TextInput in;
  char *message;
} AddRow;

static void req_unref(void *p) {
  AddReq *r = p;
  if (--r->refs == 0) free(r);
}

static void field_reset(Field *f) {
  str_secure_free(f->text);
  f->text = NULL;
  f->state = f->init_state;
}

static void field_init(Field *f, const LaceColumn *c) {
  f->name = str_dup(c->name ? c->name : "");
  f->type = c->type;
  if (c->auto_increment) f->label = str_dup("AI");
  else if (c->generated) f->label = str_dup("GENERATED");
  else if (c->default_val && c->default_val[0]) f->label = str_dup(c->default_val);
  else if (!c->nullable) { f->label = str_dup("required"); f->required = true; }
  bool ph = f->label != NULL;
  f->init_state = ph ? F_PLACEHOLDER : F_NULL;
  f->state = f->init_state;
}

static void ar_layout(Overlay *o, NcApp *app) {
  AddRow *a = o->data;
  o->cols = app->cols - 8 < 70 ? app->cols - 8 : 70;
  if (o->cols < 30) o->cols = 30;
  o->rows = (int)a->n + 1;
  if (o->rows > app->rows - 6) o->rows = app->rows - 6;
  if (o->rows < 2) o->rows = 2;
}

static int vis_fields(const Overlay *o) { return o->rows - 1; }

static void ar_draw(Overlay *o, NcApp *app) {
  AddRow *a = o->data;
  struct ncplane *p = o->plane;
  const Theme *t = &app->theme;
  int vis = vis_fields(o);
  if (a->cur < a->scroll) a->scroll = a->cur;
  if (a->cur >= a->scroll + (size_t)vis) a->scroll = a->cur - (size_t)vis + 1;
  int vx = 2 + a->name_w + 2, vw = o->cols - vx;
  o->wants_cursor = false;
  for (int i = 0; i < vis; i++) {
    size_t k = a->scroll + (size_t)i;
    if (k >= a->n) break;
    const Field *f = &a->f[k];
    int y = 1 + i;
    bool cur = k == a->cur;
    ThemeColor bg = cur ? TC_CURSOR_BG : TC_OVERLAY_BG;
    nc_style(p, t, cur ? TC_CURSOR_FG : TC_OVERLAY_FG, bg, cur ? NCSTYLE_BOLD : 0);
    nc_put(p, y, 1, o->cols, "");
    nc_put(p, y, 2, a->name_w, f->name);
    if (cur && a->editing) {
      int cx = nc_text_input_draw(p, t, &a->in, y, vx, vw, true);
      o->wants_cursor = true;
      o->cursor_y = i;
      o->cursor_x = vx - 1 + cx;
      continue;
    }
    char *shown = NULL;
    ThemeColor fg = TC_OVERLAY_FG;
    if (f->state == F_PLACEHOLDER) { shown = str_dup(f->label); fg = TC_DIM; }
    else if (f->state == F_NULL) { shown = str_dup("NULL"); fg = TC_NULL_FG; }
    else {
      shown = str_dup(f->text ? f->text : "");
      for (char *c = shown; *c; c++)
        if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    }
    nc_style(p, t, cur ? TC_CURSOR_FG : fg, bg, 0);
    nc_put(p, y, vx, vw, shown);
    str_secure_free(shown);
  }
  nc_style(p, t, TC_ERROR_FG, TC_OVERLAY_BG, 0);
  nc_put(p, o->rows, 2, o->cols - 2, a->busy ? "Inserting\xe2\x80\xa6" : (a->message ? a->message : ""));
}

static void begin_edit(AddRow *a, const char *typed) {
  Field *f = &a->f[a->cur];
  text_input_init(&a->in, f->state == F_TEXT && f->text ? f->text : "", false);
  if (typed) text_input_insert(&a->in, typed);
  a->editing = true;
}

static void end_edit(AddRow *a, bool commit) {
  if (!a->editing) return;
  Field *f = &a->f[a->cur];
  if (commit) {
    size_t cur;
    char *text = text_input_release(&a->in, &cur);
    if (!text) text = str_dup("");
    if (text[0] || f->required) {
      str_secure_free(f->text);
      f->text = text;
      f->state = F_TEXT;
    } else {
      free(text);
      field_reset(f);
    }
  } else {
    text_input_free(&a->in);
  }
  a->editing = false;
}

static void set_field(AddRow *a, int state, const char *text) {
  end_edit(a, false);
  Field *f = &a->f[a->cur];
  str_secure_free(f->text);
  f->text = state == F_TEXT ? str_dup(text) : NULL;
  f->state = state;
}

static void on_insert(NcJob *job, void *data) {
  AddReq *r = data;
  NcApp *app = r->app;
  app_log_job(app, job, "insert");
  AddRow *a = r->form ? r->form->data : NULL;
  if (a) a->busy = false;
  if (job->error) {
    app_set_status(app, true, "Not inserted: %s", job->error);
    if (a) {
      free(a->message);
      a->message = str_dup(job->error);
    }
    return;
  }
  app_set_status(app, false, "Inserted 1 row");
  View *v = app_find_view(app, r->uid);
  if (v) app_view_reload(app, v);
  if (r->form) overlay_pop_this(app, r->form);
}

static void submit(Overlay *o, NcApp *app) {
  AddRow *a = o->data;
  if (a->busy) return;
  end_edit(a, true);
  View *v = app_find_view(app, a->req->uid);
  ConnTab *t = v ? app_view_tab(app, v) : NULL;
  if (!t || !t->conn || nc_connection_closed(t->conn)) {
    app_set_status(app, true, "Not connected");
    return;
  }
  const char **cols = safe_calloc(a->n + 1, sizeof *cols);
  LaceValue *vals = safe_calloc(a->n + 1, sizeof *vals);
  size_t n = 0;
  for (size_t i = 0; i < a->n; i++) {
    Field *f = &a->f[i];
    if (f->state == F_PLACEHOLDER) continue;
    LaceValue val;
    if (f->state == F_NULL) {
      val = lace_value_null();
    } else {
      char *err = NULL;
      if (!lace_value_from_edit_text(f->text ? f->text : "", f->type, &val, &err)) {
        free(a->message);
        a->message = str_printf("%s: %s", f->name, err ? err : "invalid value");
        app_set_status(app, true, "Not inserted: %s", a->message);
        free(err);
        for (size_t k = 0; k < n; k++) lace_value_free(&vals[k]);
        free(cols);
        free(vals);
        return;
      }
    }
    cols[n] = f->name;
    vals[n++] = val;
  }
  NcJob *job = nc_job_insert(a->table, cols, vals, n);
  for (size_t k = 0; k < n; k++) lace_value_free(&vals[k]);
  free(cols);
  free(vals);
  free(a->message);
  a->message = NULL;
  a->busy = true;
  a->req->refs++;
  nc_connection_submit_full(t->conn, job, t->owner, on_insert, a->req, req_unref);
}

static void move(AddRow *a, int dir) {
  end_edit(a, true);
  if (dir < 0 && a->cur > 0) a->cur--;
  else if (dir > 0 && a->cur + 1 < a->n) a->cur++;
}

static bool ar_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  AddRow *a = o->data;
  if (ev->type != UI_EVENT_KEY) return true;
  if (a->busy) return !nc_key_special(ev, UI_KEY_ESCAPE);
  if (a->editing && nc_key_special(ev, UI_KEY_ESCAPE)) {
    end_edit(a, false);
    return true;
  }
  if (nc_key_special(ev, UI_KEY_ESCAPE)) return false;
  if (hotkey_matches(app->config, ev, HOTKEY_ROW_SAVE)) {
    submit(o, app);
    return true;
  }
  if (hotkey_matches(app->config, ev, HOTKEY_EDITOR_NULL)) {
    set_field(a, F_NULL, NULL);
    return true;
  }
  if (hotkey_matches(app->config, ev, HOTKEY_EDITOR_EMPTY)) {
    set_field(a, F_TEXT, "");
    return true;
  }
  if (nc_key_special(ev, UI_KEY_UP) || nc_key_special(ev, UI_KEY_BTAB)) { move(a, -1); return true; }
  if (nc_key_special(ev, UI_KEY_DOWN) || nc_key_special(ev, UI_KEY_TAB)) { move(a, 1); return true; }
  if (a->editing) {
    if (nc_key_special(ev, UI_KEY_ENTER)) { end_edit(a, true); return true; }
    if (nc_key_ctrl(ev, 'K')) {
      if (a->in.len) {
        app_clipboard_set(app, a->in.buf);
        app_set_status(app, false, "Copied to clipboard");
      }
      return true;
    }
    if (nc_key_ctrl(ev, 'U')) {
      const char *clip = app_clipboard_get(app);
      if (clip && *clip) text_input_insert(&a->in, clip);
      return true;
    }
    nc_text_input_key(&a->in, ev);
    return true;
  }
  if (nc_key_special(ev, UI_KEY_ENTER)) { begin_edit(a, NULL); return true; }
  if (nc_key_special(ev, UI_KEY_DELETE) || nc_key_special(ev, UI_KEY_BACKSPACE)) {
    field_reset(&a->f[a->cur]);
    return true;
  }
  if (nc_key_special(ev, UI_KEY_PAGEUP)) { a->cur = a->cur > 5 ? a->cur - 5 : 0; return true; }
  if (nc_key_special(ev, UI_KEY_PAGEDOWN)) { a->cur = a->cur + 5 < a->n ? a->cur + 5 : a->n - 1; return true; }
  if (nc_key_special(ev, UI_KEY_HOME)) { a->cur = 0; return true; }
  if (nc_key_special(ev, UI_KEY_END)) { a->cur = a->n - 1; return true; }
  char buf[8];
  const char *typed = nc_key_typed(ev, buf);
  if (typed) begin_edit(a, typed);
  return true;
}

static void ar_closed(Overlay *o, NcApp *app) {
  (void)app;
  AddRow *a = o->data;
  a->req->form = NULL;
}

static void ar_free(void *data) {
  AddRow *a = data;
  if (a->editing) text_input_free(&a->in);
  for (size_t i = 0; i < a->n; i++) {
    free(a->f[i].name);
    free(a->f[i].label);
    str_secure_free(a->f[i].text);
  }
  free(a->f);
  free(a->table);
  free(a->message);
  req_unref(a->req);
  free(a);
}

void add_row_open(NcApp *app, View *v) {
  const char *table = v->kind == VIEW_TABLE ? v->table : v->source_table;
  if (!v->schema || !table || !v->schema->num_columns) {
    app_set_status(app, true, "The table is still loading");
    return;
  }
  AddRow *a = safe_calloc(1, sizeof *a);
  a->req = safe_calloc(1, sizeof *a->req);
  a->req->app = app;
  a->req->uid = v->uid;
  a->req->refs = 1;
  a->table = str_dup(table);
  a->n = v->schema->num_columns;
  a->f = safe_calloc(a->n, sizeof *a->f);
  for (size_t i = 0; i < a->n; i++) {
    field_init(&a->f[i], &v->schema->columns[i]);
    int w = nc_text_width(a->f[i].name);
    if (w > a->name_w) a->name_w = w;
  }
  if (a->name_w > 20) a->name_w = 20;
  Overlay *o = overlay_new("Add row");
  o->footer = "\xe2\x86\x91\xe2\x86\x93 field  Enter edit  ^N NULL  ^D empty  F2 save  Esc cancel";
  o->data = a;
  o->free_data = ar_free;
  o->layout = ar_layout;
  o->draw = ar_draw;
  o->key = ar_key;
  o->closed = ar_closed;
  a->req->form = o;
  overlay_push(app, o);
}
