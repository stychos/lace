#include "ui/subviews.h"
#include "core/utf8.h"
#include "model/workspace.h"
#include "ui/filter_panel.h"
#include "ui/sidebar.h"
#include "ui/tabstrip.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

#define SV_MIN_ROWS 14   /* below this the strip gives its row to the content (as the tabs strip) */
#define SV_COL_MAX 60    /* a cell wider than this is cut (a long default or expression) */

/* ==========================================================================
 * The strip
 * ========================================================================== */

static const View *current_view(const NcApp *app) {
  const ConnTab *t = app->num_workspaces ? workspace_current_tab(app->workspaces[app->current_ws]) : NULL;
  return t && t->conn ? conntab_current_view(t) : NULL;
}

int subviews_rows(const NcApp *app) {
  if (app->rows < SV_MIN_ROWS) return 0;
  const View *v = current_view(app);
  return v && v->kind == VIEW_TABLE ? 1 : 0;
}

Rect subviews_rect(const NcApp *app) {
  Rect c = content_rect(app);
  int sw = sidebar_width(app);
  return (Rect){c.y, sw, subviews_rows(app), c.cols - sw};
}

const char *subview_label(const View *v, SubView sv) {
  switch (sv) {
  case SV_DATA: return "Data";
  case SV_STRUCTURE: return v->object == LACE_OBJ_TABLE ? "Structure" : "Definition";
  case SV_INDEXES: return "Indexes";
  case SV_FOREIGN_KEYS: return "Foreign keys";
  case SV_INFORMATION: return "Information";
  default: return "";
  }
}

static const SvOps *ops_of(const View *v) {
  switch (v->subview) {
  case SV_STRUCTURE: return v->object == LACE_OBJ_TABLE ? &sv_structure_ops : &sv_definition_ops;
  case SV_INDEXES: return &sv_indexes_ops;
  case SV_FOREIGN_KEYS: return &sv_fks_ops;
  case SV_INFORMATION: return &sv_info_ops;
  default: return NULL;
  }
}

static HotkeyAction action_of(SubView sv) {
  static const HotkeyAction a[SV_COUNT] = {HOTKEY_SUBVIEW_DATA, HOTKEY_SUBVIEW_STRUCTURE, HOTKEY_SUBVIEW_INDEXES,
                                           HOTKEY_SUBVIEW_FOREIGN_KEYS, HOTKEY_SUBVIEW_INFORMATION};
  return a[sv];
}

void subview_set(NcApp *app, View *v, SubView sv) {
  if (!v || v->kind != VIEW_TABLE) return;
  if (!view_has_subview(v, sv)) {
    static const char *const what[] = {"table", "view", "routine", "trigger"};
    app_set_status(app, false, "A %s has no %s", what[v->object <= LACE_OBJ_TRIGGER ? v->object : 0],
                   subview_label(v, sv));
    return;
  }
  if (v->subview == SV_DATA && sv != SV_DATA) {
    grid_edit_cancel(v); /* the grid goes behind the list */
    if (v->fp.focused) filter_panel_blur(app, v);
  }
  v->subview = sv;
}

size_t subviews_layout(const int *widths, size_t n, size_t current, int width, int *x0, int *x1) {
  return tabstrip_layout(widths, n, current, width, x0, x1);
}

/* The first key of the action's binding when it is short enough to show */
static const char *key_hint(const NcApp *app, HotkeyAction a) {
  const HotkeyBinding *b = &app->config->hotkeys[a];
  if (!b->num_keys || !b->keys[0] || strlen(b->keys[0]) > 3) return NULL;
  return b->keys[0];
}

