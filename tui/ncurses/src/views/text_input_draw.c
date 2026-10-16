/*
 * Lace
 * Single-line text input: ncurses drawing
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "text_input_draw.h"
#include "../core/utf8.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/mem.h"
#include <stdlib.h>
#include <string.h>

int text_input_draw(WINDOW *win, const TextInput *in, int y, int x, int width,
                    attr_t attr, bool focused) {
  const char *text = in->buf;
  size_t cursor = in->cursor;
  char *mask = NULL;
  if (in->secret) {
    /* One '*' per character: the length shows, the text does not */
    size_t chars = utf8_count(in->buf, in->len);
    mask = safe_malloc(chars + 1);
    memset(mask, '*', chars);
    mask[chars] = '\0';
    text = mask;
    cursor = utf8_count(in->buf, in->cursor);
  }
  int cursor_x = -1;
  if (focused) {
    cursor_x = tui_draw_edit_field(win, y, x, width, text, cursor, attr);
  } else {
    wattron(win, attr);
    tui_print_fit(win, y, x, width, text);
    wattroff(win, attr);
  }
  free(mask);
  return cursor_x;
}
