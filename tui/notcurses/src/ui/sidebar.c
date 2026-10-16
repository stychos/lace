#include "ui/sidebar.h"
#include "core/utf8.h"
#include "model/workspace.h"
#include "ui/input.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SIDEBAR_MAX 28
#define SIDEBAR_MIN 10
#define DOUBLE_CLICK_MS 400 /* two presses on the same entry within this are a double click */
#define MARQUEE_PAUSE_MS 1500 /* a long name rests at each end this long... */
#define MARQUEE_STEP_MS 200   /* ...and moves one column every this */

typedef struct { bool query; size_t idx; } Item;

static struct { bool set; int y, x; } g_cursor;
static bool g_animating; /* the last draw scrolled a long name: keep the loop ticking */
static struct { int64_t at_ms; size_t item; const ConnTab *tab; } g_click; /* the last left press on an entry */

int sidebar_width(const NcApp *app) {
  if (!app->num_workspaces) return 0;
  const ConnTab *t = workspace_current_tab(app->workspaces[app->current_ws]);
  if (!t || !t->sidebar) return 0;
  int w = app->cols / 3 < SIDEBAR_MAX ? app->cols / 3 : SIDEBAR_MAX;
  return w < SIDEBAR_MIN ? 0 : w;
}

bool sidebar_cursor(int *y, int *x) {
  if (!g_cursor.set) return false;
  *y = g_cursor.y;
  *x = g_cursor.x;
  return true;
}

static bool matches(const char *name, const char *needle) {
  return !needle || !*needle || strcasestr(name, needle) != NULL;
}

/* The shown entries: matching tables, then matching query views */
static size_t build_items(ConnTab *t, Item **out, size_t *num_tables) {
  size_t nt = 0;
  char **tables = t->conn ? nc_connection_tables(t->conn, &nt) : NULL;
  Item *items = safe_calloc(nt + t->num_views + 1, sizeof *items);
  size_t n = 0;
  for (size_t i = 0; i < nt; i++)
    if (matches(tables[i], t->sidebar_search)) items[n++] = (Item){false, i};
  *num_tables = n;
  for (size_t i = 0; i < t->num_views; i++)
    if (t->views[i]->kind == VIEW_QUERY && matches(view_title(t->views[i]), t->sidebar_search))
      items[n++] = (Item){true, i};
  *out = items;
  return n;
}

static void load_input(const ConnTab *t, TextInput *in) {
  text_input_init(in, t->sidebar_search ? t->sidebar_search : "", false);
  in->cursor = t->sidebar_search_pos < in->len ? t->sidebar_search_pos : in->len;
}

static void store_input(ConnTab *t, TextInput *in) {
  free(t->sidebar_search);
  t->sidebar_search = in->len ? str_dup(in->buf) : NULL;
  t->sidebar_search_pos = in->cursor;
  text_input_free(in);
}

static size_t line_of(size_t item, size_t num_tables) { return item < num_tables ? item : item + 1; }

int sidebar_marquee_offset(int overflow, int64_t now_ms) {
  if (overflow <= 0) return 0;
  int64_t period = 2 * MARQUEE_PAUSE_MS + (int64_t)overflow * MARQUEE_STEP_MS;
  int64_t at = now_ms % period;
  if (at < MARQUEE_PAUSE_MS) return 0;
  at -= MARQUEE_PAUSE_MS;
  if (at >= (int64_t)overflow * MARQUEE_STEP_MS) return overflow;
  return (int)(at / MARQUEE_STEP_MS) + 1;
}

bool sidebar_animating(void) { return g_animating; }

/* The search field shows while it has text, or from the sidebar_filter key
 * until the sidebar loses the focus with none */
static bool search_shown(ConnTab *t) {
  if (t->sidebar_search && *t->sidebar_search) return true;
  if (!t->sidebar_focused) t->sidebar_search_visible = false;
  return t->sidebar_search_visible;
}

void sidebar_show_search(ConnTab *t) {
  t->sidebar_focused = true;
  t->sidebar_search_visible = true;
}

