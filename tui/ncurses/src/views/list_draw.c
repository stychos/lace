/*
 * Lace
 * The shared list drawn in an overlay: see list_draw.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "list_draw.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void put(TuiOverlay *o, int y, int x, int w, ThemeColor fg, ThemeColor bg, attr_t a, const char *s) {
  if (w <= 0 || !o->win)
    return;
  tui_style(o->win, fg, bg, a);
  tui_print_fit(o->win, y + 1, x + 1, w, s);
}

static void put_right(TuiOverlay *o, int y, int x, int w, ThemeColor fg, ThemeColor bg, const char *s) {
  int tw = utf8_width(s, strlen(s));
  if (tw >= w) {
    put(o, y, x, w, fg, bg, 0, s);
    return;
  }
  put(o, y, x, w - tw, fg, bg, 0, "");
  put(o, y, x + w - tw, tw, fg, bg, 0, s);
}

void tui_list_draw(ListWidget *l, TuiOverlay *o, int y, int x, int rows, int cols, int *cursor_y, int *cursor_x) {
  int top = y;
  if (l->has_input) {
    put(o, y, x, cols, TC_OVERLAY_FG, TC_OVERLAY_BG, 0, "");
    put(o, y, x + 1, 2, TC_ACCENT, TC_OVERLAY_BG, 0, "> ");
    const char *text = l->input.buf ? l->input.buf : "";
    put(o, y, x + 3, cols - 3, TC_OVERLAY_FG, TC_OVERLAY_BG, 0, text);
    if (cursor_y)
      *cursor_y = y;
    if (cursor_x) {
      size_t cur = l->input.cursor < l->input.len ? l->input.cursor : l->input.len;
      int cw = utf8_width(text, cur);
      *cursor_x = x + 3 + (cw < cols - 4 ? cw : cols - 4);
    }
    top++;
    rows--;
  }
  if (rows < 1)
    return;
  l->page = rows;
  if (!l->hide_cursor) {
    if (l->cursor < l->scroll)
      l->scroll = l->cursor;
    if (l->cursor >= l->scroll + (size_t)rows)
      l->scroll = l->cursor - (size_t)rows + 1;
    /* a header just above the cursor stays in view */
    if (l->cursor > 0 && l->scroll == l->cursor && l->scroll > 0 && !list_selectable(l, l->scroll - 1))
      l->scroll--;
  }
  int kw = 0;
  for (size_t i = l->scroll; i < l->num_shown && i < l->scroll + (size_t)rows; i++) {
    const ListRow *r = &l->rows[l->shown[i]];
    if (r->keys) {
      int w = utf8_width(r->keys, strlen(r->keys));
      if (w > kw)
        kw = w;
    }
  }
  if (kw > cols / 3)
    kw = cols / 3;
  int dw = list_detail_width(l, cols);
  for (int k = 0; k < rows; k++) {
    size_t i = l->scroll + (size_t)k;
    int ry = top + k;
    if (i >= l->num_shown) {
      put(o, ry, x, cols, TC_OVERLAY_FG, TC_OVERLAY_BG, 0, "");
      continue;
    }
    const ListRow *r = &l->rows[l->shown[i]];
    if (r->header) {
      put(o, ry, x, cols, TC_DIM, TC_OVERLAY_BG, 0, "");
      char *u = str_dup(r->text);
      for (char *c = u; *c; c++)
        *c = (char)toupper((unsigned char)*c);
      put(o, ry, x + 1, cols - 2, TC_DIM, TC_OVERLAY_BG, A_BOLD, u);
      free(u);
      continue;
    }
    bool cur = !l->hide_cursor && i == l->cursor;
    ThemeColor bg = cur ? TC_CURSOR_BG : TC_OVERLAY_BG;
    ThemeColor fg = cur ? TC_CURSOR_FG : TC_OVERLAY_FG;
    put(o, ry, x, cols, fg, bg, 0, "");
    int right = cols - 1;
    if (kw && r->keys)
      put_right(o, ry, x + right - kw, kw, cur ? TC_CURSOR_FG : TC_KEY_FG, bg, r->keys);
    if (kw)
      right -= kw + 2;
    if (dw && r->detail)
      put_right(o, ry, x + right - dw, dw, cur ? TC_CURSOR_FG : TC_DIM, bg, r->detail);
    if (dw)
      right -= dw + 2;
    put(o, ry, x + 1, right - 1 > 0 ? right - 1 : 0, fg, bg, 0, r->text);
  }
  tui_style(o->win, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
}

ListAction tui_list_mouse(ListWidget *l, const TuiOverlay *o, const UiEvent *ev, int y, int rows) {
  if (ev->type != UI_EVENT_MOUSE)
    return LIST_NONE;
  UiEvent c = *ev;
  c.mouse.x = ev->mouse.x - o->x - 1; /* content coordinates */
  c.mouse.y = ev->mouse.y - o->y - 1;
  if (c.mouse.action == UI_MOUSE_CLICK || c.mouse.action == UI_MOUSE_DOUBLE_CLICK)
    c.mouse.action = UI_MOUSE_PRESS; /* ncurses reports clicks, the list takes presses */
  return list_mouse(l, &c, y, rows);
}

ListRow *tui_list_overlay_run(TuiOverlay *o, ListWidget *l, bool (*activate)(ListRow *row, void *ctx), void *ctx) {
  ListRow *closed_on = NULL;
  if (l->has_input)
    curs_set(1);
  for (;;) {
    tui_overlay_frame(o);
    int cy = 0, cx = 0;
    tui_list_draw(l, o, 0, 0, o->rows, o->cols, &cy, &cx);
    if (l->has_input)
      wmove(o->win, cy + 1, cx + 1);
    tui_overlay_refresh(o);
    int ch = wgetch(o->win);
    UiEvent ev;
    if (!render_translate_key(ch, &ev))
      continue;
    ListAction a = LIST_NONE;
    if (ev.type == UI_EVENT_RESIZE)
      break; /* the caller lays the screen out again */
    if (ev.type == UI_EVENT_MOUSE) {
      bool wheel = ev.mouse.button == UI_MOUSE_SCROLL_UP || ev.mouse.button == UI_MOUSE_SCROLL_DOWN;
      if (!tui_overlay_contains(o, ev.mouse.y, ev.mouse.x)) {
        if (!wheel)
          break; /* a click outside closes */
        continue;
      }
      a = tui_list_mouse(l, o, &ev, 0, o->rows);
    } else {
      a = list_key(l, &ev);
    }
    if (a == LIST_CANCEL)
      break;
    if (a == LIST_ACTIVATE) {
      ListRow *r = list_selected(l);
      if (r && (!activate || activate(r, ctx))) {
        closed_on = r;
        break;
      }
    }
  }
  if (l->has_input)
    curs_set(0);
  return closed_on;
}
