#include "ui/query_view.h"
#include "core/utf8.h"
#include "edit_ops.h"
#include "model/workspace.h"
#include "ui/input.h"
#include "ui/sqlhl.h"
#include "ui/textbuf.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { Rect editor; int div_y; Rect results; } Layout;

static struct { bool valid; int y, x; } g_cursor;

static void layout(const View *v, Rect r, Layout *L) {
  int er = v->editor_rows > 0 ? v->editor_rows : r.rows / 3;
  if (er < 3) er = 3;
  int maxer = r.rows - 3;
  if (maxer < 1) maxer = 1;
  if (er > maxer) er = maxer;
  if (er < 1) er = 1;
  L->editor = (Rect){r.y, r.x, er, r.cols};
  L->div_y = r.y + er;
  int rr = r.rows - er - 1;
  L->results = (Rect){L->div_y + 1, r.x, rr > 0 ? rr : 0, r.cols};
}

Rect query_view_results_rect(const NcApp *app, const View *v) {
  Layout L;
  layout(v, grid_content_rect(app), &L);
  return L.results;
}

bool query_view_cursor(int *y, int *x) {
  if (!g_cursor.valid) return false;
  *y = g_cursor.y;
  *x = g_cursor.x;
  return true;
}

/* ==========================================================================
 * The text, as a TextBuf lent from the view
 * ========================================================================== */

static void tb_load(TextBuf *tb, View *v) {
  if (!v->text) v->text = str_dup("");
  textbuf_adopt(tb, v->text, v->text_cursor, v->text_want_col);
}

static void tb_store(const TextBuf *tb, View *v) {
  v->text = tb->text;
  v->text_cursor = tb->cursor;
  v->text_want_col = tb->want_col;
}

static LaceDriver view_driver(NcApp *app, const View *v) {
  ConnTab *t = app_view_tab(app, v);
  return t && t->conn ? nc_connection_driver(t->conn) : LACE_DRIVER_SQLITE;
}

static int digits(size_t n) {
  int d = 1;
  while (n >= 10) { n /= 10; d++; }
  return d;
}

static bool has_selection(const View *v) {
  return v->text_sel_anchor != SIZE_MAX && v->text_sel_anchor != v->text_cursor;
}

static bool selection(const View *v, size_t *a, size_t *b) {
  if (v->text_sel_anchor == SIZE_MAX || v->text_sel_anchor == v->text_cursor) return false;
  size_t x = v->text_sel_anchor, y = v->text_cursor;
  *a = x < y ? x : y;
  *b = x < y ? y : x;
  return true;
}

/* ==========================================================================
 * Drawing
 * ========================================================================== */

static void style_for(NcApp *app, HlClass c) {
  const Theme *t = &app->theme;
  switch (c) {
  case HL_KEYWORD: nc_style(app->std, t, TC_ACCENT, TC_BG, NCSTYLE_BOLD); break;
  case HL_STRING: nc_style(app->std, t, TC_KEY_FG, TC_BG, 0); break;
  case HL_COMMENT: nc_style(app->std, t, TC_DIM, TC_BG, 0); break;
  case HL_NUMBER: nc_style(app->std, t, TC_NUMBER_FG, TC_BG, 0); break;
  default: nc_style(app->std, t, TC_FG, TC_BG, 0); break;
  }
}