void subviews_draw(NcApp *app, View *v) {
  app->subview_spans.n = 0;
  if (!subviews_rows(app) || !v || v->kind != VIEW_TABLE) return;
  Rect r = subviews_rect(app);
  if (r.cols < 4) return;
  struct ncplane *p = app->std;
  const Theme *th = &app->theme;
  nc_style(p, th, TC_DIM, TC_STRIPE_BG, 0);
  nc_put(p, r.y, r.x, r.cols, "");
  SubView svs[SV_COUNT];
  size_t n = 0, current = 0;
  for (int i = 0; i < SV_COUNT; i++)
    if (view_has_subview(v, (SubView)i)) {
      if ((SubView)i == v->subview) current = n;
      svs[n++] = (SubView)i;
    }
  const char *keys[SV_COUNT] = {0};
  char *labels[SV_COUNT] = {0};
  int w[SV_COUNT], x0[SV_COUNT], x1[SV_COUNT];
  size_t first = 0;
  for (int pass = 0; pass < 2; pass++) { /* with the keys; without them when they do not fit */
    for (size_t i = 0; i < n; i++) {
      free(labels[i]);
      keys[i] = pass == 0 ? key_hint(app, action_of(svs[i])) : NULL;
      labels[i] = keys[i] ? str_printf("%s %s", keys[i], subview_label(v, svs[i])) : str_dup(subview_label(v, svs[i]));
      w[i] = nc_text_width(labels[i]);
    }
    first = subviews_layout(w, n, current, r.cols, x0, x1);
    if (first == 0 && x1[n - 1] > 0) break;
  }
  if (first) nc_put(p, r.y, r.x, 1, "\xe2\x80\xb9");
  size_t last = first;
  for (size_t i = first; i < n; i++) {
    if (x1[i] == x0[i]) break;
    last = i;
    bool cur = i == current;
    if (cur) nc_style(p, th, TC_FG, TC_BG, NCSTYLE_BOLD);
    else nc_style(p, th, TC_DIM, TC_STRIPE_BG, 0);
    nc_put(p, r.y, r.x + x0[i], x1[i] - x0[i], "");
    nc_put(p, r.y, r.x + x0[i] + 1, w[i], labels[i]);
    if (keys[i]) { /* the key stands out from the label */
      nc_style(p, th, TC_KEY_FG, cur ? TC_BG : TC_STRIPE_BG, cur ? NCSTYLE_BOLD : 0);
      nc_put(p, r.y, r.x + x0[i] + 1, nc_text_width(keys[i]), keys[i]);
    }
    hit_add(&app->subview_spans, r.x + x0[i], r.x + x1[i], (int)svs[i]);
    if (i + 1 < n && x1[i + 1] > x1[i]) {
      nc_style(p, th, TC_DIM, TC_STRIPE_BG, 0);
      nc_put(p, r.y, r.x + x1[i], 1, "\xe2\x94\x82");
    }
  }
  if (last + 1 < n) {
    nc_style(p, th, TC_DIM, TC_STRIPE_BG, 0);
    nc_put(p, r.y, r.x + r.cols - 1, 1, "\xe2\x80\xba");
  }
  for (size_t i = 0; i < n; i++) free(labels[i]);
}

/* ==========================================================================
 * The column list
 * ========================================================================== */

static bool selectable(const CatalogRows *t, size_t r) { return catalog_rows_selectable(t, r); }

/* Clamp the cursor to a selectable row (the nearest one after it, else before) */
static void clamp_cursor(const CatalogRows *t, size_t *cursor) {
  if (!t->nrows) { *cursor = 0; return; }
  if (*cursor >= t->nrows) *cursor = t->nrows - 1;
  for (size_t r = *cursor; r < t->nrows; r++)
    if (selectable(t, r)) { *cursor = r; return; }
  for (size_t r = *cursor + 1; r-- > 0;)
    if (selectable(t, r)) { *cursor = r; return; }
}

static void move_cursor(const CatalogRows *t, size_t *cursor, long delta) {
  if (!t->nrows) return;
  long c = (long)*cursor;
  long step = delta < 0 ? -1 : 1;
  long left = delta < 0 ? -delta : delta;
  long last = c;
  while (left > 0) {
    c += step;
    if (c < 0 || c >= (long)t->nrows) break;
    if (selectable(t, (size_t)c)) { last = c; left--; }
  }
  *cursor = (size_t)last;
}

