#include "edit_ops.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "ui/panels.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>

typedef struct {
  ListWidget list;
  uint64_t seq; /* app->log_seq when the rows were built */
} LogPanel;

static void rebuild(LogPanel *p, NcApp *app) {
  bool at_end = !p->list.num_shown || p->list.cursor + 1 >= p->list.num_shown;
  size_t keep = p->list.cursor;
  ListRow *rows = safe_calloc(app->num_log ? app->num_log : 1, sizeof *rows);
  for (size_t i = 0; i < app->num_log; i++) {
    rows[i].text = str_dup(app->log[i].line);
    rows[i].kind = app->log[i].error;
    rows[i].data = NULL;
  }
  list_set_rows(&p->list, rows, app->num_log);
  if (app->num_log) p->list.cursor = at_end || keep >= app->num_log ? app->num_log - 1 : keep;
  p->seq = app->log_seq;
}

static bool log_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  LogPanel *p = o->data;
  if (ev->type == UI_EVENT_KEY && hotkey_matches(app->config, ev, HOTKEY_TOGGLE_LOG)) {
    overlay_pop_this(app, o);
    return true;
  }
  ListAction a = ev->type == UI_EVENT_MOUSE ? list_mouse(&p->list, ev, 0, o->rows) : list_key(&p->list, ev);
  if (a == LIST_CANCEL) {
    overlay_pop_this(app, o);
  } else if (a == LIST_ACTIVATE) {
    ListRow *r = list_selected(&p->list);
    if (r) {
      app_clipboard_set(app, r->text);
      app_set_status(app, false, "Copied the log line");
    }
  }
  return true;
}

static void log_draw(Overlay *o, NcApp *app) {
  LogPanel *p = o->data;
  if (p->seq != app->log_seq) rebuild(p, app);
  list_draw(&p->list, o->plane, &app->theme, 1, 1, o->rows, o->cols, NULL, NULL);
}

static void log_layout(Overlay *o, NcApp *app) {
  o->cols = app->cols - 6 < 130 ? app->cols - 6 : 130;
  o->rows = app->rows - 6;
}

static void log_free(void *data) {
  LogPanel *p = data;
  list_free(&p->list);
  free(p);
}

void log_open(NcApp *app) {
  LogPanel *p = safe_calloc(1, sizeof *p);
  list_init(&p->list, false);
  rebuild(p, app);
  Overlay *o = overlay_new("Log");
  o->footer = "Enter copy  Esc close  \xe2\x86\x91\xe2\x86\x93 scroll";
  o->data = p;
  o->free_data = log_free;
  o->draw = log_draw;
  o->key = log_key;
  o->layout = log_layout;
  overlay_push(app, o);
}