/* The text from the first character at or past display column cols */
static const char *skip_cols(const char *s, int cols) {
  size_t len = strlen(s), pos = 0;
  int w = 0;
  while (pos < len && w < cols) {
    size_t next = utf8_next(s, len, pos);
    w += utf8_width(s + pos, next - pos);
    pos = next;
  }
  return s + pos;
}

/* The entry of the current view: a query's own row, a table's by name */
static size_t open_item_of(const ConnTab *t, const Item *items, size_t n, char **tables) {
  const View *cv = conntab_current_view(t);
  for (size_t i = 0; cv && i < n; i++) {
    const Item *it = &items[i];
    if (it->query ? it->idx == t->current
                  : cv->kind == VIEW_TABLE && cv->table && !strcmp(tables[it->idx], cv->table))
      return i;
  }
  return SIZE_MAX;
}

void sidebar_draw(NcApp *app, ConnTab *t, Rect r) {
  g_cursor.set = false;
  g_animating = false;
  struct ncplane *p = app->std;
  int cw = r.cols - 1;
  if (cw < 4 || r.rows < 3) return;
  Item *items;
  size_t nt_items = 0;
  size_t n = build_items(t, &items, &nt_items);
  if (t->sidebar_cursor >= n) t->sidebar_cursor = n ? n - 1 : 0;
  bool loaded = t->conn && nc_connection_tables_loaded(t->conn);
  size_t nt = 0;
  char **tables = t->conn ? nc_connection_tables(t->conn, &nt) : NULL;
  const View *cv = conntab_current_view(t);
  size_t open_item = open_item_of(t, items, n, tables);
  /* The cursor follows the current view when it changes (a tab click, the
   * keys, a new view): its entry is highlighted and scrolled into view */
  uint64_t uid = cv ? cv->uid : 0;
  if (uid != t->sidebar_view_uid && (!cv || cv->kind == VIEW_QUERY || loaded)) {
    t->sidebar_view_uid = uid;
    if (open_item != SIZE_MAX) t->sidebar_cursor = open_item;
  }
  /* divider */
  nc_style(p, &app->theme, TC_DIM, TC_BG, 0);
  for (int y = r.y; y < r.y + r.rows; y++) nc_put(p, y, r.x + cw, 1, "\xe2\x94\x82");
  /* the search field, only when called or filled */
  int top = 0;
  if (search_shown(t)) {
    TextInput in;
    load_input(t, &in);
    int off = nc_text_input_draw(p, &app->theme, &in, r.y, r.x + 1, cw - 1, t->sidebar_focused);
    if (t->sidebar_focused) { g_cursor.set = true; g_cursor.y = r.y; g_cursor.x = r.x + 1 + off; }
    text_input_free(&in);
    top = 1;
  }
  /* TABLES */
  nc_style(p, &app->theme, TC_DIM, TC_BG, NCSTYLE_BOLD);
  nc_put(p, r.y + top, r.x + 1, cw - 1, "TABLES");
  if (!loaded) {
    nc_style(p, &app->theme, TC_DIM, TC_BG, 0);
    nc_put(p, r.y + top + 1, r.x + 1, cw - 1, "loading\xe2\x80\xa6");
  } else if (!nt_items) {
    nc_style(p, &app->theme, TC_DIM, TC_BG, 0);
    nc_put(p, r.y + top + 1, r.x + 1, cw - 1, t->sidebar_search && *t->sidebar_search ? "no match" : "none");
  }
  size_t lines = n + (n > nt_items ? 1 : 0);
  int vis = r.rows - 1 - top;
  size_t cur_line = n ? line_of(t->sidebar_cursor, nt_items) : 0;
  if (cur_line < t->sidebar_scroll) t->sidebar_scroll = cur_line;
  if (vis > 0 && cur_line >= t->sidebar_scroll + (size_t)vis) t->sidebar_scroll = cur_line - (size_t)vis + 1;
  if (t->sidebar_scroll > 0 && lines <= (size_t)vis) t->sidebar_scroll = 0;
  int row = 0;
  for (size_t line = t->sidebar_scroll; line < lines && row < vis; line++, row++) {
    int y = r.y + 1 + top + row;
    if (n > nt_items && line == nt_items) { /* the QUERIES header */
      nc_style(p, &app->theme, TC_DIM, TC_BG, NCSTYLE_BOLD);
      nc_put(p, y, r.x + 1, cw - 1, "QUERIES");
      continue;
    }
    size_t item = line < nt_items ? line : line - (n > nt_items ? 1 : 0);
    const Item *it = &items[item];
    /* the selection shows while the list is driven (focused, or nothing else to
     * drive); the open view's entry is highlighted as the strip's current tab,
     * also when the selection is on it */
    bool cur = item == t->sidebar_cursor && (t->sidebar_focused || !cv);
    bool open = item == open_item;
    const char *name = it->query ? view_title(t->views[it->idx]) : tables[it->idx];
    if (open) nc_style(p, &app->theme, TC_ACCENT, TC_BG, NCSTYLE_BOLD);
    else nc_style(p, &app->theme, cur ? TC_CURSOR_FG : TC_FG, cur ? TC_CURSOR_BG : TC_BG, 0);
    nc_put(p, y, r.x, cw, "");
    /* a long name scrolls in place on the highlighted entry, so all of it can be read */
    int overflow = utf8_width(name, strlen(name)) - (cw - 1);
    if (overflow > 0 && (cur || (open && !t->sidebar_focused))) {
      g_animating = true;
      name = skip_cols(name, sidebar_marquee_offset(overflow, app_now_ms()));
    }
    nc_put(p, y, r.x + 1, cw - 1, name);
  }
  free(items);
}