static void draw_editor(NcApp *app, View *v, Rect ed, bool focused) {
  struct ncplane *p = app->std;
  const Theme *t = &app->theme;
  TextBuf tb;
  tb_load(&tb, v);
  nc_style(p, t, TC_FG, TC_BG, 0);
  for (int y = ed.y; y < ed.y + ed.rows; y++) nc_put(p, y, ed.x, ed.cols, "");
  size_t nlines = textbuf_num_lines(&tb);
  int gw = digits(nlines) + 1;
  int tw = ed.cols - gw;
  if (tw < 1 || ed.rows < 1) return;
  size_t cl, cc;
  textbuf_line_col(&tb, &cl, &cc);
  size_t rows = (size_t)ed.rows;
  if (cl < v->text_scroll_line) v->text_scroll_line = cl;
  else if (cl >= v->text_scroll_line + rows) v->text_scroll_line = cl - rows + 1;
  if (cc < v->text_scroll_col) v->text_scroll_col = cc;
  else if (cc >= v->text_scroll_col + (size_t)tw) v->text_scroll_col = cc - (size_t)tw + 1;
  size_t sl = v->text_scroll_line, sc = v->text_scroll_col;

  nc_style(p, t, TC_DIM, TC_BG, 0);
  for (size_t i = 0; i < rows && sl + i < nlines; i++) {
    char num[32];
    snprintf(num, sizeof num, "%*zu", gw - 1, sl + i + 1);
    nc_put(p, ed.y + (int)i, ed.x, gw - 1, num);
  }

  LaceDriver d = view_driver(app, v);
  size_t sel_a = 0, sel_b = 0;
  bool has_sel = selection(v, &sel_a, &sel_b);
  size_t pos = 0, line = 0, col = 0;
  while (pos < tb.len && line < sl + rows) {
    HlClass cls;
    size_t n = sqlhl_next(tb.text, pos, d, &cls);
    if (n == 0) break;
    size_t end = pos + n;
    for (size_t i = pos; i < end;) {
      if (tb.text[i] == '\n') {
        line++;
        col = 0;
        i++;
        continue;
      }
      size_t nx = utf8_next(tb.text, tb.len, i);
      if (nx <= i) nx = i + 1;
      bool tab = tb.text[i] == '\t';
      size_t w = tab ? 4 : (size_t)utf8_width(tb.text + i, nx - i);
      if (line >= sl && line < sl + rows && col >= sc && col + w <= sc + (size_t)tw && w > 0) {
        if (has_sel && i >= sel_a && i < sel_b) nc_style(p, t, TC_BG, TC_ACCENT, 0);
        else style_for(app, cls);
        char ch[16];
        size_t cn = nx - i < sizeof ch - 1 ? nx - i : sizeof ch - 1;
        if (tab) memset(ch, ' ', 4), ch[4] = 0;
        else memcpy(ch, tb.text + i, cn), ch[cn] = 0;
        nc_put(p, ed.y + (int)(line - sl), ed.x + gw + (int)(col - sc), (int)w, ch);
      }
      col += w;
      i = nx;
    }
    pos = end;
  }
  if (focused) {
    g_cursor.valid = true;
    g_cursor.y = ed.y + (int)(cl - sl);
    g_cursor.x = ed.x + gw + (int)(cc - sc);
  }
}

/* "3,000 rows", "1,000+ rows", "3 rows affected", the error, or "" */
static char *summary(const View *v, bool *error) {
  *error = false;
  if (v->running) return str_dup("Running\xe2\x80\xa6");
  if (v->error) {
    *error = true;
    char *s = str_dup(v->error);
    for (char *c = s; *c; c++)
      if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    return s;
  }
  if (v->affected >= 0)
    return str_printf("%lld %s affected", (long long)v->affected, v->affected == 1 ? "row" : "rows");
  if (page_cache_num_columns(v->cache) > 0) {
    size_t n = page_cache_extent(v->cache);
    char raw[32];
    snprintf(raw, sizeof raw, "%zu", n);
    char out[48];
    size_t len = strlen(raw), o = 0;
    for (size_t i = 0; i < len; i++) {
      if (i && (len - i) % 3 == 0) out[o++] = ',';
      out[o++] = raw[i];
    }
    out[o] = 0;
    bool known = page_cache_total_known(v->cache);
    return str_printf("%s%s%s %s", known && page_cache_total_approximate(v->cache) ? "\xe2\x89\x88" : "",
                      out, known ? "" : "+", n == 1 && known ? "row" : "rows");
  }
  return str_dup("");
}

static void draw_divider(NcApp *app, View *v, int y, int x, int w) {
  const Theme *t = &app->theme;
  struct ncplane *p = app->std;
  nc_style(p, t, v->focus_results ? TC_ACCENT : TC_DIM, TC_BG, 0);
  for (int i = 0; i < w; i++) nc_put(p, y, x + i, 1, "\xe2\x94\x80");
  bool err;
  char *s = summary(v, &err);
  if (*s && w > 6) {
    char *label = str_printf(" %s ", s);
    int lw = nc_text_width(label);
    if (lw > w - 4) lw = w - 4;
    nc_style(p, t, err ? TC_ERROR_FG : TC_FG, TC_BG, 0);
    nc_put(p, y, x + 2, lw, label);
    free(label);
  }
  free(s);
}