static void draw_message(NcApp *app, Rect r, const char *msg, bool error) {
  nc_style(app->std, &app->theme, error ? TC_ERROR_FG : TC_DIM, TC_BG, 0);
  int w = nc_text_width(msg);
  if (w > r.cols) w = r.cols;
  nc_put(app->std, r.y + 1 + (r.rows - 1) / 2, r.x + (r.cols - w) / 2, w, msg);
}

static int cell_width(const char *s, bool last) {
  int w = nc_text_width(s);
  return !last && w > SV_COL_MAX ? SV_COL_MAX : w; /* the last column takes what is left */
}

static void svt_draw(NcApp *app, const CatalogRows *t, Rect r, size_t *cursor, size_t *scroll) {
  struct ncplane *p = app->std;
  const Theme *th = &app->theme;
  int *w = safe_calloc(t->ncols ? t->ncols : 1, sizeof *w);
  for (size_t i = 0; t->head && i < t->ncols; i++) w[i] = cell_width(t->head[i], i + 1 == t->ncols);
  for (size_t row = 0; row < t->nrows; row++) {
    if (t->span[row]) continue; /* a heading row with cells (a header under a heading) counts */
    for (size_t i = 0; i < t->ncols; i++) {
      int cw = cell_width(t->cells[row][i], i + 1 == t->ncols);
      if (cw > w[i]) w[i] = cw;
    }
  }
  int y = r.y;
  if (t->head) {
    nc_style(p, th, TC_HEADER_FG, TC_HEADER_BG, NCSTYLE_BOLD);
    nc_put(p, y, r.x, r.cols, "");
    int x = r.x + 1;
    for (size_t i = 0; i < t->ncols && x < r.x + r.cols; i++) {
      int cw = w[i];
      if (cw > r.x + r.cols - x) cw = r.x + r.cols - x;
      nc_put(p, y, x, cw, t->head[i]);
      x += w[i] + 2;
    }
    y++;
  }
  int vis = r.y + r.rows - y;
  clamp_cursor(t, cursor);
  if (vis < 1) { free(w); return; }
  if (*cursor < *scroll) *scroll = *cursor;
  if (*cursor >= *scroll + (size_t)vis) *scroll = *cursor - (size_t)vis + 1;
  if (t->nrows <= (size_t)vis) *scroll = 0;
  for (int i = 0; i < vis; i++, y++) {
    size_t row = *scroll + (size_t)i;
    if (row >= t->nrows) break;
    bool cur = row == *cursor && !t->heading[row];
    ThemeColor bg = cur ? TC_CURSOR_BG : (i % 2 ? TC_STRIPE_BG : TC_ROW_BG);
    if (t->heading[row] && !t->span[row]) nc_style(p, th, TC_HEADER_FG, TC_HEADER_BG, NCSTYLE_BOLD);
    else if (t->heading[row]) nc_style(p, th, TC_ACCENT, TC_BG, NCSTYLE_BOLD);
    else nc_style(p, th, cur ? TC_CURSOR_FG : TC_FG, bg, 0);
    nc_put(p, y, r.x, r.cols, "");
    if (t->span[row]) {
      nc_put(p, y, r.x + 1, r.cols - 1, t->cells[row][0]);
      continue;
    }
    int x = r.x + 1;
    for (size_t c = 0; c < t->ncols && x < r.x + r.cols; c++) {
      int cw = w[c];
      if (cw > r.x + r.cols - x) cw = r.x + r.cols - x;
      nc_put(p, y, x, cw, t->cells[row][c]);
      x += w[c] + 2;
    }
  }
  free(w);
}

