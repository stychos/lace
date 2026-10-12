/*
 * Lace
 * Width-aware text drawing and string helpers
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/session.h"
#include "core/actions.h"
#include "../../liblace/include/util/mem.h"
#include "tui_internal.h"
#include "views/config_view.h"
#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


/*
 * Sanitize string for single-line cell display.
 * Replaces newlines, tabs, and control characters with safe alternatives.
 * Returns a newly allocated string that must be freed.
 */
char *tui_sanitize_for_display(const char *str) {
  if (!str)
    return NULL;

  size_t len = strlen(str);
  char *result = safe_malloc(len + 1);

  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    if (c == '\n' || c == '\r') {
      result[i] = ' '; /* Replace newlines with space */
    } else if (c == '\t') {
      result[i] = ' '; /* Replace tabs with space */
    } else if (c < 32 && c != 0) {
      result[i] = '?'; /* Replace other control chars */
    } else {
      result[i] = str[i];
    }
  }
  result[len] = '\0';
  return result;
}

/* Print str at (y, x) clipped to width display columns, never splitting a
 * character. tui_print_fit also pads with spaces to exactly width columns. */
void tui_print_clip(WINDOW *win, int y, int x, int width, const char *str) {
  if (!win || width <= 0)
    return;
  wmove(win, y, x);
  if (!str)
    return;
  int cols = 0;
  size_t n = utf8_fit(str, strlen(str), width, &cols);
  if (n > 0)
    waddnstr(win, str, (int)n);
}

void tui_print_fit(WINDOW *win, int y, int x, int width, const char *str) {
  if (!win || width <= 0)
    return;
  int cols = 0;
  size_t n = str ? utf8_fit(str, strlen(str), width, &cols) : 0;
  wmove(win, y, x);
  if (n > 0)
    waddnstr(win, str, (int)n);
  for (; cols < width; cols++)
    waddch(win, ' ');
}

int tui_draw_edit_field(WINDOW *win, int y, int x, int width, const char *buf,
                        size_t pos, attr_t attr) {
  if (!win || width <= 0)
    return -1;
  if (!buf)
    buf = "";
  size_t len = strlen(buf);
  if (pos > len)
    pos = len;

  /* Scroll so the text before the cursor leaves one column for it */
  size_t start = 0;
  int before = utf8_width(buf, pos);
  if (before > width - 1)
    start = utf8_offset_at_col(buf, pos, before - (width - 1));
  while (start < pos && utf8_width(buf + start, pos - start) > width - 1)
    start = utf8_next(buf, len, start);

  wattron(win, attr);
  mvwhline(win, y, x, ' ', width);
  size_t n = utf8_fit(buf + start, len - start, width, NULL);
  if (n > 0)
    mvwaddnstr(win, y, x, buf + start, (int)n);
  wattroff(win, attr);

  /* Cursor: the whole character under it, in reverse video */
  int cursor_x = x + utf8_width(buf + start, pos - start);
  if (cursor_x < x || cursor_x >= x + width)
    return -1;
  wattron(win, A_REVERSE | A_BOLD);
  if (pos < len && (unsigned char)buf[pos] >= 32)
    mvwaddnstr(win, y, cursor_x, buf + pos, (int)(utf8_next(buf, len, pos) - pos));
  else
    mvwaddch(win, y, cursor_x, ' ');
  wattroff(win, A_REVERSE | A_BOLD);
  return cursor_x;
}

/* ASCII-only lowercase: tolower() in a UTF-8 locale would fold single
 * bytes of multi-byte characters as if they were Latin-1 */
static int ascii_lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

/* Case-insensitive (ASCII) substring search */
const char *tui_str_istr(const char *haystack, const char *needle) {
  if (!haystack || !needle)
    return NULL;
  if (!*needle)
    return haystack;

  for (; *haystack; haystack++) {
    const char *h = haystack;
    const char *n = needle;
    while (*h && *n &&
           ascii_lower((unsigned char)*h) == ascii_lower((unsigned char)*n)) {
      h++;
      n++;
    }
    if (!*n)
      return haystack;
  }
  return NULL;
}
