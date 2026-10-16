#include "ui/palette.h"
#include "conn/connection.h"
#include "model/workspace.h"
#include "ui/dialogs/confirm.h"
#include "ui/dialogs/prompt.h"
#include "ui/grid.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "ui/panels.h"
#include "util/mem.h"
#include "util/str.h"
#include <ctype.h>
#include <notcurses/notcurses.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Length counts for a little, but never enough to cross a class (310/210/...) */
#define LEN_PENALTY_MAX 9

static bool word_start(const char *hay, size_t i) {
  if (i == 0) return true;
  char c = hay[i - 1];
  return c == ' ' || c == '_' || c == '-' || c == '.';
}

static bool match_at(const char *hay, size_t i, const char *w, size_t wn) {
  for (size_t k = 0; k < wn; k++)
    if (!hay[i + k] || tolower((unsigned char)hay[i + k]) != tolower((unsigned char)w[k])) return false;
  return true;
}

/* Best class of one word in hay, or -1 */
static int word_score(const char *hay, size_t hn, const char *w, size_t wn) {
  int best = -1;
  for (size_t i = 0; i + wn <= hn; i++) {
    if (!match_at(hay, i, w, wn)) continue;
    int s = i == 0 ? 310 : word_start(hay, i) ? 210 : 110;
    if (s > best) best = s;
    if (best == 310) return best;
  }
  if (best >= 0) return best;
  size_t k = 0;
  for (size_t i = 0; i < hn && k < wn; i++)
    if (tolower((unsigned char)hay[i]) == tolower((unsigned char)w[k])) k++;
  return k == wn ? 60 : -1;
}

int palette_score(const char *needle, const char *hay) {
  size_t hn = strlen(hay);
  int sum = 0;
  const char *p = needle;
  for (;;) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char *e = p;
    while (*e && *e != ' ') e++;
    int s = word_score(hay, hn, p, (size_t)(e - p));
    if (s < 0) return -1;
    sum += s;
    p = e;
  }
  size_t pen = hn / 8;
  sum -= (int)(pen > LEN_PENALTY_MAX ? LEN_PENALTY_MAX : pen);
  return sum < 0 ? 0 : sum;
}

static int row_score(const char *needle, const ListRow *row) { return palette_score(needle, row->text); }

typedef enum { PAL_ACTION, PAL_TABLE, PAL_VIEW, PAL_WORKSPACE, PAL_NEW_WS, PAL_RENAME_WS,
               PAL_CLOSE_WS, PAL_SAVE_WS, PAL_SAVED_WS, PAL_SAVED_CONN, PAL_RECENT,
               PAL_CONNECT_DIALOG, PAL_SORT, PAL_DELETE_SAVED_WS, PAL_PREF } PalKind;

typedef struct {
  ListWidget list;
  char **owned; size_t num_owned;     /* strings the rows' data point to */
} Palette;

typedef struct { ListRow *rows; size_t n, cap; Palette *pal; } Builder;

static ListRow *add_row(Builder *b, const char *text, const char *detail, char *keys, int kind, void *data) {
  if (b->n == b->cap) {
    b->cap = b->cap ? b->cap * 2 : 64;
    b->rows = safe_realloc(b->rows, b->cap * sizeof *b->rows);
  }
  ListRow *r = &b->rows[b->n++];
  *r = (ListRow){str_dup(text), detail ? str_dup(detail) : NULL, keys, kind, data, false};
  return r;
}

static void add_header(Builder *b, const char *name) {
  add_row(b, name, NULL, NULL, -1, NULL)->header = true;
}

static char *own(Builder *b, const char *s) {
  Palette *p = b->pal;
  p->owned = safe_realloc(p->owned, (p->num_owned + 1) * sizeof *p->owned);
  return p->owned[p->num_owned++] = str_dup(s);
}

static void add_conn(ConnectionItem *item, void *ctx) {
  Builder *b = ctx;
  if (item->type != CONN_ITEM_CONNECTION) return;
  const SavedConnection *c = &item->connection;
  const char *path[16];
  int depth = 0;
  for (ConnectionItem *f = item->parent; f && f->parent && depth < 16; f = f->parent)
    path[depth++] = f->folder.name;
  char *detail = NULL;
  for (int i = depth - 1; i >= 0; i--) {
    char *next = detail ? str_printf("%s/%s", detail, path[i]) : str_dup(path[i]);
    free(detail);
    detail = next;
  }
  char *text = str_printf("Connect to %s", c->name ? c->name : "(unnamed)");
  add_row(b, text, detail, NULL, PAL_SAVED_CONN, own(b, c->id ? c->id : ""));
  free(text);
  free(detail);
}

