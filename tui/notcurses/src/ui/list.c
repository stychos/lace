#include "ui/list.h"
#include "config/config.h"
#include "ui/input.h"
#include "core/utf8.h"
#include "util/mem.h"
#include "util/str.h"
#include <ctype.h>
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

void list_init(ListWidget *l, bool has_input) {
  memset(l, 0, sizeof *l);
  l->has_input = has_input;
  l->input_focused = has_input;
  l->page = 10;
  text_input_init(&l->input, NULL, false);
}

static void free_rows(ListWidget *l) {
  for (size_t i = 0; i < l->num_rows; i++) {
    free(l->rows[i].text);
    free(l->rows[i].detail);
    free(l->rows[i].keys);
  }
  free(l->rows);
  free(l->shown);
  l->rows = NULL;
  l->shown = NULL;
  l->num_rows = l->num_shown = 0;
}

void list_free(ListWidget *l) {
  free_rows(l);
  text_input_free(&l->input);
}

static bool selectable(const ListWidget *l, size_t pos) {
  return pos < l->num_shown && !l->rows[l->shown[pos]].header;
}

static void cursor_first(ListWidget *l) {
  l->cursor = 0;
  l->scroll = 0;
  for (size_t i = 0; i < l->num_shown; i++)
    if (selectable(l, i)) { l->cursor = i; return; }
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

typedef struct { size_t header; size_t first, count; int best; } Section;

void list_filter(ListWidget *l) {
  free(l->shown);
  l->shown = safe_calloc(l->num_rows ? l->num_rows : 1, sizeof *l->shown);
  l->num_shown = 0;
  const char *needle = l->has_input && l->input.buf ? l->input.buf : "";
  if (!l->has_input) {
    for (size_t i = 0; i < l->num_rows; i++) l->shown[l->num_shown++] = i;
    cursor_first(l);
    return;
  }
  /* Rows grouped under their header; a section appears when a child matches,
   * sections ordered by their best score, children likewise (both stable) */
  int *score = safe_calloc(l->num_rows ? l->num_rows : 1, sizeof *score);
  Section *secs = safe_calloc(l->num_rows + 1, sizeof *secs);
  size_t ns = 0;
  for (size_t i = 0; i < l->num_rows; i++) {
    const ListRow *r = &l->rows[i];
    if (r->header || ns == 0) {
      secs[ns++] = (Section){r->header ? i : (size_t)-1, r->header ? i + 1 : i, 0, -1};
      if (r->header) continue;
    }
    score[i] = l->score ? l->score(needle, r) : (contains_ci(r->text, needle) ? 0 : -1);
    secs[ns - 1].count++;
    if (score[i] > secs[ns - 1].best) secs[ns - 1].best = score[i];
    if (score[i] >= 0 && secs[ns - 1].best < 0) secs[ns - 1].best = 0;
  }
  /* order sections by best score, descending, stable (insertion sort) */
  for (size_t i = 1; i < ns; i++) {
    Section s = secs[i];
    size_t j = i;
    while (j > 0 && secs[j - 1].best < s.best) { secs[j] = secs[j - 1]; j--; }
    secs[j] = s;
  }
  for (size_t k = 0; k < ns; k++) {
    Section *s = &secs[k];
    if (s->best < 0) continue;
    if (s->header != (size_t)-1) l->shown[l->num_shown++] = s->header;
    size_t start = l->num_shown;
    for (size_t i = s->first; i < s->first + s->count; i++) {
      if (score[i] < 0) continue;
      size_t pos = l->num_shown++;
      while (pos > start && score[l->shown[pos - 1]] < score[i]) { l->shown[pos] = l->shown[pos - 1]; pos--; }
      l->shown[pos] = i;
    }
  }
  free(score);
  free(secs);
  cursor_first(l);
}

void list_set_rows(ListWidget *l, ListRow *rows, size_t n) {
  free_rows(l);
  l->rows = rows;
  l->num_rows = n;
  list_filter(l);
}

ListRow *list_selected(ListWidget *l) {
  return selectable(l, l->cursor) ? &l->rows[l->shown[l->cursor]] : NULL;
}

static bool move_to(ListWidget *l, size_t pos) {
  if (pos == l->cursor) return false;
  l->cursor = pos;
  return true;
}

static bool step(ListWidget *l, int dir, size_t count) {
  size_t pos = l->cursor;
  while (count--) {
    size_t p = pos;
    for (;;) {
      if (dir < 0) { if (p == 0) break; p--; }
      else { if (p + 1 >= l->num_shown) break; p++; }
      if (selectable(l, p)) { pos = p; break; }
    }
  }
  return move_to(l, pos);
}

/* A page jump: n display rows away, landing on the nearest selectable row */
static bool jump(ListWidget *l, int dir, size_t n) {
  if (!l->num_shown) return false;
  size_t t = dir < 0 ? (l->cursor > n ? l->cursor - n : 0)
                     : (l->cursor + n < l->num_shown ? l->cursor + n : l->num_shown - 1);
  size_t p = t;
  while (!selectable(l, p) && (dir < 0 ? p + 1 < l->num_shown : p > 0)) p += dir < 0 ? 1 : (size_t)-1;
  if (!selectable(l, p)) return false;
  return move_to(l, p);
}

int list_detail_width(const ListWidget *l, int cols) {
  int cap = cols / 3, w = 0;
  if (l->detail_cols > 0) return l->detail_cols < cap ? l->detail_cols : cap;
  for (size_t i = 0; i < l->num_shown; i++) {
    const ListRow *r = &l->rows[l->shown[i]];
    if (r->header || !r->detail) continue;
    int dw = nc_text_width(r->detail);
    if (dw > w) w = dw;
  }
  return w < cap ? w : cap;
}

static ListAction after_move(bool moved) { return moved ? LIST_CHANGED : LIST_NONE; }

static ListAction go_first(ListWidget *l) {
  size_t keep = l->cursor;
  cursor_first(l);
  l->scroll = 0;
  return after_move(keep != l->cursor);
}

static ListAction go_last(ListWidget *l) {
  size_t pos = l->num_shown;
  while (pos > 0 && !selectable(l, pos - 1)) pos--;
  return pos ? after_move(move_to(l, pos - 1)) : LIST_NONE;
}

static const Config *g_hotkeys;

void list_use_hotkeys(const Config *config) { g_hotkeys = config; }

ListAction list_key(ListWidget *l, const UiEvent *ev) {
  if (ev->type != UI_EVENT_KEY) return LIST_NONE;
  size_t page = l->page > 1 ? (size_t)l->page - 1 : 1;
  bool typing = l->has_input && l->input_focused;
  if (ev->key.is_special && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
    switch (ev->key.key) {
    case UI_KEY_UP: return after_move(step(l, -1, 1));
    case UI_KEY_DOWN: return after_move(step(l, 1, 1));
    case UI_KEY_PAGEUP: return after_move(jump(l, -1, page));
    case UI_KEY_PAGEDOWN: return after_move(jump(l, 1, page));
    case UI_KEY_HOME: if (typing) break;
      return go_first(l);
    case UI_KEY_END: if (typing) break;
      return go_last(l);
    case UI_KEY_ENTER: return list_selected(l) ? LIST_ACTIVATE : LIST_NONE;
    case UI_KEY_ESCAPE: return LIST_CANCEL;
    case UI_KEY_TAB:
    case UI_KEY_BTAB:
      if (l->has_input) l->input_focused = !l->input_focused;
      return LIST_NONE;
    default: break;
    }
  }
  if (g_hotkeys && !typing) { /* the navigation hotkeys (j, k, g, G, ...) when letters are not typed */
    if (hotkey_matches(g_hotkeys, ev, HOTKEY_MOVE_UP)) return after_move(step(l, -1, 1));
    if (hotkey_matches(g_hotkeys, ev, HOTKEY_MOVE_DOWN)) return after_move(step(l, 1, 1));
    if (hotkey_matches(g_hotkeys, ev, HOTKEY_PAGE_UP)) return after_move(jump(l, -1, page));
    if (hotkey_matches(g_hotkeys, ev, HOTKEY_PAGE_DOWN)) return after_move(jump(l, 1, page));
    if (hotkey_matches(g_hotkeys, ev, HOTKEY_FIRST_ROW)) return go_first(l);
    if (hotkey_matches(g_hotkeys, ev, HOTKEY_LAST_ROW)) return go_last(l);
  }
  if (typing) {
    char *before = str_dup(l->input.buf ? l->input.buf : "");
    bool consumed = nc_text_input_key(&l->input, ev);
    bool changed = strcmp(before, l->input.buf ? l->input.buf : "") != 0;
    free(before);
    if (changed) { list_filter(l); return LIST_CHANGED; }
    (void)consumed;
  }
  return LIST_NONE;
}

ListAction list_mouse(ListWidget *l, const UiEvent *ev, int y, int rows) {
  if (ev->type != UI_EVENT_MOUSE) return LIST_NONE;
  int top = y + (l->has_input ? 1 : 0);
  int h = rows - (l->has_input ? 1 : 0);
  if (ev->mouse.button == UI_MOUSE_SCROLL_UP) return after_move(step(l, -1, 3));
  if (ev->mouse.button == UI_MOUSE_SCROLL_DOWN) return after_move(step(l, 1, 3));
  if (ev->mouse.button != UI_MOUSE_LEFT || ev->mouse.action != UI_MOUSE_PRESS) return LIST_NONE;
  if (ev->mouse.y < top || ev->mouse.y >= top + h) return LIST_NONE;
  size_t pos = l->scroll + (size_t)(ev->mouse.y - top);
  if (!selectable(l, pos)) return LIST_NONE;
  l->cursor = pos;
  return LIST_ACTIVATE;
}

static void put_upper(struct ncplane *p, int y, int x, int w, const char *s) {
  char *u = str_dup(s);
  for (char *c = u; *c; c++) *c = (char)toupper((unsigned char)*c);
  nc_put(p, y, x, w, u);
  free(u);
}

void list_draw(ListWidget *l, struct ncplane *p, const Theme *t, int y, int x, int rows, int cols,
               int *cursor_y, int *cursor_x) {
  int top = y;
  if (l->has_input) {
    nc_style(p, t, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
    nc_put(p, y, x, cols, "");
    nc_style(p, t, TC_ACCENT, TC_OVERLAY_BG, 0);
    nc_put(p, y, x + 1, 2, "> ");
    nc_style(p, t, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
    const char *text = l->input.buf ? l->input.buf : "";
    nc_put(p, y, x + 3, cols - 3, text);
    if (cursor_y) *cursor_y = y;
    if (cursor_x) {
      int cw;
      char *head = strndup(text, l->input.cursor < l->input.len ? l->input.cursor : l->input.len);
      cw = head ? nc_text_width(head) : 0;
      free(head);
      *cursor_x = x + 3 + (cw < cols - 4 ? cw : cols - 4);
    }
    top++;
    rows--;
  }
  if (rows < 1) return;
  l->page = rows;
  if (l->cursor < l->scroll) l->scroll = l->cursor;
  if (l->cursor >= l->scroll + (size_t)rows) l->scroll = l->cursor - (size_t)rows + 1;
  /* a header just above the cursor stays in view */
  if (l->cursor > 0 && l->scroll == l->cursor && l->scroll > 0 && !selectable(l, l->scroll - 1))
    l->scroll--;
  int kw = 0;
  for (size_t i = l->scroll; i < l->num_shown && i < l->scroll + (size_t)rows; i++) {
    const ListRow *r = &l->rows[l->shown[i]];
    if (r->keys) { int w = nc_text_width(r->keys); if (w > kw) kw = w; }
  }
  if (kw > cols / 3) kw = cols / 3;
  int dw = list_detail_width(l, cols);
  for (int k = 0; k < rows; k++) {
    size_t i = l->scroll + (size_t)k;
    int ry = top + k;
    if (i >= l->num_shown) {
      nc_style(p, t, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
      nc_put(p, ry, x, cols, "");
      continue;
    }
    const ListRow *r = &l->rows[l->shown[i]];
    if (r->header) {
      nc_style(p, t, TC_DIM, TC_OVERLAY_BG, NCSTYLE_BOLD);
      nc_put(p, ry, x, cols, "");
      put_upper(p, ry, x + 1, cols - 2, r->text);
      continue;
    }
    bool cur = i == l->cursor;
    ThemeColor bg = cur ? TC_CURSOR_BG : TC_OVERLAY_BG;
    nc_style(p, t, cur ? TC_CURSOR_FG : TC_OVERLAY_FG, bg, 0);
    nc_put(p, ry, x, cols, "");
    int right = cols - 1;
    if (kw && r->keys) {
      nc_style(p, t, cur ? TC_CURSOR_FG : TC_KEY_FG, bg, 0);
      nc_put_right(p, ry, x + right - kw, kw, r->keys);
    }
    if (kw) right -= kw + 2;
    if (dw && r->detail) {
      nc_style(p, t, cur ? TC_CURSOR_FG : TC_DIM, bg, 0);
      nc_put_right(p, ry, x + right - dw, dw, r->detail);
    }
    if (dw) right -= dw + 2;
    nc_style(p, t, cur ? TC_CURSOR_FG : TC_OVERLAY_FG, bg, 0);
    nc_put(p, ry, x + 1, right - 1 > 0 ? right - 1 : 0, r->text);
  }
}
