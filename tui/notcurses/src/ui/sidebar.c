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

/* The sections, in order: TABLES is always open, the next three fold (a
 * header row is selectable and Enter or a click toggles it), QUERIES lists
 * the tab's query views */
typedef enum { SEC_TABLES, SEC_VIEWS, SEC_ROUTINES, SEC_TRIGGERS, SEC_QUERIES, SEC_COUNT } Section;
static const char *const SECTION_TITLE[SEC_COUNT] = {"TABLES", "VIEWS", "FUNCTIONS", "TRIGGERS", "QUERIES"};
static bool folds(Section s) { return s == SEC_VIEWS || s == SEC_ROUTINES || s == SEC_TRIGGERS; }

/* One line of the sidebar: a section header, or an entry of a section (idx:
 * a table's index in the connection's list, an object's in its object list,
 * a query view's among the tab's views) */
typedef struct { bool header; Section sec; size_t idx; } Line;
/* The selectable lines (headers that fold, every entry) by cursor position */
typedef struct { Line *lines; size_t num_lines; size_t *items; size_t num_items; } Layout;

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

static Section section_of(LaceObjectKind k) {
  return k == LACE_OBJ_VIEW ? SEC_VIEWS : k == LACE_OBJ_ROUTINE ? SEC_ROUTINES : k == LACE_OBJ_TRIGGER ? SEC_TRIGGERS : SEC_TABLES;
}

static void add_line(Layout *L, Line l, bool selectable) {
  L->lines = safe_realloc(L->lines, (L->num_lines + 1) * sizeof *L->lines);
  L->lines[L->num_lines] = l;
  if (selectable) {
    L->items = safe_realloc(L->items, (L->num_items + 1) * sizeof *L->items);
    L->items[L->num_items++] = L->num_lines;
  }
  L->num_lines++;
}

/* The lines: TABLES and its matching tables (loading/none/no match as a
 * note), each object section that has matching entries (folded: only its
 * header), QUERIES with the matching query views */
static void build(ConnTab *t, Layout *L) {
  memset(L, 0, sizeof *L);
  size_t nt = 0, no = 0;
  char **tables = t->conn ? nc_connection_tables(t->conn, &nt) : NULL;
  const LaceObject *objs = t->conn ? nc_connection_objects(t->conn, &no) : NULL;
  add_line(L, (Line){true, SEC_TABLES, 0}, false);
  for (size_t i = 0; i < nt; i++)
    if (matches(tables[i], t->sidebar_search)) add_line(L, (Line){false, SEC_TABLES, i}, true);
  for (Section s = SEC_VIEWS; s <= SEC_TRIGGERS; s++) {
    bool any = false;
    for (size_t i = 0; i < no; i++)
      if (section_of(objs[i].kind) == s && objs[i].name && matches(objs[i].name, t->sidebar_search)) any = true;
    if (!any) continue;
    add_line(L, (Line){true, s, 0}, true);
    if (t->sidebar_collapsed & (1u << s)) continue;
    for (size_t i = 0; i < no; i++)
      if (section_of(objs[i].kind) == s && objs[i].name && matches(objs[i].name, t->sidebar_search))
        add_line(L, (Line){false, s, i}, true);
  }
  bool queries = false;
  for (size_t i = 0; i < t->num_views; i++)
    if (t->views[i]->kind == VIEW_QUERY && matches(view_title(t->views[i]), t->sidebar_search)) queries = true;
  if (queries) {
    add_line(L, (Line){true, SEC_QUERIES, 0}, false);
    for (size_t i = 0; i < t->num_views; i++)
      if (t->views[i]->kind == VIEW_QUERY && matches(view_title(t->views[i]), t->sidebar_search))
        add_line(L, (Line){false, SEC_QUERIES, i}, true);
  }
}

static void layout_free(Layout *L) {
  free(L->lines);
  free(L->items);
}

