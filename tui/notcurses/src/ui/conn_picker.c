#include "ui/conn_picker.h"
#include "core/connstr_display.h"
#include "core/utf8.h"
#include "ui/dialogs/confirm.h"
#include "ui/dialogs/connect.h"
#include "ui/input.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <ctype.h>
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_version = 1;
void cp_changed(void) { g_version++; }

void nc_draw_input(struct ncplane *pl, const Theme *t, int y, int x, int w, const TextInput *in,
                   ThemeColor fg, ThemeColor bg, int *cursor_x) {
  nc_style(pl, t, fg, bg, 0);
  nc_put(pl, y, x, w, "");
  const char *buf = in->buf ? in->buf : "";
  size_t len = in->len, cur = in->cursor < len ? in->cursor : len;
  char *disp;
  size_t dcur;
  if (in->secret) {
    size_t n = utf8_count(buf, len);
    disp = safe_malloc(n + 1);
    memset(disp, '*', n);
    disp[n] = 0;
    dcur = utf8_count(buf, cur);
  } else {
    disp = str_dup(buf);
    dcur = cur;
  }
  size_t start = 0, dl = strlen(disp);
  while (start < dcur && utf8_width(disp + start, dcur - start) > w - 1) start = utf8_next(disp, dl, start);
  nc_put(pl, y, x, w, disp + start);
  if (cursor_x) *cursor_x = x + utf8_width(disp + start, dcur - start);
  free(disp);
}

/* ---- rows ---- */

typedef struct { ListRow *rows; size_t n, cap; } Rows;

static void add(Rows *r, const char *text, const char *detail, int kind, size_t idx, bool header) {
  if (r->n == r->cap) {
    r->cap = r->cap ? r->cap * 2 : 32;
    r->rows = safe_realloc(r->rows, r->cap * sizeof *r->rows);
  }
  r->rows[r->n++] = (ListRow){str_dup(text), detail ? str_dup(detail) : NULL, NULL, kind,
                              (void *)(intptr_t)idx, header};
}

static bool contains_ci(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  if (!n) return true;
  for (; *hay; hay++) {
    size_t i = 0;
    while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
    if (i == n) return true;
  }
  return false;
}

static void restore(ConnPicker *p, int kind, size_t idx) {
  ListWidget *l = &p->list;
  for (size_t i = 0; i < l->num_shown; i++) {
    const ListRow *r = &l->rows[l->shown[i]];
    if (!r->header && r->kind == kind && (size_t)(intptr_t)r->data == idx) { l->cursor = i; return; }
  }
}

void cp_rebuild(NcApp *app, ConnPicker *p) {
  int keep_kind = -1;
  size_t keep_idx = 0;
  ListRow *sel = list_selected(&p->list);
  if (sel) { keep_kind = sel->kind; keep_idx = (size_t)(intptr_t)sel->data; }
  const char *filter = p->in.buf ? p->in.buf : "";
  Rows r = {0};
  size_t n = app->connmgr ? connmgr_count_filtered(app->connmgr, filter) : 0;
  if (n) add(&r, "Saved connections", NULL, 0, 0, true);
  for (size_t i = 0; i < n; i++) {
    ConnectionItem *it = connmgr_get_filtered_item(app->connmgr, filter, i);
    if (!it) continue;
    int depth = connmgr_get_item_depth(it);
    int indent = depth > 1 ? (depth - 1) * 2 : 0;
    if (it->type == CONN_ITEM_FOLDER) {
      char *text = str_printf("%*s%s %s", indent, "", it->folder.expanded ? "\xe2\x96\xbe" : "\xe2\x96\xb8",
                              connmgr_item_name(it));
      add(&r, text, NULL, CPK_FOLDER, i, false);
      free(text);
    } else {
      char *text = str_printf("%*s  %s", indent, "", connmgr_item_name(it));
      char *cs = connmgr_build_connstr(&it->connection);
      char *detail = cs ? connstr_display(cs) : NULL;
      add(&r, text, detail, CPK_CONN, i, false);
      free(text);
      free(detail);
      str_secure_free(cs);
    }
  }
  bool header = false;
  for (size_t i = 0; i < app->recent.count; i++) {
    char *d = connstr_display(app->recent.items[i].url);
    const char *label = d ? d : app->recent.items[i].url;
    if (contains_ci(label, filter) || contains_ci(app->recent.items[i].url, filter)) {
      if (!header) { add(&r, "Recent", NULL, 0, 0, true); header = true; }
      char *text = str_printf("  %s", label);
      add(&r, text, NULL, CPK_RECENT, i, false);
      free(text);
    }
    free(d);
  }
  list_set_rows(&p->list, r.rows, r.n);
  p->seen = g_version;
  if (keep_kind > 0) restore(p, keep_kind, keep_idx);
}

