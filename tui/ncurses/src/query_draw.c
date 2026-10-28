/*
 * Lace
 * Query tab drawing, as the notcurses TUI's query view: the editor (line
 * numbers, SQL highlighting) over a divider that carries the result's
 * summary ("3 rows", "1,000+ rows", "2 rows affected", the error), the
 * results grid below it
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include "core/chrome.h"
#include "core/sqlhl.h"
#include "query_internal.h"
#include "views/theme.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int query_editor_height(const Tab *tab, int win_rows) {
  int er = tab && tab->query_editor_rows > 0 ? tab->query_editor_rows : win_rows / 3; /* a third, as notcurses */
  if (er < 3)
    er = 3;
  int maxer = win_rows - 3;
  if (maxer < 1)
    maxer = 1;
  if (er > maxer)
    er = maxer;
  return er < 1 ? 1 : er;
}

int query_visible_result_rows(TuiState *state) {
  int rows = getmaxy(state->main_win);
  int visible = rows - query_editor_height(TUI_TAB(state), rows) - 1 - QUERY_RESULTS_HEADER_ROWS;
  return visible < 1 ? 1 : visible;
}

static int digits(size_t n) {
  int d = 1;
  while (n >= 10) {
    n /= 10;
    d++;
  }
  return d;
}

static void style_for(WINDOW *w, HlClass c) {
  switch (c) {
  case HL_KEYWORD: tui_style(w, TC_ACCENT, TC_BG, A_BOLD); break;
  case HL_STRING: tui_style(w, TC_KEY_FG, TC_BG, 0); break;
  case HL_COMMENT: tui_style(w, TC_DIM, TC_BG, 0); break;
  case HL_NUMBER: tui_style(w, TC_NUMBER_FG, TC_BG, 0); break;
  default: tui_style(w, TC_FG, TC_BG, 0); break;
  }
}

static void draw_editor(TuiState *state, Tab *tab, int rows, int cols, bool focused) {
  WINDOW *w = state->main_win;
  tui_style(w, TC_FG, TC_BG, 0);
  for (int y = 0; y < rows; y++)
    tui_print_fit(w, y, 0, cols, "");
  QueryLineInfo *lines = NULL;
  size_t nlines = 0;
  query_rebuild_line_cache(tab, &lines, &nlines);
  if (nlines == 0) {
    free(lines);
    return;
  }
  int gw = digits(nlines) + 1;
  int tw = cols - gw;
  size_t cl, cc;
  query_cursor_to_line_col(tab, &cl, &cc);
  if (cl < tab->query_scroll_line)
    tab->query_scroll_line = cl;
  else if (cl >= tab->query_scroll_line + (size_t)rows)
    tab->query_scroll_line = cl - (size_t)rows + 1;
  size_t sl = tab->query_scroll_line;
  if (tw < 1 || rows < 1) {
    free(lines);
    return;
  }

  tui_style(w, TC_DIM, TC_BG, 0);
  for (size_t i = 0; i < (size_t)rows && sl + i < nlines; i++) {
    char num[32];
    snprintf(num, sizeof num, "%*zu", gw - 1, sl + i + 1);
    tui_print_fit(w, (int)i, 0, gw - 1, num);
  }

  /* The text, token by token in its class's colour (tabs 4 wide) */
  const char *text = tab->query_text ? tab->query_text : "";
  size_t len = tab->query_text ? tab->query_len : 0;
  LaceDriver d = TUI_CONN(state) ? TUI_CONN(state)->driver_type : LACE_DRIVER_SQLITE;
  size_t pos = 0, line = 0, col = 0;
  while (pos < len && line < sl + (size_t)rows) {
    HlClass cls;
    size_t n = sqlhl_next(text, pos, d, &cls);
    if (n == 0)
      break;
    size_t end = pos + n > len ? len : pos + n;
    for (size_t i = pos; i < end;) {
      if (text[i] == '\n') {
        line++;
        col = 0;
        i++;
        continue;
      }
      size_t nx = utf8_next(text, len, i);
      if (nx <= i)
        nx = i + 1;
      bool tab_ch = text[i] == '\t';
      unsigned char c = (unsigned char)text[i];
      size_t cw = tab_ch ? 4 : (c < 32 || c == 127) ? 0 : (size_t)utf8_width(text + i, nx - i);
      if (line >= sl && cw > 0 && col + cw <= (size_t)tw) {
        style_for(w, cls);
        if (tab_ch)
          mvwaddstr(w, (int)(line - sl), gw + (int)col, "    ");
        else
          mvwaddnstr(w, (int)(line - sl), gw + (int)col, text + i, (int)(nx - i));
      }
      col += cw;
      i = nx;
    }
    pos = end;
  }

  /* The cursor, in reverse video */
  if (focused && cl >= sl && cl < sl + (size_t)rows && cl < nlines) {
    const QueryLineInfo *li = &lines[cl];
    const char *lt = text + li->start;
    size_t ccol = cc < li->len ? cc : li->len;
    int cx = gw + (int)query_display_width(lt, ccol);
    if (cx < cols) {
      tui_style(w, TC_BG, TC_FG, 0);
      if (ccol < li->len && (unsigned char)lt[ccol] >= 32 && lt[ccol] != 127)
        mvwaddnstr(w, (int)(cl - sl), cx, lt + ccol, (int)(utf8_next(lt, li->len, ccol) - ccol));
      else
        mvwaddch(w, (int)(cl - sl), cx, ' ');
    }
  }
  tui_style(w, TC_FG, TC_BG, 0);
  free(lines);
}