bool palette_lists_action(const NcApp *app, HotkeyAction a) {
  if (!(hotkey_action_frontends(a) & CONFIG_FE_NOTCURSES) || !app_action_applies(app, a)) return false;
  /* Keys of an editor or a dialog, which only mean something there, and the
   * ncurses TUI's tab keys, which this frontend does not run (close_view and
   * the connection-tab actions are its own) */
  if (a == HOTKEY_CUT_LINE || a == HOTKEY_PASTE || a == HOTKEY_ROW_SAVE ||
      a == HOTKEY_NEW_TAB || a == HOTKEY_CLOSE_TAB || hotkey_get_category(a) == HOTKEY_CAT_EDITOR)
    return false;
  const NcWorkspace *ws = app->num_workspaces ? app->workspaces[app->current_ws] : NULL;
  if (a >= HOTKEY_CONN_TAB_1 && a <= HOTKEY_CONN_TAB_9) /* only the tabs that exist */
    return ws && (size_t)(a - HOTKEY_CONN_TAB_1) < ws->num_tabs;
  if (a == HOTKEY_CLEAR_SELECTIONS) { /* only with something to clear */
    const ConnTab *t = ws ? workspace_current_tab(ws) : NULL;
    const View *v = t ? conntab_current_view(t) : NULL;
    return v && v->num_selected > 0;
  }
  return true;
}

static ListRow *build_rows(NcApp *app, Palette *pal, size_t *count) {
  Builder b = {.pal = pal};
  add_header(&b, "Actions");
  for (int a = 0; a < HOTKEY_COUNT; a++) {
    if (!palette_lists_action(app, (HotkeyAction)a)) continue;
    add_row(&b, hotkey_action_name((HotkeyAction)a), NULL, hotkey_get_display(app->config, (HotkeyAction)a),
            PAL_ACTION, (void *)(intptr_t)a);
  }
  NcWorkspace *ws = app->num_workspaces ? app->workspaces[app->current_ws] : NULL;
  ConnTab *tab = ws ? workspace_current_tab(ws) : NULL;
  const View *cur = tab ? conntab_current_view(tab) : NULL;
  if (cur && cur->kind == VIEW_TABLE)
    add_row(&b, "Sort by column\xe2\x80\xa6", NULL, NULL, PAL_SORT, NULL);
  if (tab && tab->conn) {
    size_t n = 0;
    char **tables = nc_connection_tables(tab->conn, &n);
    if (n) add_header(&b, "Tables");
    for (size_t i = 0; i < n; i++) {
      char *text = str_printf("Open table %s", tables[i]);
      add_row(&b, text, NULL, NULL, PAL_TABLE, own(&b, tables[i]));
      free(text);
    }
  }
  if (tab && tab->num_views) {
    add_header(&b, "Views");
    for (size_t i = 0; i < tab->num_views; i++)
      add_row(&b, view_title(tab->views[i]), NULL, NULL, PAL_VIEW, (void *)(intptr_t)i);
  }
  add_header(&b, "Workspaces");
  for (size_t i = 0; i < app->num_workspaces; i++) {
    char *text = str_printf("Switch to %s", app->workspaces[i]->name);
    add_row(&b, text, NULL, NULL, PAL_WORKSPACE, (void *)(intptr_t)i);
    free(text);
  }
  add_row(&b, "New workspace", NULL, NULL, PAL_NEW_WS, NULL);
  add_row(&b, "Rename workspace", NULL, NULL, PAL_RENAME_WS, NULL);
  add_row(&b, "Close workspace", NULL, NULL, PAL_CLOSE_WS, NULL);
  add_row(&b, "Save workspace as\xe2\x80\xa6", NULL, NULL, PAL_SAVE_WS, NULL);
  for (size_t i = 0; i < app->saved_ws.count; i++) {
    char *text = str_printf("Open saved workspace %s", app->saved_ws.items[i].name);
    add_row(&b, text, NULL, NULL, PAL_SAVED_WS, own(&b, app->saved_ws.items[i].name));
    free(text);
    text = str_printf("Delete saved workspace %s", app->saved_ws.items[i].name);
    add_row(&b, text, NULL, NULL, PAL_DELETE_SAVED_WS, own(&b, app->saved_ws.items[i].name));
    free(text);
  }
  add_header(&b, "Connect");
  if (app->connmgr) connmgr_for_each_connection(app->connmgr, add_conn, &b);
  for (size_t i = 0; i < app->recent.count; i++) {
    char *text = str_printf("Connect to %s", app->recent.items[i].url);
    add_row(&b, text, NULL, NULL, PAL_RECENT, own(&b, app->recent.items[i].url));
    free(text);
  }
  add_row(&b, "Connect\xe2\x80\xa6", NULL, NULL, PAL_CONNECT_DIALOG, NULL);
  add_header(&b, "Preferences"); /* the dialog's on/off settings: one list of them */
  for (int i = 0; i < prefs_count(); i++) {
    if (!prefs_is_toggle(i)) continue;
    char *text = prefs_toggle_text(app, i);
    add_row(&b, text, NULL, NULL, PAL_PREF, (void *)(intptr_t)i);
    free(text);
  }
  *count = b.n;
  return b.rows;
}

