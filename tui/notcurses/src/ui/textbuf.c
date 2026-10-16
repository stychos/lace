#include "ui/textbuf.h"
#include "core/utf8.h"
#include "util/mem.h"
#include "util/str.h"
#include <string.h>

#define TAB_COLS 4

static void reserve(TextBuf *tb, size_t need) {
  if (need + 1 <= tb->cap) return;
  size_t cap = tb->cap ? tb->cap : 64;
  while (cap < need + 1) cap *= 2;
  tb->text = safe_realloc(tb->text, cap);
  tb->cap = cap;
}

static int char_cols(const char *p, size_t n) { return *p == '\t' ? TAB_COLS : utf8_width(p, n); }

static void remember_col(TextBuf *tb) {
  size_t l, c;
  textbuf_line_col(tb, &l, &c);
  tb->want_col = c;
}

void textbuf_init(TextBuf *tb, const char *initial) {
  memset(tb, 0, sizeof *tb);
  textbuf_set(tb, initial);
}

void textbuf_free(TextBuf *tb) {
  if (tb->text) {
    str_secure_free(tb->text);
  }
  memset(tb, 0, sizeof *tb);
}

void textbuf_set(TextBuf *tb, const char *text) {
  size_t n = text ? strlen(text) : 0;
  reserve(tb, n);
  if (n) memcpy(tb->text, text, n);
  tb->text[n] = 0;
  tb->len = tb->cursor = n;
  remember_col(tb);
}

void textbuf_adopt(TextBuf *tb, char *text, size_t cursor, size_t want_col) {
  tb->text = text;
  tb->len = strlen(text);
  tb->cap = tb->len + 1;
  tb->cursor = cursor > tb->len ? tb->len : cursor;
  tb->want_col = want_col;
}

void textbuf_insert(TextBuf *tb, const char *s, size_t n) {
  if (!n) return;
  reserve(tb, tb->len + n);
  memmove(tb->text + tb->cursor + n, tb->text + tb->cursor, tb->len - tb->cursor + 1);
  memcpy(tb->text + tb->cursor, s, n);
  tb->len += n;
  tb->cursor += n;
  remember_col(tb);
}

void textbuf_delete(TextBuf *tb, bool backward) {
  size_t a, b;
  if (backward) {
    if (!tb->cursor) return;
    a = utf8_prev(tb->text, tb->cursor);
    b = tb->cursor;
  } else {
    if (tb->cursor >= tb->len) return;
    a = tb->cursor;
    b = utf8_next(tb->text, tb->len, tb->cursor);
    if (b <= a) b = a + 1;
  }
  memmove(tb->text + a, tb->text + b, tb->len - b + 1);
  tb->len -= b - a;
  tb->cursor = a;
  remember_col(tb);
}

void textbuf_move_char(TextBuf *tb, int dir) {
  if (dir < 0) {
    if (tb->cursor) tb->cursor = utf8_prev(tb->text, tb->cursor);
  } else if (tb->cursor < tb->len) {
    size_t nx = utf8_next(tb->text, tb->len, tb->cursor);
    tb->cursor = nx > tb->cursor ? nx : tb->cursor + 1;
  }
  remember_col(tb);
}

size_t textbuf_num_lines(const TextBuf *tb) {
  size_t n = 1;
  for (size_t i = 0; i < tb->len; i++)
    if (tb->text[i] == '\n') n++;
  return n;
}

size_t textbuf_line_start(const TextBuf *tb, size_t line) {
  size_t pos = 0;
  while (line > 0) {
    const char *nl = memchr(tb->text + pos, '\n', tb->len - pos);
    if (!nl) break;
    pos = (size_t)(nl - tb->text) + 1;
    line--;
  }
  return pos;
}

size_t textbuf_line_end(const TextBuf *tb, size_t line) {
  size_t s = textbuf_line_start(tb, line);
  const char *nl = memchr(tb->text + s, '\n', tb->len - s);
  return nl ? (size_t)(nl - tb->text) : tb->len;
}

void textbuf_line_col(const TextBuf *tb, size_t *line, size_t *col) {
  size_t l = 0, start = 0;
  for (size_t i = 0; i < tb->cursor && i < tb->len; i++)
    if (tb->text[i] == '\n') { l++; start = i + 1; }
  size_t c = 0;
  for (size_t i = start; i < tb->cursor && i < tb->len;) {
    size_t nx = utf8_next(tb->text, tb->len, i);
    if (nx <= i) nx = i + 1;
    c += (size_t)char_cols(tb->text + i, nx - i);
    i = nx;
  }
  if (line) *line = l;
  if (col) *col = c;
}

size_t textbuf_cursor_at(const TextBuf *tb, size_t line, size_t col) {
  size_t s = textbuf_line_start(tb, line), e = textbuf_line_end(tb, line);
  size_t c = 0, i = s;
  while (i < e) {
    size_t nx = utf8_next(tb->text, e, i);
    if (nx <= i) nx = i + 1;
    size_t w = (size_t)char_cols(tb->text + i, nx - i);
    if (c + w > col) break;
    c += w;
    i = nx;
  }
  return i;
}

void textbuf_home(TextBuf *tb) {
  size_t l;
  textbuf_line_col(tb, &l, NULL);
  tb->cursor = textbuf_line_start(tb, l);
  tb->want_col = 0;
}

void textbuf_end(TextBuf *tb) {
  size_t l;
  textbuf_line_col(tb, &l, NULL);
  tb->cursor = textbuf_line_end(tb, l);
  remember_col(tb);
}

void textbuf_move_line(TextBuf *tb, int dir) {
  size_t l, c;
  textbuf_line_col(tb, &l, &c);
  size_t want = tb->want_col;
  size_t last = textbuf_num_lines(tb) - 1;
  if (dir < 0) {
    if (l == 0) { tb->cursor = 0; return; }
    l--;
  } else {
    if (l >= last) { tb->cursor = tb->len; return; }
    l++;
  }
  tb->cursor = textbuf_cursor_at(tb, l, want);
  tb->want_col = want;
}

size_t textbuf_statement_at(const TextBuf *tb, LaceDriver d, size_t *start, size_t *end) {
  size_t a = 0, b = 0;
  if (!tb->text || !lace_sql_statement_at(tb->text, tb->cursor, d, &a, &b) || b <= a) return 0;
  if (start) *start = a;
  if (end) *end = b;
  return b - a;
}