/* "3 rows", "1,000+ rows", "2 rows affected", the error, or "" */
static char *summary(const Tab *tab, bool *error) {
  *error = false;
  if (tab->query_error) {
    *error = true;
    char *s = str_dup(tab->query_error);
    for (char *c = s; *c; c++)
      if (*c == '\n' || *c == '\r' || *c == '\t')
        *c = ' ';
    return s;
  }
  if (!tab->query_results && tab->query_exec_success)
    return str_printf("%lld %s affected", (long long)tab->query_affected, tab->query_affected == 1 ? "row" : "rows");
  if (tab->query_results && tab->query_results->num_columns > 0) {
    bool known = true;
    size_t n = tab->query_results->num_rows;
    if (tab->query_paginated) {
      known = tab->query_total_known;
      n = known ? tab->query_total_rows : tab->query_loaded_offset + tab->query_loaded_count;
    }
    char *c = chrome_commas(n);
    char *out = str_printf("%s%s %s", c, known ? "" : "+", n == 1 && known ? "row" : "rows");
    free(c);
    return out;
  }
  return str_dup("");
}

static void draw_divider(TuiState *state, Tab *tab, int y, int cols, bool results_focused) {
  WINDOW *w = state->main_win;
  tui_style(w, results_focused ? TC_ACCENT : TC_DIM, TC_BG, 0);
  for (int i = 0; i < cols; i++)
    mvwaddstr(w, y, i, "\xe2\x94\x80");
  bool err;
  char *s = summary(tab, &err);
  if (*s && cols > 6) {
    char *label = str_printf(" %s ", s);
    int lw = utf8_width(label, strlen(label));
    if (lw > cols - 4)
      lw = cols - 4;
    tui_style(w, err ? TC_ERROR_FG : TC_FG, TC_BG, 0);
    tui_print_fit(w, y, 2, lw, label);
    free(label);
  }
  free(s);
  tui_style(w, TC_FG, TC_BG, 0);
}

/* What the results area says without rows to show */
static void draw_placeholder(TuiState *state, Tab *tab, int y0, int rows, int cols) {
  WINDOW *w = state->main_win;
  if (rows < 1)
    return;
  const char *msg = tab->query_error         ? tab->query_error
                    : tab->query_exec_success ? ""
                    : tab->query_results      ? "No rows"
                                              : "^R runs the statement at the cursor";
  if (!*msg)
    return;
  /* wrapped and centred, so a long error is read whole */
  char *flat = str_dup(msg);
  for (char *c = flat; *c; c++)
    if (*c == '\r' || *c == '\t')
      *c = ' ';
  char **lines;
  size_t n = chrome_wrap(flat, cols - 4 > 1 ? cols - 4 : 1, &lines), shown = n;
  if (shown > (size_t)rows)
    shown = (size_t)rows;
  tui_style(w, tab->query_error ? TC_ERROR_FG : TC_DIM, TC_BG, 0);
  int top = y0 + (rows - (int)shown) / 2;
  for (size_t i = 0; i < shown; i++) {
    int mw = utf8_width(lines[i], strlen(lines[i]));
    tui_print_fit(w, top + (int)i, (cols - mw) / 2 > 0 ? (cols - mw) / 2 : 0, mw, lines[i]);
  }
  chrome_wrap_free(lines, n);
  free(flat);
  tui_style(w, TC_FG, TC_BG, 0);
}

void tui_draw_query(TuiState *state) {
  if (!state || !state->main_win)
    return;
  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || tab->type != TAB_TYPE_QUERY || !ui)
    return;
  WINDOW *w = state->main_win;
  wbkgd(w, tui_theme_attr(TC_FG, TC_BG) | ' ');
  werase(w);
  int win_rows, win_cols;
  getmaxyx(w, win_rows, win_cols);
  bool focus_results = ui->query_focus_results && !ui->query_unfocused;
  bool focus_editor = !ui->query_unfocused && !ui->query_focus_results;
  int er = query_editor_height(tab, win_rows);
  draw_editor(state, tab, er, win_cols, focus_editor);
  draw_divider(state, tab, er, win_cols, focus_results);
  int results_y = er + 1, results_rows = win_rows - results_y;
  tui_style(w, TC_FG, TC_BG, 0);
  for (int y = results_y; y < win_rows; y++)
    tui_print_fit(w, y, 0, win_cols, "");
  if (tab->query_results && tab->query_results->num_columns > 0 && !tab->query_error) {
    GridDrawParams params = {.win = w,
                             .start_y = results_y,
                             .start_x = 0,
                             .height = results_rows,
                             .width = win_cols,
                             .data = tab->query_results,
                             .col_widths = tab->query_result_col_widths,
                             .num_col_widths = tab->query_result_num_cols,
                             .cursor_row = tab->query_result_row,
                             .cursor_col = tab->query_result_col,
                             .scroll_row = tab->query_result_scroll_row,
                             .scroll_col = tab->query_result_scroll_col,
                             .selection_offset = tab->query_loaded_offset,
                             .is_focused = focus_results,
                             .is_editing = ui->query_result_editing,
                             .edit_buffer = ui->query_result_edit_buf,
                             .edit_pos = ui->query_result_edit_pos,
                             .show_header_line = false,
                             .sort_entries = NULL,
                             .num_sort_entries = 0};
    tui_draw_result_grid(state, &params);
  } else {
    draw_placeholder(state, tab, results_y, results_rows, win_cols);
  }
  wrefresh(w);
}