static void rename_done(NcApp *app, char *text, void *data) {
  (void)data;
  if (text && *text) app_rename_workspace(app, app->current_ws, text);
  free(text);
}

static void save_as_done(NcApp *app, char *text, void *data) {
  (void)data;
  if (text && *text) app_save_workspace_as(app, app->current_ws, text);
  free(text);
}

static void dispatch(NcApp *app, int kind, intptr_t idx, char *str) {
  switch (kind) {
  case PAL_ACTION: app_do_action(app, (HotkeyAction)idx); break;
  case PAL_PREF: prefs_toggle(app, (int)idx); break;
  case PAL_TABLE: app_open_table(app, str); break;
  case PAL_VIEW: app_switch_view(app, (size_t)idx); break;
  case PAL_WORKSPACE: app_switch_workspace(app, (size_t)idx); break;
  case PAL_NEW_WS: app_new_workspace(app, NULL); break;
  case PAL_RENAME_WS: {
    const char *cur = app->num_workspaces ? app->workspaces[app->current_ws]->name : "";
    prompt_open(app, "Rename workspace", "Name", cur, false, rename_done, NULL);
    break;
  }
  case PAL_CLOSE_WS: app_close_workspace(app, app->current_ws); break;
  case PAL_SAVE_WS: prompt_open(app, "Save workspace", "Name", "", false, save_as_done, NULL); break;
  case PAL_SAVED_WS: {
    size_t i = saved_workspaces_find(&app->saved_ws, str);
    if (i != SIZE_MAX) app_open_saved_workspace(app, i);
    break;
  }
  case PAL_DELETE_SAVED_WS: {
    size_t i = saved_workspaces_find(&app->saved_ws, str);
    if (i != SIZE_MAX) app_delete_saved_workspace(app, i);
    break;
  }
  case PAL_SAVED_CONN: app_connect_saved(app, str, true, NULL, NULL); break;
  case PAL_RECENT: app_connect_url(app, str, NULL, NULL, true, NULL, NULL); break;
  case PAL_CONNECT_DIALOG: connect_open(app); break;
  case PAL_SORT: {
    View *v = app_current_view(app);
    if (v && v->kind == VIEW_TABLE) sort_picker_open(app, v);
    break;
  }
  default: break;
  }
}

static void activate(Overlay *o, NcApp *app) {
  Palette *pal = o->data;
  ListRow *r = list_selected(&pal->list);
  if (!r) return;
  int kind = r->kind;
  intptr_t idx = 0;
  char *str = NULL;
  if (kind == PAL_ACTION || kind == PAL_VIEW || kind == PAL_WORKSPACE || kind == PAL_PREF)
    idx = (intptr_t)r->data;
  else if (r->data) str = str_dup(r->data);
  overlay_pop_this(app, o);
  dispatch(app, kind, idx, str);
  free(str);
}

static bool palette_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  Palette *pal = o->data;
  ListAction a = ev->type == UI_EVENT_MOUSE ? list_mouse(&pal->list, ev, 0, o->rows)
                                            : list_key(&pal->list, ev);
  if (a == LIST_ACTIVATE) activate(o, app);
  else if (a == LIST_CANCEL) overlay_pop_this(app, o);
  return true;
}

static void palette_draw(Overlay *o, NcApp *app) {
  Palette *pal = o->data;
  int cy = 1, cx = 1;
  list_draw(&pal->list, o->plane, &app->theme, 1, 1, o->rows, o->cols, &cy, &cx);
  o->wants_cursor = pal->list.has_input && pal->list.input_focused;
  o->cursor_y = cy - 1;
  o->cursor_x = cx - 1;
}

static void palette_layout(Overlay *o, NcApp *app) {
  o->cols = app->cols - 8 < 90 ? app->cols - 8 : 90;
  o->rows = app->rows - 6 < 24 ? app->rows - 6 : 24;
}

static void palette_free(void *data) {
  Palette *pal = data;
  list_free(&pal->list);
  for (size_t i = 0; i < pal->num_owned; i++) free(pal->owned[i]);
  free(pal->owned);
  free(pal);
}

void palette_open(NcApp *app) {
  Palette *pal = safe_calloc(1, sizeof *pal);
  list_init(&pal->list, true);
  pal->list.score = row_score;
  size_t n = 0;
  ListRow *rows = build_rows(app, pal, &n);
  list_set_rows(&pal->list, rows, n);
  Overlay *o = overlay_new("Command palette");
  o->footer = "Enter run  Esc close  \xe2\x86\x91\xe2\x86\x93 move";
  o->data = pal;
  o->free_data = palette_free;
  o->draw = palette_draw;
  o->key = palette_key;
  o->layout = palette_layout;
  overlay_push(app, o);
}