static size_t num_tables_shown(const Layout *L) {
  size_t n = 0;
  for (size_t i = 0; i < L->num_lines; i++)
    if (!L->lines[i].header && L->lines[i].sec == SEC_TABLES) n++;
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

/* The name a line shows */
static const char *line_name(const ConnTab *t, const Line *l) {
  if (l->header) return SECTION_TITLE[l->sec];
  if (l->sec == SEC_QUERIES) return l->idx < t->num_views ? view_title(t->views[l->idx]) : "";
  if (l->sec == SEC_TABLES) {
    size_t nt = 0;
    char **tables = nc_connection_tables(t->conn, &nt);
    return l->idx < nt ? tables[l->idx] : "";
  }
  size_t no = 0;
  const LaceObject *objs = nc_connection_objects(t->conn, &no);
  return l->idx < no && objs[l->idx].name ? objs[l->idx].name : "";
}

/* The item (cursor position) of the current view: a query's own row, an
 * object's by kind and name */
static size_t open_item_of(const ConnTab *t, const Layout *L) {
  const View *cv = conntab_current_view(t);
  if (!cv) return SIZE_MAX;
  for (size_t i = 0; i < L->num_items; i++) {
    const Line *l = &L->lines[L->items[i]];
    if (l->header) continue;
    if (l->sec == SEC_QUERIES ? l->idx == t->current
                              : cv->kind == VIEW_TABLE && cv->table && section_of(cv->object) == l->sec &&
                                    !strcmp(line_name(t, l), cv->table))
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
  Layout L;
  build(t, &L);
  size_t n = L.num_items;
  if (t->sidebar_cursor >= n) t->sidebar_cursor = n ? n - 1 : 0;
  bool loaded = t->conn && nc_connection_tables_loaded(t->conn);
  const View *cv = conntab_current_view(t);
  size_t open_item = open_item_of(t, &L);
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
  /* a note under TABLES while there is nothing to list */
  const char *note = !loaded ? "loading\xe2\x80\xa6"
                     : !num_tables_shown(&L) ? (t->sidebar_search && *t->sidebar_search ? "no match" : "none")
                                             : NULL;
  /* TABLES stays at the top; the lines under it scroll (the note is the first of them) */
  nc_style(p, &app->theme, TC_DIM, TC_BG, NCSTYLE_BOLD);
  nc_put(p, r.y + top, r.x + 1, cw - 1, SECTION_TITLE[SEC_TABLES]);
  size_t lines = L.num_lines + (note ? 1 : 0);
  size_t body = lines - 1;
  int vis = r.rows - 1 - top;
  size_t cur_line = n ? L.items[t->sidebar_cursor] + (note ? 1 : 0) : 1; /* every item is under the header */
  size_t cur_body = cur_line ? cur_line - 1 : 0;
  if (cur_body < t->sidebar_scroll) t->sidebar_scroll = cur_body;
  if (vis > 0 && cur_body >= t->sidebar_scroll + (size_t)vis) t->sidebar_scroll = cur_body - (size_t)vis + 1;
  if (t->sidebar_scroll > 0 && body <= (size_t)vis) t->sidebar_scroll = 0;
  int row = 0;
  for (size_t line = 1 + t->sidebar_scroll; line < lines && row < vis; line++, row++) {
    int y = r.y + 1 + top + row;
    if (note && line == 1) { /* right under TABLES */
      nc_style(p, &app->theme, TC_DIM, TC_BG, 0);
      nc_put(p, y, r.x + 1, cw - 1, note);
      continue;
    }
    size_t li = note && line > 1 ? line - 1 : line;
    const Line *l = &L.lines[li];
    size_t item = SIZE_MAX;
    for (size_t i = 0; i < n; i++)
      if (L.items[i] == li) { item = i; break; }
    /* the selection shows while the list is driven (focused, or nothing else to
     * drive); the open view's entry is highlighted as the strip's current tab,
     * also when the selection is on it */
    bool cur = item != SIZE_MAX && item == t->sidebar_cursor && (t->sidebar_focused || !cv);
    bool open = item != SIZE_MAX && item == open_item;
    if (l->header) {
      nc_style(p, &app->theme, cur ? TC_CURSOR_FG : TC_DIM, cur ? TC_CURSOR_BG : TC_BG, NCSTYLE_BOLD);
      nc_put(p, y, r.x, cw, "");
      char *title = folds(l->sec) ? str_printf("%s %s", t->sidebar_collapsed & (1u << l->sec) ? "\xe2\x96\xb8" : "\xe2\x96\xbe",
                                               SECTION_TITLE[l->sec])
                                  : str_dup(SECTION_TITLE[l->sec]);
      nc_put(p, y, r.x + 1, cw - 1, title);
      free(title);
      continue;
    }
    const char *name = line_name(t, l);
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
  layout_free(&L);
}

/* Open the entry: a query view is switched to; a section header folds; a
 * table (view, routine, trigger) opens for keeps (Enter, a double click) or
 * in the preview tab (a single click) */
static void activate(NcApp *app, ConnTab *t, size_t item, bool keep) {
  Layout L;
  build(t, &L);
  if (item < L.num_items) {
    Line l = L.lines[L.items[item]];
    if (l.header) {
      t->sidebar_collapsed ^= 1u << l.sec;
    } else if (l.sec == SEC_QUERIES) {
      t->sidebar_focused = false;
      app_switch_view(app, l.idx);
    } else if (l.sec == SEC_TABLES) {
      t->sidebar_focused = false;
      char *name = str_dup(line_name(t, &l)); /* a refresh may replace the list */
      if (keep) app_open_table(app, name);
      else app_preview_table(app, name);
      free(name);
    } else {
      size_t no = 0;
      const LaceObject *objs = nc_connection_objects(t->conn, &no);
      if (l.idx < no) {
        t->sidebar_focused = false;
        LaceObjectKind kind = objs[l.idx].kind;
        char *name = str_dup(objs[l.idx].name), *table = objs[l.idx].table ? str_dup(objs[l.idx].table) : NULL;
        char *detail = objs[l.idx].detail ? str_dup(objs[l.idx].detail) : NULL;
        if (keep) app_open_object(app, kind, name, table, detail);
        else app_preview_object(app, kind, name, table, detail);
        free(name);
        free(table);
        free(detail);
      }
    }
  }
  layout_free(&L);
}

char *sidebar_highlighted_object(ConnTab *t, LaceObjectKind *kind, char **table, char **detail) {
  if (table) *table = NULL;
  if (detail) *detail = NULL;
  if (kind) *kind = LACE_OBJ_TABLE;
  if (!t->conn) return NULL;
  Layout L;
  build(t, &L);
  char *name = NULL;
  if (t->sidebar_cursor < L.num_items) {
    const Line *l = &L.lines[L.items[t->sidebar_cursor]];
    if (!l->header && l->sec == SEC_TABLES) {
      name = str_dup(line_name(t, l));
    } else if (!l->header && l->sec != SEC_QUERIES) {
      size_t no = 0;
      const LaceObject *objs = nc_connection_objects(t->conn, &no);
      if (l->idx < no) {
        name = str_dup(objs[l->idx].name);
        if (kind) *kind = objs[l->idx].kind;
        if (table && objs[l->idx].table) *table = str_dup(objs[l->idx].table);
        if (detail && objs[l->idx].detail) *detail = str_dup(objs[l->idx].detail);
      }
    }
  }
  layout_free(&L);
  return name;
}

static size_t count_items(ConnTab *t) {
  Layout L;
  build(t, &L);
  size_t n = L.num_items;
  layout_free(&L);
  return n;
}

static void move(ConnTab *t, long delta) {
  size_t n = count_items(t);
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
  Rect c = content_rect(app); /* the sidebar's rows: the search (when shown), the sections */
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
  if (y == c.y + top) return true; /* the TABLES header, pinned */
  Layout L;
  build(t, &L);
  bool loaded = t->conn && nc_connection_tables_loaded(t->conn);
  bool note = !loaded || !num_tables_shown(&L);
  size_t line = 1 + t->sidebar_scroll + (size_t)(y - c.y - top - 1);
  if (note && line == 1) { layout_free(&L); return true; } /* the note under TABLES */
  size_t li = note && line > 1 ? line - 1 : line;
  size_t item = SIZE_MAX;
  for (size_t i = 0; li < L.num_lines && i < L.num_items; i++)
    if (L.items[i] == li) { item = i; break; }
  bool header = item != SIZE_MAX && L.lines[li].header;
  layout_free(&L);
  if (item != SIZE_MAX) {
    int64_t now = app_now_ms();
    bool dbl = g_click.tab == t && g_click.item == item && now - g_click.at_ms <= DOUBLE_CLICK_MS;
    g_click.at_ms = dbl ? 0 : now; /* a third press starts over */
    g_click.item = item;
    g_click.tab = t;
    t->sidebar_cursor = item;
    if (header) activate(app, t, item, true); /* a click folds and unfolds */
    else activate(app, t, item, dbl);
  }
  return true;
}
