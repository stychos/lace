#include "ui/list.h"
#include "util/str.h"
#include <ctype.h>
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

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
  if (!l->hide_cursor) {
    if (l->cursor < l->scroll) l->scroll = l->cursor;
    if (l->cursor >= l->scroll + (size_t)rows) l->scroll = l->cursor - (size_t)rows + 1;
    /* a header just above the cursor stays in view */
    if (l->cursor > 0 && l->scroll == l->cursor && l->scroll > 0 && !list_selectable(l, l->scroll - 1))
      l->scroll--;
  }
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
    bool cur = !l->hide_cursor && i == l->cursor;
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