void subview_draw(NcApp *app, View *v, Rect r) {
  if (r.rows < 1 || r.cols < 4) return;
  nc_style(app->std, &app->theme, TC_FG, TC_BG, 0);
  for (int y = r.y; y < r.y + r.rows; y++) nc_put(app->std, y, r.x, r.cols, "");
  const SvOps *ops = ops_of(v);
  if (!ops) return;
  CatalogRows t;
  char *msg = NULL;
  catalog_rows_init(&t, 0, NULL);
  if (!ops->build(app, v, &t, &msg)) {
    draw_message(app, r, msg ? msg : "Nothing to show", msg && strncmp(msg, "Loading", 7) != 0);
  } else {
    svt_draw(app, &t, r, &v->sv_cursor[v->subview], &v->sv_scroll[v->subview]);
  }
  free(msg);
  catalog_rows_free(&t);
}

/* ==========================================================================
 * Keys, mouse, actions
 * ========================================================================== */

static bool build_now(NcApp *app, View *v, CatalogRows *t) {
  const SvOps *ops = ops_of(v);
  char *msg = NULL;
  catalog_rows_init(t, 0, NULL);
  bool ok = ops && ops->build(app, v, t, &msg);
  free(msg);
  return ok;
}

static bool list_move(NcApp *app, View *v, long delta, bool absolute_end) {
  CatalogRows t;
  if (!build_now(app, v, &t)) { catalog_rows_free(&t); return true; }
  size_t *cursor = &v->sv_cursor[v->subview];
  if (absolute_end) {
    *cursor = delta < 0 ? 0 : t.nrows ? t.nrows - 1 : 0;
    clamp_cursor(&t, cursor);
    if (delta < 0) { /* the first selectable row */
      for (size_t r = 0; r < t.nrows; r++) if (selectable(&t, r)) { *cursor = r; break; }
    }
  } else {
    clamp_cursor(&t, cursor);
    move_cursor(&t, cursor, delta);
  }
  catalog_rows_free(&t);
  return true;
}

static bool list_activate(NcApp *app, View *v) {
  const SvOps *ops = ops_of(v);
  if (!ops || !ops->activate) return false;
  CatalogRows t;
  if (!build_now(app, v, &t)) { catalog_rows_free(&t); return true; }
  size_t cursor = v->sv_cursor[v->subview];
  clamp_cursor(&t, &cursor);
  if (cursor < t.nrows && selectable(&t, cursor)) {
    int tag = t.tag[cursor];
    catalog_rows_free(&t);
    ops->activate(app, v, tag);
    return true;
  }
  catalog_rows_free(&t);
  return true;
}

const char *subview_enter_hint(const View *v) {
  const SvOps *ops = v && v->kind == VIEW_TABLE ? ops_of(v) : NULL;
  return ops ? ops->hint : NULL;
}

bool subview_handle_key(NcApp *app, View *v, const UiEvent *ev, Rect r) {
  if (ev->type != UI_EVENT_KEY || !v || v->kind != VIEW_TABLE || v->subview == SV_DATA) return false;
  long page = r.rows > 2 ? r.rows - 2 : 1;
  if (ev->key.is_special && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
    switch (ev->key.key) {
    case UI_KEY_UP: return list_move(app, v, -1, false);
    case UI_KEY_DOWN: return list_move(app, v, 1, false);
    case UI_KEY_PAGEUP: return list_move(app, v, -page, false);
    case UI_KEY_PAGEDOWN: return list_move(app, v, page, false);
    case UI_KEY_HOME: return list_move(app, v, -1, true);
    case UI_KEY_END: return list_move(app, v, 1, true);
    case UI_KEY_ENTER: return list_activate(app, v);
    default: return false;
    }
  }
  return false;
}

