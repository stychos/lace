/*
 * Lace
 * Single-line text input
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "text_input.h"
#include "../core/utf8.h"
#include "../render_helpers.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/mem.h"
#include "../../../liblace/include/util/str.h"
#include <stdlib.h>
#include <string.h>

void text_input_init(TextInput *in, const char *initial, bool secret) {
  memset(in, 0, sizeof(*in));
  in->owned = true;
  in->secret = secret;
  in->cap = 64;
  in->buf = safe_malloc(in->cap);
  in->buf[0] = '\0';
  text_input_set(in, initial);
}

void text_input_bind(TextInput *in, char *buf, size_t cap, bool secret) {
  memset(in, 0, sizeof(*in));
  in->buf = buf;
  in->cap = cap;
  in->secret = secret;
  in->len = strlen(buf);
  in->cursor = in->len;
}

void text_input_adopt(TextInput *in, char *heap_text, size_t cursor) {
  memset(in, 0, sizeof(*in));
  in->owned = true;
  in->buf = heap_text;
  in->len = strlen(heap_text);
  in->cap = in->len + 1;
  in->cursor = cursor > in->len ? in->len : cursor;
  while (in->cursor > 0 && ((unsigned char)in->buf[in->cursor] & 0xC0) == 0x80)
    in->cursor--; /* Onto a character boundary */
}

char *text_input_release(TextInput *in, size_t *cursor) {
  char *text = in->buf;
  if (cursor)
    *cursor = in->cursor;
  memset(in, 0, sizeof(*in));
  return text;
}

void text_input_free(TextInput *in) {
  if (in->owned && in->buf) {
    str_secure_free(in->buf); /* Text may hold a password or connstr */
  }
  memset(in, 0, sizeof(*in));
}

/* Make room for len + extra bytes plus the NUL. A bound buffer cannot grow:
 * returns how many of the extra bytes fit. */
static size_t reserve(TextInput *in, size_t extra) {
  if (in->len + extra + 1 <= in->cap)
    return extra;
  if (!in->owned)
    return in->cap > in->len + 1 ? in->cap - in->len - 1 : 0;
  size_t cap = in->cap;
  while (in->len + extra + 1 > cap)
    cap *= 2;
  char *grown = safe_malloc(cap);
  memcpy(grown, in->buf, in->len + 1);
  str_secure_free(in->buf); /* No stray copy of a password or connstr */
  in->buf = grown;
  in->cap = cap;
  return extra;
}

void text_input_set(TextInput *in, const char *text) {
  size_t n = text ? strlen(text) : 0;
  in->len = 0;
  in->buf[0] = '\0';
  size_t fit = reserve(in, n);
  if (fit < n)
    fit = utf8_truncate(text, fit); /* Never cut a character in half */
  if (fit > 0)
    memcpy(in->buf, text, fit);
  in->len = fit;
  in->buf[fit] = '\0';
  in->cursor = fit;
}

/* Insert one byte of typed text. ncurses delivers a multi-byte character a
 * byte at a time; in a fixed buffer, the lead byte only goes in when the
 * whole character fits, so the text is never left with half a character. */
static void insert_byte(TextInput *in, unsigned char c) {
  size_t need = utf8_seq_len(c);
  if ((c & 0xC0) == 0x80) {
    /* A continuation byte only completes a character begun just before the
     * cursor; if that lead byte was refused (no room), drop the rest too */
    size_t start = in->cursor;
    while (start > 0 && in->cursor - start < 3 &&
           ((unsigned char)in->buf[start - 1] & 0xC0) == 0x80)
      start--;
    if (start == 0 ||
        utf8_seq_len((unsigned char)in->buf[start - 1]) <= in->cursor - start + 1)
      return;
    need = 1;
  }
  if (reserve(in, need) < need)
    return;
  memmove(in->buf + in->cursor + 1, in->buf + in->cursor,
          in->len - in->cursor + 1);
  in->buf[in->cursor] = (char)c;
  in->cursor++;
  in->len++;
}

void text_input_insert(TextInput *in, const char *text) {
  if (!text)
    return;
  size_t n = strlen(text);
  size_t fit = reserve(in, n);
  if (fit < n)
    fit = utf8_truncate(text, fit);
  memmove(in->buf + in->cursor + fit, in->buf + in->cursor,
          in->len - in->cursor + 1);
  for (size_t i = 0; i < fit; i++) {
    char c = text[i];
    in->buf[in->cursor + i] = (c == '\n' || c == '\r') ? ' ' : c;
  }
  in->cursor += fit;
  in->len += fit;
}

/* Remove bytes [from, to) */
static void delete_range(TextInput *in, size_t from, size_t to) {
  memmove(in->buf + from, in->buf + to, in->len - to + 1);
  in->len -= to - from;
  if (in->cursor > to)
    in->cursor -= to - from;
  else if (in->cursor > from)
    in->cursor = from;
}

bool text_input_handle_key(TextInput *in, const UiEvent *event) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  if (render_event_is_special(event, UI_KEY_LEFT)) {
    in->cursor = utf8_prev(in->buf, in->cursor);
    return true;
  }
  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    in->cursor = utf8_next(in->buf, in->len, in->cursor);
    return true;
  }
  if (render_event_is_special(event, UI_KEY_HOME) ||
      render_event_is_ctrl(event, 'A')) {
    in->cursor = 0;
    return true;
  }
  if (render_event_is_special(event, UI_KEY_END) ||
      render_event_is_ctrl(event, 'E')) {
    in->cursor = in->len;
    return true;
  }
  if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
    if (in->cursor > 0)
      delete_range(in, utf8_prev(in->buf, in->cursor), in->cursor);
    return true;
  }
  if (render_event_is_special(event, UI_KEY_DELETE) ||
      render_event_is_ctrl(event, 'D')) {
    if (in->cursor < in->len)
      delete_range(in, in->cursor, utf8_next(in->buf, in->len, in->cursor));
    return true;
  }
  if (render_event_is_ctrl(event, 'U')) {
    delete_range(in, 0, in->cursor);
    return true;
  }
  if (render_event_is_ctrl(event, 'K')) {
    delete_range(in, in->cursor, in->len);
    return true;
  }

  int key_char = render_event_get_char(event);
  if (render_event_is_char(event) && utf8_is_text_key(key_char)) {
    insert_byte(in, (unsigned char)key_char);
    return true;
  }
  return false;
}

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