/* Open the entry: a query view is switched to; a table opens for keeps
 * (Enter, a double click) or in the preview tab (a single click) */
static void activate(NcApp *app, ConnTab *t, size_t item, bool keep) {
  Item *items;
  size_t nt = 0;
  size_t n = build_items(t, &items, &nt);
  if (item < n) {
    Item it = items[item];
    t->sidebar_focused = false;
    if (it.query) {
      app_switch_view(app, it.idx);
    } else {
      size_t cnt = 0;
      char **tables = nc_connection_tables(t->conn, &cnt);
      if (it.idx < cnt) {
        char *name = str_dup(tables[it.idx]); /* a refresh may replace the list */
        if (keep) app_open_table(app, name);
        else app_preview_table(app, name);
        free(name);
      }
    }
  }
  free(items);
}

char *sidebar_highlighted_table(ConnTab *t) {
  if (!t->conn) return NULL;
  Item *items;
  size_t nt = 0;
  size_t n = build_items(t, &items, &nt);
  char *name = NULL;
  if (t->sidebar_cursor < n && !items[t->sidebar_cursor].query) {
    size_t cnt = 0;
    char **tables = nc_connection_tables(t->conn, &cnt);
    if (items[t->sidebar_cursor].idx < cnt) name = str_dup(tables[items[t->sidebar_cursor].idx]);
  }
  free(items);
  return name;
}

static size_t count_items(ConnTab *t) {
  Item *items;
  size_t nt = 0;
  size_t n = build_items(t, &items, &nt);
  free(items);
  return n;
}

static void move(ConnTab *t, long delta) {
  Item *items;
  size_t nt = 0;
  size_t n = build_items(t, &items, &nt);
  free(items);
  if (!n) return;
  long c = (long)t->sidebar_cursor + delta;
  if (c < 0) c = 0;
  if (c >= (long)n) c = (long)n - 1;
  t->sidebar_cursor = (size_t)c;
}