bool subviews_handle_mouse(NcApp *app, View *v, const UiEvent *ev, Rect r) {
  if (ev->type != UI_EVENT_MOUSE || !v || v->kind != VIEW_TABLE) return false;
  int x = ev->mouse.x, y = ev->mouse.y;
  Rect s = subviews_rect(app);
  if (s.rows && y == s.y && x >= s.x && x < s.x + s.cols) {
    if (ev->mouse.action == UI_MOUSE_PRESS && ev->mouse.button == UI_MOUSE_LEFT) {
      int sv = hit_find(&app->subview_spans, x);
      if (sv >= 0) subview_set(app, v, (SubView)app->subview_spans.v[sv].value);
    }
    return true;
  }
  if (v->subview == SV_DATA) return false;
  if (y < r.y || y >= r.y + r.rows || x < r.x || x >= r.x + r.cols) return false;
  if (ev->mouse.action != UI_MOUSE_PRESS) return true;
  if (ev->mouse.button == UI_MOUSE_SCROLL_UP) return list_move(app, v, -3, false);
  if (ev->mouse.button == UI_MOUSE_SCROLL_DOWN) return list_move(app, v, 3, false);
  if (ev->mouse.button != UI_MOUSE_LEFT) return true;
  CatalogRows t;
  if (!build_now(app, v, &t)) { catalog_rows_free(&t); return true; }
  int top = r.y + (t.head ? 1 : 0);
  if (y >= top) {
    size_t row = v->sv_scroll[v->subview] + (size_t)(y - top);
    if (row < t.nrows && selectable(&t, row)) v->sv_cursor[v->subview] = row;
  }
  catalog_rows_free(&t);
  return true;
}

bool subviews_do_action(NcApp *app, View *v, HotkeyAction a) {
  if (!v || v->kind != VIEW_TABLE) return false;
  for (int i = 0; i < SV_COUNT; i++)
    if (a == action_of((SubView)i)) {
      subview_set(app, v, (SubView)i);
      return true;
    }
  if (v->subview == SV_DATA) return false;
  switch (a) {
  case HOTKEY_MOVE_UP: return list_move(app, v, -1, false);
  case HOTKEY_MOVE_DOWN: return list_move(app, v, 1, false);
  case HOTKEY_PAGE_UP: case HOTKEY_PAGE_DOWN: {
    Rect r = grid_content_rect(app);
    long page = r.rows > 2 ? r.rows - 2 : 1;
    return list_move(app, v, a == HOTKEY_PAGE_UP ? -page : page, false);
  }
  case HOTKEY_FIRST_ROW: return list_move(app, v, -1, true);
  case HOTKEY_LAST_ROW: return list_move(app, v, 1, true);
  case HOTKEY_REFRESH: app_view_refresh_catalog(app, v); return true;
  case HOTKEY_TOGGLE_FILTERS: case HOTKEY_ADD_FILTER: /* the filters belong to Data: go there */
    if (view_has_subview(v, SV_DATA)) subview_set(app, v, SV_DATA);
    return false;
  default: return false;
  }
}

bool subview_action_applies(const View *v, HotkeyAction a) {
  if (!v || v->kind != VIEW_TABLE) return false;
  for (int i = 0; i < SV_COUNT; i++)
    if (a == action_of((SubView)i)) return view_has_subview(v, (SubView)i);
  if (v->subview == SV_DATA) return true; /* the grid's own rules */
  switch (a) {
  case HOTKEY_MOVE_UP: case HOTKEY_MOVE_DOWN: case HOTKEY_PAGE_UP: case HOTKEY_PAGE_DOWN:
  case HOTKEY_FIRST_ROW: case HOTKEY_LAST_ROW: case HOTKEY_REFRESH: case HOTKEY_TOGGLE_SIDEBAR:
    return true;
  case HOTKEY_TOGGLE_FILTERS: case HOTKEY_ADD_FILTER:
    return view_has_subview(v, SV_DATA);
  default:
    return hotkey_get_category(a) != HOTKEY_CAT_TABLE && hotkey_get_category(a) != HOTKEY_CAT_NAVIGATION &&
           hotkey_get_category(a) != HOTKEY_CAT_FILTERS;
  }
}
