/*
 * Lace
 * UTF-8 helpers for text editing and display
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#define _XOPEN_SOURCE 700
#include "utf8.h"
#include <string.h>
#include <wchar.h>

static bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

bool utf8_is_text_key(int ch) {
  return (ch >= 32 && ch < 127) || (ch >= 0x80 && ch <= 0xFF);
}

size_t utf8_seq_len(unsigned char c) {
  if (c >= 0xF0 && c <= 0xF4)
    return 4;
  if (c >= 0xE0)
    return c <= 0xEF ? 3 : 1;
  if (c >= 0xC2)
    return 2;
  return 1;
}

size_t utf8_truncate(const char *s, size_t n) {
  if (!s)
    return 0;
  size_t p = n;
  while (p > 0 && n - p < 3 && is_cont((unsigned char)s[p]))
    p--;
  /* p is the start of the character containing s[n]; keep it only if it
   * ends at or before n */
  if (p < n && p + utf8_seq_len((unsigned char)s[p]) > n)
    return p;
  return n;
}

size_t utf8_prev(const char *s, size_t pos) {
  if (!s || pos == 0)
    return 0;
  size_t p = pos - 1;
  /* A character is at most 4 bytes: lead byte plus 3 continuations */
  for (int i = 0; i < 3 && p > 0 && is_cont((unsigned char)s[p]); i++)
    p--;
  return p;
}

/* Length of the character at s (1 for an invalid byte), within n bytes */
static size_t char_len(const char *s, size_t n, wchar_t *wc) {
  mbstate_t st;
  memset(&st, 0, sizeof(st));
  size_t r = mbrtowc(wc, s, n, &st);
  if (r == (size_t)-1 || r == (size_t)-2 || r == 0) {
    *wc = (wchar_t)-1;
    return 1;
  }
  return r;
}

size_t utf8_next(const char *s, size_t len, size_t pos) {
  if (!s || pos >= len)
    return len;
  wchar_t wc;
  size_t r = char_len(s + pos, len - pos, &wc);
  if (wc == (wchar_t)-1) {
    /* Not decodable (or a C locale): still skip a whole UTF-8 sequence */
    size_t p = pos + 1;
    while (p < len && p - pos < 4 && is_cont((unsigned char)s[p]))
      p++;
    return p;
  }
  return pos + r;
}

size_t utf8_count(const char *s, size_t n) {
  size_t count = 0;
  for (size_t p = 0; s && p < n && s[p]; p = utf8_next(s, n, p))
    count++;
  return count;
}

static int char_cols(wchar_t wc) {
  if (wc == (wchar_t)-1)
    return 1;
  int w = wcwidth(wc);
  return w < 0 ? 1 : w;
}

int utf8_width(const char *s, size_t n) {
  int cols = 0;
  for (size_t p = 0; s && p < n && s[p];) {
    wchar_t wc;
    size_t r = char_len(s + p, n - p, &wc);
    if (wc == (wchar_t)-1)
      r = utf8_next(s, n, p) - p;
    cols += char_cols(wc);
    p += r;
  }
  return cols;
}

size_t utf8_fit(const char *s, size_t n, int max_cols, int *cols) {
  int used = 0;
  size_t p = 0;
  while (s && p < n && s[p]) {
    wchar_t wc;
    size_t r = char_len(s + p, n - p, &wc);
    if (wc == (wchar_t)-1)
      r = utf8_next(s, n, p) - p;
    int w = char_cols(wc);
    if (used + w > max_cols)
      break;
    used += w;
    p += r;
  }
  if (cols)
    *cols = used;
  return p;
}

size_t utf8_offset_at_col(const char *s, size_t n, int col) {
  if (col <= 0)
    return 0;
  return utf8_fit(s, n, col, NULL);
}