void cp_init(NcApp *app, ConnPicker *p, bool input_focused) {
  memset(p, 0, sizeof *p);
  text_input_init(&p->in, NULL, false);
  list_init(&p->list, false);
  p->input_focused = input_focused;
  cp_rebuild(app, p);
}

void cp_free(ConnPicker *p) {
  list_free(&p->list);
  text_input_free(&p->in);
}

/* ---- keys ---- */

static bool ctrl(const UiEvent *ev, int letter) {
  return !ev->key.is_special && (ev->key.mods & UI_MOD_CTRL) && toupper(ev->key.key) == letter;
}

CpResult cp_key(NcApp *app, ConnPicker *p, const UiEvent *ev, bool picker_mode) {
  CpResult res = {false, CP_NONE};
  if (ev->type != UI_EVENT_KEY) return res;
  const char *text = p->in.buf ? p->in.buf : "";
  ListRow *sel = list_selected(&p->list);
  if (ev->key.is_special && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
    switch (ev->key.key) {
    case UI_KEY_ESCAPE:
      if (picker_mode) return res;
      res.consumed = true;
      if (*text) { text_input_set(&p->in, ""); cp_rebuild(app, p); }
      p->input_focused = false; /* even with nothing to pick: global keys (q) work again */
      return res;
    case UI_KEY_TAB:
    case UI_KEY_BTAB:
      res.consumed = true;
      if (p->input_focused) { if (sel) p->input_focused = false; }
      else p->input_focused = true;
      return res;
    case UI_KEY_DOWN:
      res.consumed = true;
      if (p->input_focused) { if (sel) p->input_focused = false; }
      else if (list_key(&p->list, ev) == LIST_NONE && !sel) p->input_focused = true;
      return res;
    case UI_KEY_UP:
      res.consumed = true;
      if (p->input_focused) return res;
      if (list_key(&p->list, ev) == LIST_NONE) p->input_focused = true;
      return res;
    case UI_KEY_PAGEUP:
    case UI_KEY_PAGEDOWN:
      res.consumed = true;
      if (!p->input_focused) list_key(&p->list, ev);
      return res;
    case UI_KEY_ENTER:
      res.consumed = true;
      if (p->input_focused && *text) res.act = CP_CONNECT_TEXT;
      else if (sel) res.act = CP_ROW;
      return res;
    case UI_KEY_HOME:
    case UI_KEY_END:
      if (!p->input_focused) { res.consumed = true; list_key(&p->list, ev); return res; }
      break;
    default: break;
    }
  }
  if (ctrl(ev, 'S')) { /* save the typed URL, or the recent one under the cursor */
    res.consumed = true;
    if (p->input_focused && *text) res.act = CP_SAVE;
    else if (sel && sel->kind == CPK_RECENT) res.act = CP_SAVE;
    return res;
  }
  if (p->input_focused) {
    char *before = str_dup(text);
    res.consumed = nc_text_input_key(&p->in, ev);
    if (strcmp(before, p->in.buf ? p->in.buf : "") != 0) cp_rebuild(app, p);
    str_secure_free(before);
    return res;
  }
  if (ev->key.is_special || (ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) return res;
  switch (ev->key.key) {
  case '/': res.consumed = true; p->input_focused = true; break;
  case 'n': res.consumed = true; res.act = CP_NEW; break;
  case 'e': if (sel && sel->kind == CPK_CONN) { res.consumed = true; res.act = CP_EDIT; } break;
  case 'x':
    if (sel && sel->kind == CPK_CONN) { res.consumed = true; res.act = CP_DELETE; }
    else if (sel && sel->kind == CPK_RECENT) { res.consumed = true; res.act = CP_FORGET; }
    break;
  case 's': if (sel && sel->kind == CPK_RECENT) { res.consumed = true; res.act = CP_SAVE; } break;
  case 't': if (sel && sel->kind != CPK_FOLDER) { res.consumed = true; res.act = CP_ROW_TAB; } break;
  case 'w':
    if (picker_mode && sel && sel->kind != CPK_FOLDER) { res.consumed = true; res.act = CP_ROW_WORKSPACE; }
    break;
  case ' ': if (sel && sel->kind == CPK_FOLDER) { res.consumed = true; res.act = CP_ROW; } break;
  default: break;
  }
  return res;
}

/* ---- actions ---- */

static void delete_confirmed(NcApp *app, bool yes, void *data) {
  char *id = data;
  if (yes && app->connmgr) {
    ConnectionItem *it = connmgr_find_by_id(app->connmgr, id);
    if (it) {
      char *err = NULL;
      connmgr_remove_item(app->connmgr, it);
      if (!connmgr_save(app->connmgr, &err)) app_set_status(app, true, "connections: %s", err ? err : "cannot save");
      free(err);
      cp_changed();
    }
  }
  free(id);
}

void cp_perform(NcApp *app, ConnPicker *p, CpAction act, Overlay *o) {
  const char *text = p->in.buf ? p->in.buf : "";
  ListRow *sel = list_selected(&p->list);
  int kind = sel ? sel->kind : 0;
  size_t idx = sel ? (size_t)(intptr_t)sel->data : 0;
  ConnectionItem *it = kind == CPK_CONN || kind == CPK_FOLDER
                           ? connmgr_get_filtered_item(app->connmgr, text, idx) : NULL;
  bool as_tab = act != CP_ROW_WORKSPACE;
  char *err = NULL;
  switch (act) {
  case CP_CONNECT_TEXT: {
    char *cs = app_target_to_connstr(text, &err);
    if (!cs) { app_set_status(app, true, "%s", err ? err : "Not a connection"); free(err); return; }
    if (o) overlay_pop_this(app, o);
    else { /* the welcome screen: the text is spent, the list shows again */
      text_input_set(&p->in, "");
      cp_rebuild(app, p);
    }
    app_connect_url(app, cs, NULL, NULL, true, NULL, NULL);
    str_secure_free(cs);
    return;
  }
  case CP_ROW:
  case CP_ROW_TAB:
  case CP_ROW_WORKSPACE:
    if (kind == CPK_FOLDER && it) {
      connmgr_toggle_folder(it);
      cp_rebuild(app, p);
    } else if (kind == CPK_CONN && it) {
      char *id = str_dup(it->connection.id);
      if (o) overlay_pop_this(app, o);
      app_connect_saved(app, id, as_tab, NULL, NULL);
      free(id);
    } else if (kind == CPK_RECENT && idx < app->recent.count) {
      char *url = str_dup(app->recent.items[idx].url);
      if (o) overlay_pop_this(app, o);
      app_connect_url(app, url, NULL, NULL, as_tab, NULL, NULL);
      str_secure_free(url);
    }
    return;
  case CP_EDIT:
    if (it && kind == CPK_CONN) connect_form_open(app, lace_saved_conn_copy(&it->connection));
    return;
  case CP_NEW: connect_form_open(app, NULL); return;
  case CP_DELETE:
    if (it && kind == CPK_CONN) {
      char *msg = str_printf("Delete the saved connection \"%s\"?", connmgr_item_name(it));
      confirm_open(app, "Delete connection", msg, "Delete", true, delete_confirmed,
                   str_dup(it->connection.id));
      free(msg);
    }
    return;
  case CP_FORGET:
    if (kind == CPK_RECENT && idx < app->recent.count) {
      recent_remove(&app->recent, idx);
      recent_save(&app->recent, NULL);
      cp_rebuild(app, p);
    }
    return;
  case CP_SAVE: {
    char *url = NULL;
    if (p->input_focused && *text) url = app_target_to_connstr(text, &err);
    else if (kind == CPK_RECENT && idx < app->recent.count) url = str_dup(app->recent.items[idx].url);
    LaceSavedConnection *m = url ? lace_saved_conn_from_connstr(url, &err) : NULL;
    if (!m) app_set_status(app, true, "%s", err ? err : "Not a connection");
    else connect_form_open(app, m);
    str_secure_free(url);
    free(err);
    return;
  }
  default: return;
  }
}

/* ---- drawing ---- */

void cp_draw(NcApp *app, ConnPicker *p, struct ncplane *pl, int y, int x, int rows, int cols,
             int *cursor_y, int *cursor_x) {
  if (p->seen != g_version) cp_rebuild(app, p);
  const Theme *t = &app->theme;
  nc_style(pl, t, TC_ACCENT, TC_OVERLAY_BG, 0);
  nc_put(pl, y, x + 1, 2, "> ");
  int cx = x + 3;
  if (p->in.len == 0 && !p->input_focused) {
    nc_style(pl, t, TC_DIM, TC_EDIT_BG, 0);
    nc_put(pl, y, x + 3, cols - 4, "URL or file, or pick below");
  } else {
    nc_draw_input(pl, t, y, x + 3, cols - 4, &p->in, TC_OVERLAY_FG, TC_EDIT_BG, &cx);
  }
  if (cursor_y) *cursor_y = y;
  if (cursor_x) *cursor_x = cx;
  if (rows > 2) list_draw(&p->list, pl, t, y + 2, x, rows - 2, cols, NULL, NULL);
}