static void draw_placeholder(NcApp *app, const View *v, Rect r) {
  nc_style(app->std, &app->theme, TC_FG, TC_BG, 0);
  for (int y = r.y; y < r.y + r.rows; y++) nc_put(app->std, y, r.x, r.cols, "");
  if (r.rows < 1) return;
  const char *msg = v->running ? "Running\xe2\x80\xa6"
                    : v->error ? v->error
                    : v->affected >= 0 ? ""
                    : v->ran ? "No rows" : "^R runs the statement at the cursor";
  if (!*msg) return;
  char *flat = str_dup(msg);
  for (char *c = flat; *c; c++)
    if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
  nc_style(app->std, &app->theme, v->error && !v->running ? TC_ERROR_FG : TC_DIM, TC_BG, 0);
  int w = nc_text_width(flat);
  if (w > r.cols - 2) w = r.cols - 2;
  if (w > 0) nc_put(app->std, r.y + (r.rows - 1) / 2, r.x + (r.cols - w) / 2, w, flat);
  free(flat);
}

void query_view_draw(NcApp *app, View *v, Rect r) {
  g_cursor.valid = false;
  grid_edit_begin_frame();
  Layout L;
  layout(v, r, &L);
  draw_editor(app, v, L.editor, !v->focus_results);
  draw_divider(app, v, L.div_y, r.x, r.cols);
  if (page_cache_num_columns(v->cache) > 0) grid_draw(app, v, L.results);
  else draw_placeholder(app, v, L.results);
}

/* ==========================================================================
 * Keys
 * ========================================================================== */

static void delete_selection(TextBuf *tb, View *v) {
  size_t a, b;
  if (!selection(v, &a, &b)) return;
  memmove(tb->text + a, tb->text + b, tb->len - b + 1);
  tb->len -= b - a;
  tb->cursor = a;
  v->text_sel_anchor = SIZE_MAX;
}

static void run_at_cursor(NcApp *app, View *v) {
  TextBuf tb;
  tb_load(&tb, v);
  size_t a, b;
  char *sql = NULL;
  if (selection(v, &a, &b)) {
    sql = str_ndup(tb.text + a, b - a);
  } else if (textbuf_statement_at(&tb, view_driver(app, v), &a, &b)) {
    sql = str_ndup(tb.text + a, b - a);
  }
  if (!sql) {
    app_set_status(app, false, "No statement at the cursor");
    return;
  }
  app_query_run(app, v, sql);
  str_secure_free(sql);
}

static void set_split(View *v, Rect r, int er) {
  int maxer = r.rows - 3;
  if (er < 3) er = 3;
  if (er > maxer) er = maxer;
  if (er < 1) er = 1;
  v->editor_rows = er;
}

static void resize_split(NcApp *app, View *v, int delta) {
  Layout L;
  Rect r = grid_content_rect(app);
  layout(v, r, &L);
  set_split(v, r, L.editor.rows + delta);
}

/* The mouse over the whole content area r: a press on the divider starts a
 * drag that sets the editor's height, a click in the editor puts the cursor
 * there, the wheel moves it; anything below the divider is the grid's */
