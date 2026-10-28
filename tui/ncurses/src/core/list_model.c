/*
 * Lace
 * The TUIs' list model: see list_model.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "list_model.h"
#include "utf8.h"
#include "../../../liblace/include/util/mem.h"
#include "../../../liblace/include/util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* A key into the filter: its text (the kitty protocol's associated text,
 * notcurses), else the line editor's keys */
static bool input_key(TextInput *in, const UiEvent *ev) {
  if (ev->type == UI_EVENT_KEY && !ev->key.is_special && ev->key.text[0] &&
      !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
    text_input_insert(in, ev->key.text);
    return true;
  }
  return text_input_handle_key(in, ev);
}

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

bool list_selectable(const ListWidget *l, size_t pos) {
  return pos < l->num_shown && !l->rows[l->shown[pos]].header;
}

static void cursor_first(ListWidget *l) {
  l->cursor = 0;
  l->scroll = 0;
  for (size_t i = 0; i < l->num_shown; i++)
    if (list_selectable(l, i)) { l->cursor = i; return; }
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
  return list_selectable(l, l->cursor) ? &l->rows[l->shown[l->cursor]] : NULL;
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
      if (list_selectable(l, p)) { pos = p; break; }
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
  while (!list_selectable(l, p) && (dir < 0 ? p + 1 < l->num_shown : p > 0)) p += dir < 0 ? 1 : (size_t)-1;
  if (!list_selectable(l, p)) return false;
  return move_to(l, p);
}

int list_detail_width(const ListWidget *l, int cols) {
  int cap = cols / 3, w = 0;
  if (l->detail_cols > 0) return l->detail_cols < cap ? l->detail_cols : cap;
  for (size_t i = 0; i < l->num_shown; i++) {
    const ListRow *r = &l->rows[l->shown[i]];
    if (r->header || !r->detail) continue;
    int dw = utf8_width(r->detail, strlen(r->detail));
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
  while (pos > 0 && !list_selectable(l, pos - 1)) pos--;
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
    bool consumed = input_key(&l->input, ev);
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
  if (!list_selectable(l, pos)) return LIST_NONE;
  l->cursor = pos;
  return LIST_ACTIVATE;
}

