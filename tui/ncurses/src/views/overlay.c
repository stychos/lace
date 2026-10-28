/*
 * Lace
 * Overlays: see overlay.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "overlay.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/str.h"
#include <stdlib.h>
#include <string.h>

bool tui_overlay_open(TuiOverlay *o, const char *title, const char *footer, int rows, int cols, int y, int x) {
  memset(o, 0, sizeof *o);
  int sr, sc;
  getmaxyx(stdscr, sr, sc);
  /* the shadow below must not cover the status row */
  int max_rows = sr - 5, max_cols = sc - 4;
  if (max_rows < 1 || max_cols < 4)
    return false;
  o->rows = rows > max_rows ? max_rows : rows < 1 ? 1 : rows;
  o->cols = cols > max_cols ? max_cols : cols < 1 ? 1 : cols;
  o->y = y >= 0 ? y : (sr - (o->rows + 2)) / 2;
  o->x = x >= 0 ? x : (sc - (o->cols + 2)) / 2;
  if (o->y + o->rows + 2 > sr - 1)
    o->y = sr - 1 - (o->rows + 2);
  if (o->x + o->cols + 2 > sc)
    o->x = sc - (o->cols + 2);
  if (o->y < 0)
    o->y = 0;
  if (o->x < 0)
    o->x = 0;
  int shadow_cols = o->x + o->cols + 4 <= sc ? o->cols + 2 : sc - o->x - 2;
  if (shadow_cols > 0 && o->y + o->rows + 3 <= sr - 1)
    o->shadow = newwin(o->rows + 2, shadow_cols, o->y + 1, o->x + 2);
  o->win = newwin(o->rows + 2, o->cols + 2, o->y, o->x);
  o->status = newwin(1, sc, sr - 1, 0);
  if (!o->win) {
    tui_overlay_close(o);
    return false;
  }
  keypad(o->win, TRUE);
  o->title = str_dup(title ? title : "");
  o->footer = str_dup(footer ? footer : "");
  return true;
}

void tui_overlay_set_footer(TuiOverlay *o, const char *footer) {
  free(o->footer);
  o->footer = str_dup(footer ? footer : "");
}

static void put_label(WINDOW *w, int y, int x, int max, const char *text) {
  char *label = str_printf(" %s ", text);
  int lw = utf8_width(label, strlen(label));
  tui_print_clip(w, y, x, lw < max ? lw : max, label);
  free(label);
}

void tui_frame_rect(WINDOW *win, int y0, int x0, int h, int w, const char *title, const char *footer) {
  if (!win || h < 2 || w < 2)
    return;
  tui_style(win, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
  for (int y = y0 + 1; y < y0 + h - 1; y++)
    tui_print_fit(win, y, x0 + 1, w - 2, "");
  tui_style(win, TC_OVERLAY_BORDER, TC_OVERLAY_BG, 0);
  mvwaddstr(win, y0, x0, "╭");
  mvwaddstr(win, y0, x0 + w - 1, "╮");
  mvwaddstr(win, y0 + h - 1, x0, "╰");
  mvwaddstr(win, y0 + h - 1, x0 + w - 1, "╯");
  for (int x = x0 + 1; x < x0 + w - 1; x++) {
    mvwaddstr(win, y0, x, "─");
    mvwaddstr(win, y0 + h - 1, x, "─");
  }
  for (int y = y0 + 1; y < y0 + h - 1; y++) {
    mvwaddstr(win, y, x0, "│");
    mvwaddstr(win, y, x0 + w - 1, "│");
  }
  if (title && *title) {
    tui_style(win, TC_ACCENT, TC_OVERLAY_BG, A_BOLD);
    put_label(win, y0, x0 + 2, w - 4, title);
  }
  if (footer && *footer) {
    tui_style(win, TC_KEY_FG, TC_OVERLAY_BG, 0);
    put_label(win, y0 + h - 1, x0 + 2, w - 4, footer);
  }
  tui_style(win, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
}

void tui_frame_window(WINDOW *win, const char *title, const char *footer) {
  if (!win)
    return;
  wbkgd(win, tui_theme_attr(TC_OVERLAY_FG, TC_OVERLAY_BG) | ' ');
  werase(win);
  int h, w;
  getmaxyx(win, h, w);
  tui_frame_rect(win, 0, 0, h, w, title, footer);
}

void tui_overlay_frame(TuiOverlay *o) {
  if (!o->win)
    return;
  if (o->shadow) {
    wbkgd(o->shadow, tui_theme_attr(TC_SHADOW_BG, TC_SHADOW_BG) | ' ');
    werase(o->shadow);
  }
  wbkgd(o->win, tui_theme_attr(TC_OVERLAY_FG, TC_OVERLAY_BG) | ' ');
  werase(o->win);
  int h = o->rows + 2, w = o->cols + 2;
  tui_style(o->win, TC_OVERLAY_BORDER, TC_OVERLAY_BG, 0);
  mvwaddstr(o->win, 0, 0, "╭");
  mvwaddstr(o->win, 0, w - 1, "╮");
  mvwaddstr(o->win, h - 1, 0, "╰");
  mvwaddstr(o->win, h - 1, w - 1, "╯");
  for (int x = 1; x < w - 1; x++) {
    mvwaddstr(o->win, 0, x, "─");
    mvwaddstr(o->win, h - 1, x, "─");
  }
  for (int y = 1; y < h - 1; y++) {
    mvwaddstr(o->win, y, 0, "│");
    mvwaddstr(o->win, y, w - 1, "│");
  }
  if (o->title && *o->title) {
    tui_style(o->win, TC_ACCENT, TC_OVERLAY_BG, A_BOLD);
    put_label(o->win, 0, 2, w - 4, o->title);
  }
  if (o->footer && *o->footer) {
    tui_style(o->win, TC_KEY_FG, TC_OVERLAY_BG, 0);
    put_label(o->win, h - 1, 2, w - 4, o->footer);
  }
  if (o->status) {
    wbkgd(o->status, tui_theme_attr(TC_STATUS_FG, TC_STATUS_BG) | ' ');
    werase(o->status);
    tui_style(o->status, TC_STATUS_FG, TC_STATUS_BG, 0);
    int sc = getmaxx(o->status);
    tui_print_fit(o->status, 0, 1, sc - 1, o->footer ? o->footer : "");
  }
  tui_style(o->win, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
}

void tui_overlay_put(TuiOverlay *o, int row, int col, int width, ThemeColor fg, attr_t attrs, const char *text) {
  if (!o->win || row < 0 || row >= o->rows || col < 0 || col >= o->cols)
    return;
  if (col + width > o->cols)
    width = o->cols - col;
  tui_style(o->win, fg, TC_OVERLAY_BG, attrs);
  tui_print_fit(o->win, row + 1, col + 1, width, text);
  tui_style(o->win, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
}

void tui_overlay_refresh(TuiOverlay *o) {
  if (o->shadow)
    wnoutrefresh(o->shadow);
  if (o->status)
    wnoutrefresh(o->status);
  if (o->win)
    wnoutrefresh(o->win);
  doupdate();
}

void tui_overlay_close(TuiOverlay *o) {
  if (o->win)
    delwin(o->win);
  if (o->shadow)
    delwin(o->shadow);
  if (o->status)
    delwin(o->status);
  free(o->title);
  free(o->footer);
  memset(o, 0, sizeof *o);
  touchwin(stdscr);
}

bool tui_overlay_contains(const TuiOverlay *o, int y, int x) {
  return o->win && y >= o->y && y < o->y + o->rows + 2 && x >= o->x && x < o->x + o->cols + 2;
}
