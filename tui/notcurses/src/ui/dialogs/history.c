#include "edit_ops.h"
#include "model/workspace.h"
#include "ui/dialogs/confirm.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "ui/panels.h"
#include "ui/textbuf.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void fill(ListWidget *l, NcConnection *conn) {
  LaceHistory *h = nc_connection_history(conn);
  size_t n = h ? lace_history_count(h) : 0;
  if (!n) {
    ListRow *rows = safe_calloc(1, sizeof *rows);
    rows[0] = (ListRow){str_dup("No history yet"), NULL, NULL, -1, NULL, true};
    list_set_rows(l, rows, 1);
    return;
  }
  ListRow *rows = safe_calloc(n, sizeof *rows);
  for (size_t k = 0; k < n; k++) { /* newest first */
    size_t i = n - 1 - k;
    const LaceHistoryEntry *e = lace_history_get(h, i);
    char *flat = str_dup(e->sql ? e->sql : "");
    for (char *c = flat; *c; c++)
      if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    char when[16] = "";
    struct tm tm;
    time_t ts = e->timestamp;
    if (localtime_r(&ts, &tm)) strftime(when, sizeof when, "%H:%M:%S", &tm);
    rows[k].text = flat;
    rows[k].detail = str_printf("%s  %s", lace_history_type_tag(e->type), when);
    rows[k].kind = (int)i;
  }
  list_set_rows(l, rows, n);
}

/* The statement goes in at the editor's cursor of the current query view */
static bool insert_into_editor(NcApp *app, const char *sql) {
  View *v = app_current_view(app);
  if (!v || v->kind != VIEW_QUERY) return false;
  if (!v->text) v->text = str_dup("");
  TextBuf tb;
  textbuf_adopt(&tb, v->text, v->text_cursor, v->text_want_col);
  textbuf_insert(&tb, sql, strlen(sql));
  v->text = tb.text;
  v->text_cursor = tb.cursor;
  v->text_want_col = tb.want_col;
  v->text_sel_anchor = SIZE_MAX;
  v->focus_results = false;
  return true;
}

static void history_draw(Overlay *o, NcApp *app);

static void clear_done(NcApp *app, bool yes, void *data) {
  (void)data;
  ConnTab *t = app_current_tab(app);
  if (!yes || !t || !t->conn) return;
  nc_connection_history_clear(t->conn);
  for (size_t i = 0; i < app->num_overlays; i++) /* the history overlay, if it is still open */
    if (app->overlays[i]->draw == history_draw) fill(app->overlays[i]->data, t->conn);
  app_set_status(app, false, "History cleared");
}

static bool history_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  ListWidget *l = o->data;
  ConnTab *t = app_current_tab(app);
  if (!t || !t->conn) {
    overlay_pop_this(app, o);
    return true;
  }
  if (ev->type == UI_EVENT_KEY && hotkey_matches(app->config, ev, HOTKEY_TOGGLE_HISTORY)) {
    overlay_pop_this(app, o);
    return true;
  }
  if (ev->type == UI_EVENT_KEY && !l->input_focused) { /* the shared history keys (both TUIs) */
    if (hotkey_matches(app->config, ev, HOTKEY_HISTORY_DELETE)) {
      ListRow *r = list_selected(l);
      if (r) {
        size_t cur = l->cursor;
        nc_connection_history_remove(t->conn, (size_t)r->kind);
        fill(l, t->conn);
        if (l->num_shown) l->cursor = cur < l->num_shown ? cur : l->num_shown - 1;
      }
      return true;
    }
    if (hotkey_matches(app->config, ev, HOTKEY_HISTORY_CLEAR)) {
      confirm_open(app, "Clear history", "Remove every entry of this connection's history?", "Clear", true,
                   clear_done, NULL);
      return true;
    }
    if (hotkey_matches(app->config, ev, HOTKEY_HISTORY_CLOSE)) {
      overlay_pop_this(app, o);
      return true;
    }
  }
  ListAction a = ev->type == UI_EVENT_MOUSE ? list_mouse(l, ev, 0, o->rows) : list_key(l, ev);
  if (ev->type == UI_EVENT_KEY && !l->input_focused && hotkey_matches(app->config, ev, HOTKEY_HISTORY_COPY))
    a = LIST_ACTIVATE;
  if (a == LIST_CANCEL) {
    overlay_pop_this(app, o);
  } else if (a == LIST_ACTIVATE) {
    ListRow *r = list_selected(l);
    const LaceHistoryEntry *e = r ? lace_history_get(nc_connection_history(t->conn), (size_t)r->kind) : NULL;
    if (e && e->sql) {
      char *sql = str_dup(e->sql);
      app_clipboard_set(app, sql);
      overlay_pop_this(app, o);
      app_set_status(app, false, insert_into_editor(app, sql) ? "Inserted into the editor" : "Copied");
      str_secure_free(sql);
    }
  }
  return true;
}

static void history_draw(Overlay *o, NcApp *app) {
  list_draw(o->data, o->plane, &app->theme, 1, 1, o->rows, o->cols, NULL, NULL);
}

static void history_layout(Overlay *o, NcApp *app) {
  o->cols = app->cols - 8 < 110 ? app->cols - 8 : 110;
  o->rows = app->rows - 6 < 24 ? app->rows - 6 : 24;
}

static void history_free(void *data) {
  list_free(data);
  free(data);
}

void history_open(NcApp *app) {
  ConnTab *t = app_current_tab(app);
  if (!t || !t->conn) return;
  ListWidget *l = safe_calloc(1, sizeof *l);
  list_init(l, false);
  fill(l, t->conn);
  Overlay *o = overlay_new("History");
  o->footer = "Enter copy/insert  x remove  c clear  Esc close";
  o->data = l;
  o->free_data = history_free;
  o->draw = history_draw;
  o->key = history_key;
  o->layout = history_layout;
  overlay_push(app, o);
}