bool sidebar_handle_key(NcApp *app, ConnTab *t, const UiEvent *ev) {
  if (ev->type != UI_EVENT_KEY || !t->sidebar || !sidebar_width(app)) return false;
  bool focused = t->sidebar_focused;
  bool soft = !focused && !conntab_current_view(t); /* nothing else to drive: the list takes the arrows */
  if (!focused && !soft) return false;
  int page = app->rows > 8 ? app->rows - 6 : 1;
  if (ev->key.is_special && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
    switch (ev->key.key) {
    case UI_KEY_UP: move(t, -1); return true;
    case UI_KEY_DOWN: move(t, 1); return true;
    case UI_KEY_PAGEUP: if (focused) { move(t, -page); return true; } break;
    case UI_KEY_PAGEDOWN: if (focused) { move(t, page); return true; } break;
    case UI_KEY_ENTER: activate(app, t, t->sidebar_cursor, true); return true;
    case UI_KEY_ESCAPE: /* closes the search field (clearing it), then leaves the sidebar */
      if (!focused) return false;
      if (search_shown(t)) {
        if (t->sidebar_search && *t->sidebar_search) t->sidebar_cursor = t->sidebar_scroll = 0;
        free(t->sidebar_search);
        t->sidebar_search = NULL;
        t->sidebar_search_pos = 0;
        t->sidebar_search_visible = false;
      } else {
        t->sidebar_focused = false;
      }
      return true;
    default: break;
    }
  }
  if (soft || !search_shown(t)) { /* no field: the list takes the navigation hotkeys (j, k, g, G, ...) */
    const Config *cfg = app->config;
    if (hotkey_matches(cfg, ev, HOTKEY_MOVE_DOWN)) { move(t, 1); return true; }
    if (hotkey_matches(cfg, ev, HOTKEY_MOVE_UP)) { move(t, -1); return true; }
    if (hotkey_matches(cfg, ev, HOTKEY_PAGE_DOWN)) { move(t, page); return true; }
    if (hotkey_matches(cfg, ev, HOTKEY_PAGE_UP)) { move(t, -page); return true; }
    if (hotkey_matches(cfg, ev, HOTKEY_FIRST_ROW)) { t->sidebar_cursor = 0; return true; }
    if (hotkey_matches(cfg, ev, HOTKEY_LAST_ROW)) { move(t, (long)count_items(t)); return true; }
    if (soft) return false;
    if (hotkey_matches(cfg, ev, HOTKEY_SIDEBAR_FILTER)) { /* the one key that calls the field */
      sidebar_show_search(t);
      return true;
    }
    return false; /* the rest are the hotkeys' */
  }
  if (ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT)) return false; /* the hotkeys keep their keys */
  TextInput in;
  load_input(t, &in);
  char *before = str_dup(in.buf ? in.buf : "");
  bool used = nc_text_input_key(&in, ev);
  bool changed = strcmp(before, in.buf ? in.buf : "") != 0;
  free(before);
  store_input(t, &in);
  if (changed) t->sidebar_cursor = t->sidebar_scroll = 0;
  return used;
}

bool sidebar_handle_mouse(NcApp *app, ConnTab *t, const UiEvent *ev) {
  int sw = sidebar_width(app);
  if (!sw || ev->type != UI_EVENT_MOUSE) return false;
  Rect c = content_rect(app); /* the sidebar's rows: the search (when shown), TABLES, the entries */
  int x = ev->mouse.x, y = ev->mouse.y;
  if (x >= sw || y < c.y || y >= c.y + c.rows) return false;
  if (ev->mouse.button == UI_MOUSE_SCROLL_UP) { move(t, -3); return true; }
  if (ev->mouse.button == UI_MOUSE_SCROLL_DOWN) { move(t, 3); return true; }
  if (ev->mouse.button != UI_MOUSE_LEFT || ev->mouse.action != UI_MOUSE_PRESS) return true;
  int top = search_shown(t) ? 1 : 0;
  if (top && y == c.y) {
    sidebar_show_search(t);
    return true;
  }
  if (y < c.y + 1 + top) return true; /* the TABLES header */
  Item *items;
  size_t nt = 0;
  size_t n = build_items(t, &items, &nt);
  free(items);
  size_t line = t->sidebar_scroll + (size_t)(y - c.y - 1 - top);
  if (n > nt && line == nt) return true; /* the QUERIES header */
  size_t item = line < nt ? line : line - (n > nt ? 1 : 0);
  if (item < n) {
    int64_t now = app_now_ms();
    bool dbl = g_click.tab == t && g_click.item == item && now - g_click.at_ms <= DOUBLE_CLICK_MS;
    g_click.at_ms = dbl ? 0 : now; /* a third press starts over */
    g_click.item = item;
    g_click.tab = t;
    t->sidebar_cursor = item;
    activate(app, t, item, dbl);
  }
  return true;
}