bool query_view_handle_mouse(NcApp *app, View *v, const UiEvent *ev, Rect r) {
  (void)app;
  if (ev->type != UI_EVENT_MOUSE) return false;
  Layout L;
  layout(v, r, &L);
  int x = ev->mouse.x, y = ev->mouse.y;
  if (v->split_drag) {
    if (ev->mouse.action == UI_MOUSE_RELEASE) v->split_drag = false;
    else set_split(v, r, y - r.y);
    return true;
  }
  if (x < r.x || x >= r.x + r.cols || y < r.y || y >= r.y + r.rows) return false;
  bool left = ev->mouse.button == UI_MOUSE_LEFT && ev->mouse.action == UI_MOUSE_PRESS;
  if (y == L.div_y) {
    if (left) v->split_drag = true;
    return true;
  }
  if (y > L.div_y) return false;
  TextBuf tb;
  tb_load(&tb, v);
  if (ev->mouse.button == UI_MOUSE_SCROLL_UP || ev->mouse.button == UI_MOUSE_SCROLL_DOWN) {
    for (int i = 0; i < 3; i++) textbuf_move_line(&tb, ev->mouse.button == UI_MOUSE_SCROLL_UP ? -1 : 1);
    v->text_sel_anchor = SIZE_MAX;
  } else if (left) {
    size_t nlines = textbuf_num_lines(&tb);
    int gw = digits(nlines) + 1;
    size_t line = v->text_scroll_line + (size_t)(y - L.editor.y);
    if (line >= nlines) line = nlines - 1;
    int dx = x - L.editor.x - gw;
    size_t col = v->text_scroll_col + (size_t)(dx > 0 ? dx : 0);
    tb.cursor = textbuf_cursor_at(&tb, line, col);
    tb.want_col = col;
    v->text_sel_anchor = SIZE_MAX;
    v->focus_results = false;
  }
  tb_store(&tb, v);
  return true; /* releases and other buttons over the editor are ours too */
}

bool query_view_do_action(NcApp *app, View *v, HotkeyAction a) {
  if (!v || v->kind != VIEW_QUERY) return false;
  switch (a) {
  case HOTKEY_EXECUTE_QUERY: run_at_cursor(app, v); return true;
  case HOTKEY_EXECUTE_ALL: app_query_run_all(app, v, false); return true;
  case HOTKEY_EXECUTE_TRANSACTION: app_query_run_all(app, v, true); return true;
  case HOTKEY_SPLIT_UP: resize_split(app, v, -1); return true;
  case HOTKEY_SPLIT_DOWN: resize_split(app, v, 1); return true;
  default: return false;
  }
}

static const HotkeyAction QUERY_ACTIONS[] = {HOTKEY_EXECUTE_QUERY, HOTKEY_EXECUTE_ALL,
                                             HOTKEY_EXECUTE_TRANSACTION, HOTKEY_SPLIT_UP,
                                             HOTKEY_SPLIT_DOWN};

/* The results' keys: movement and the writing actions the grid offers a query view */
static const HotkeyAction RESULT_ACTIONS[] = {
    HOTKEY_MOVE_UP, HOTKEY_MOVE_DOWN, HOTKEY_MOVE_LEFT, HOTKEY_MOVE_RIGHT, HOTKEY_PAGE_UP,
    HOTKEY_PAGE_DOWN, HOTKEY_FIRST_ROW, HOTKEY_LAST_ROW, HOTKEY_GOTO_ROW, HOTKEY_FIRST_COL,
    HOTKEY_LAST_COL, HOTKEY_REFRESH, HOTKEY_TOGGLE_SELECTION, HOTKEY_CLEAR_SELECTIONS,
    HOTKEY_COLUMN_NARROWER, HOTKEY_COLUMN_WIDER, HOTKEY_EDIT_MODAL, HOTKEY_SET_NULL,
    HOTKEY_SET_EMPTY, HOTKEY_DELETE_ROW, HOTKEY_CELL_COPY, HOTKEY_CELL_PASTE};

static bool editor_key(NcApp *app, View *v, const UiEvent *ev, Rect ed) {
  if (nc_key_special(ev, UI_KEY_ESCAPE)) {
    v->focus_results = true;
    v->text_sel_anchor = SIZE_MAX;
    return true;
  }
  TextBuf tb;
  tb_load(&tb, v);
  bool shift = ev->key.mods & UI_MOD_SHIFT;
  bool handled = true, moved = false;
  char buf[8];
  const char *typed;
  if (ev->key.is_special && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
    switch (ev->key.key) {
    case UI_KEY_LEFT: textbuf_move_char(&tb, -1); moved = true; break;
    case UI_KEY_RIGHT: textbuf_move_char(&tb, 1); moved = true; break;
    case UI_KEY_UP: textbuf_move_line(&tb, -1); moved = true; break;
    case UI_KEY_DOWN: textbuf_move_line(&tb, 1); moved = true; break;
    case UI_KEY_PAGEUP:
      for (int i = 0; i < ed.rows - 1; i++) textbuf_move_line(&tb, -1);
      moved = true;
      break;
    case UI_KEY_PAGEDOWN:
      for (int i = 0; i < ed.rows - 1; i++) textbuf_move_line(&tb, 1);
      moved = true;
      break;
    case UI_KEY_HOME: textbuf_home(&tb); moved = true; break;
    case UI_KEY_END: textbuf_end(&tb); moved = true; break;
    case UI_KEY_BACKSPACE:
      if (has_selection(v)) delete_selection(&tb, v);
      else textbuf_delete(&tb, true);
      break;
    case UI_KEY_DELETE:
      if (has_selection(v)) delete_selection(&tb, v);
      else textbuf_delete(&tb, false);
      break;
    case UI_KEY_ENTER:
      delete_selection(&tb, v);
      textbuf_insert(&tb, "\n", 1);
      break;
    case UI_KEY_TAB:
      delete_selection(&tb, v);
      textbuf_insert(&tb, "\t", 1);
      break;
    default: handled = false; break;
    }
  } else if (hotkey_matches(app->config, ev, HOTKEY_CUT_LINE)) {
    size_t a, b;
    if (!selection(v, &a, &b)) {
      size_t l, c;
      textbuf_line_col(&tb, &l, &c);
      a = textbuf_line_start(&tb, l);
      b = textbuf_line_end(&tb, l);
      if (b < tb.len) b++; /* the newline goes too */
      else if (a > 0) a--; /* the last line: take the break before it */
    }
    if (b > a) {
      char *cut = str_ndup(tb.text + a, b - a);
      app_clipboard_set(app, cut);
      str_secure_free(cut);
      memmove(tb.text + a, tb.text + b, tb.len - b + 1);
      tb.len -= b - a;
      tb.cursor = a;
      v->text_sel_anchor = SIZE_MAX;
      tb.want_col = 0;
      size_t l, c;
      textbuf_line_col(&tb, &l, &c);
      tb.want_col = c;
    }
  } else if (hotkey_matches(app->config, ev, HOTKEY_PASTE)) {
    const char *clip = app_clipboard_get(app);
    delete_selection(&tb, v);
    if (clip && *clip) textbuf_insert(&tb, clip, strlen(clip));
  } else if ((typed = nc_key_typed(ev, buf))) {
    delete_selection(&tb, v);
    textbuf_insert(&tb, typed, strlen(typed));
  } else {
    handled = false;
  }
  if (!handled) return false;
  if (moved) {
    if (shift) {
      if (v->text_sel_anchor == SIZE_MAX) v->text_sel_anchor = v->text_cursor > tb.len ? tb.len : v->text_cursor;
    } else {
      v->text_sel_anchor = SIZE_MAX;
    }
  }
  tb_store(&tb, v);
  if (v->text_sel_anchor != SIZE_MAX && v->text_sel_anchor > tb.len) v->text_sel_anchor = tb.len;
  return true;
}

bool query_view_handle_key(NcApp *app, View *v, const UiEvent *ev, Rect r) {
  if (ev->type != UI_EVENT_KEY) return false;
  for (size_t i = 0; i < sizeof QUERY_ACTIONS / sizeof *QUERY_ACTIONS; i++)
    if (hotkey_matches(app->config, ev, QUERY_ACTIONS[i]) && query_view_do_action(app, v, QUERY_ACTIONS[i]))
      return true;
  Layout L;
  layout(v, r, &L);
  if (!v->focus_results) return editor_key(app, v, ev, L.editor);
  if (nc_key_special(ev, UI_KEY_ENTER)) {
    v->focus_results = false;
    return true;
  }
  for (size_t i = 0; i < sizeof RESULT_ACTIONS / sizeof *RESULT_ACTIONS; i++)
    if (hotkey_matches(app->config, ev, RESULT_ACTIONS[i]) && grid_do_action(app, v, RESULT_ACTIONS[i]))
      return true;
  return false;
}
