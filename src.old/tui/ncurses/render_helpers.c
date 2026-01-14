/*
 * Lace
 * TUI Render Helpers Implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "render_helpers.h"
#include <ncurses.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Current attributes for tracking state */
static UiColor s_current_color = UI_COLOR_DEFAULT;
static UiAttr s_current_attrs = UI_ATTR_NORMAL;

/* ============================================================================
 * Initialization
 * ============================================================================
 */

RenderContext *render_init(void) {
  const RenderBackend *backend = render_backend_current();
  if (!backend || !backend->init) {
    return NULL;
  }
  return backend->init();
}

void render_shutdown(RenderContext *ctx) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->shutdown) {
    backend->shutdown(ctx);
  }
}

/* ============================================================================
 * Frame Management
 * ============================================================================
 */

void render_begin_frame(RenderContext *ctx) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->begin_frame) {
    backend->begin_frame(ctx);
  }
  s_current_color = UI_COLOR_DEFAULT;
  s_current_attrs = UI_ATTR_NORMAL;
}

void render_end_frame(RenderContext *ctx) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->end_frame) {
    backend->end_frame(ctx);
  }
}

void render_get_size(RenderContext *ctx, int *width, int *height) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->get_size) {
    backend->get_size(ctx, width, height);
  } else {
    if (width)
      *width = 80;
    if (height)
      *height = 24;
  }
}

/* ============================================================================
 * Color and Attribute Helpers
 * ============================================================================
 */

void render_set_color(RenderContext *ctx, UiColor color) {
  render_set_color_attrs(ctx, color, UI_ATTR_NORMAL);
}

void render_set_color_attrs(RenderContext *ctx, UiColor color, UiAttr attrs) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->set_color) {
    backend->set_color(ctx, color, attrs);
  }
  s_current_color = color;
  s_current_attrs = attrs;
}

void render_set_bold(RenderContext *ctx, bool bold) {
  UiAttr attrs = s_current_attrs;
  if (bold) {
    attrs |= UI_ATTR_BOLD;
  } else {
    attrs &= ~UI_ATTR_BOLD;
  }
  render_set_color_attrs(ctx, s_current_color, attrs);
}

void render_set_reverse(RenderContext *ctx, bool reverse) {
  UiAttr attrs = s_current_attrs;
  if (reverse) {
    attrs |= UI_ATTR_REVERSE;
  } else {
    attrs &= ~UI_ATTR_REVERSE;
  }
  render_set_color_attrs(ctx, s_current_color, attrs);
}

void render_reset(RenderContext *ctx) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->reset_attrs) {
    backend->reset_attrs(ctx);
  }
  s_current_color = UI_COLOR_DEFAULT;
  s_current_attrs = UI_ATTR_NORMAL;
}

/* ============================================================================
 * Text Drawing
 * ============================================================================
 */

void render_char(RenderContext *ctx, int x, int y, int ch) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->draw_char) {
    backend->draw_char(ctx, x, y, ch);
  }
}

void render_string(RenderContext *ctx, int x, int y, const char *str) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->draw_string && str) {
    backend->draw_string(ctx, x, y, str);
  }
}

void render_printf(RenderContext *ctx, int x, int y, const char *fmt, ...) {
  if (!fmt)
    return;

  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  render_string(ctx, x, y, buf);
}

void render_string_fixed(RenderContext *ctx, int x, int y, const char *str,
                         int width) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->draw_string_n) {
    backend->draw_string_n(ctx, x, y, str ? str : "", width);
  }
}

void render_string_left(RenderContext *ctx, int x, int y, const char *str,
                        int width) {
  render_string_fixed(ctx, x, y, str, width);
}

void render_string_right(RenderContext *ctx, int x, int y, const char *str,
                         int width) {
  if (!str) {
    render_string_fixed(ctx, x, y, "", width);
    return;
  }

  /* Calculate display width (for UTF-8 strings) */
  int display_width = 0;
  const char *p = str;
  while (*p) {
    wchar_t wc;
    int mb_len = mbtowc(&wc, p, MB_CUR_MAX);
    if (mb_len <= 0) {
      p++;
      display_width++;
      continue;
    }
    int char_width = wcwidth(wc);
    if (char_width < 0)
      char_width = 1;
    display_width += char_width;
    p += mb_len;
  }

  if (display_width >= width) {
    /* String is longer than width, truncate from left */
    render_string_fixed(ctx, x, y, str, width);
  } else {
    /* Pad with spaces on left */
    int pad = width - display_width;
    const RenderBackend *backend = render_backend_current();
    if (backend && backend->draw_hline) {
      backend->draw_hline(ctx, x, y, pad, ' ');
    }
    render_string(ctx, x + pad, y, str);
  }
}

void render_string_center(RenderContext *ctx, int x, int y, const char *str,
                          int width) {
  if (!str) {
    render_string_fixed(ctx, x, y, "", width);
    return;
  }

  size_t len = strlen(str);
  if ((int)len >= width) {
    render_string_fixed(ctx, x, y, str, width);
  } else {
    int pad_left = (width - (int)len) / 2;
    int pad_right = width - (int)len - pad_left;

    const RenderBackend *backend = render_backend_current();
    if (backend && backend->draw_hline) {
      backend->draw_hline(ctx, x, y, pad_left, ' ');
      backend->draw_hline(ctx, x + pad_left + (int)len, y, pad_right, ' ');
    }
    render_string(ctx, x + pad_left, y, str);
  }
}

/* ============================================================================
 * Line Drawing
 * ============================================================================
 */

void render_hline(RenderContext *ctx, int x, int y, int width) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->draw_hline) {
    backend->draw_hline(ctx, x, y, width, ACS_HLINE);
  }
}

void render_vline(RenderContext *ctx, int x, int y, int height) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->draw_vline) {
    backend->draw_vline(ctx, x, y, height, ACS_VLINE);
  }
}

void render_box(RenderContext *ctx, int x, int y, int width, int height) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->draw_box) {
    backend->draw_box(ctx, x, y, width, height);
  }
}

void render_fill(RenderContext *ctx, int x, int y, int width, int height) {
  render_fill_char(ctx, x, y, width, height, ' ');
}

void render_fill_char(RenderContext *ctx, int x, int y, int width, int height,
                      int ch) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->fill_rect) {
    backend->fill_rect(ctx, x, y, width, height, ch);
  }
}

/* ============================================================================
 * Region Helpers
 * ============================================================================
 */

RenderRegion render_region(int x, int y, int width, int height) {
  RenderRegion region = {x, y, width, height};
  return region;
}

void render_region_char(RenderContext *ctx, RenderRegion *region, int x, int y,
                        int ch) {
  if (!region)
    return;

  /* Convert to absolute coordinates */
  int abs_x = region->x + x;
  int abs_y = region->y + y;

  /* Clip to region bounds */
  if (x < 0 || x >= region->width || y < 0 || y >= region->height) {
    return;
  }

  render_char(ctx, abs_x, abs_y, ch);
}

void render_region_string(RenderContext *ctx, RenderRegion *region, int x,
                          int y, const char *str) {
  if (!region || !str)
    return;

  /* Clip y to region bounds */
  if (y < 0 || y >= region->height) {
    return;
  }

  /* Convert to absolute coordinates */
  int abs_x = region->x + x;
  int abs_y = region->y + y;

  /* Calculate max width we can draw */
  int max_width = region->width - x;
  if (max_width <= 0 || x < 0) {
    return;
  }

  const RenderBackend *backend = render_backend_current();
  if (backend && backend->draw_string_n) {
    backend->draw_string_n(ctx, abs_x, abs_y, str, max_width);
  }
}

void render_region_printf(RenderContext *ctx, RenderRegion *region, int x,
                          int y, const char *fmt, ...) {
  if (!fmt)
    return;

  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  render_region_string(ctx, region, x, y, buf);
}

void render_region_hline(RenderContext *ctx, RenderRegion *region, int x, int y,
                         int width) {
  if (!region)
    return;

  /* Clip to region bounds */
  if (y < 0 || y >= region->height) {
    return;
  }

  int abs_x = region->x + x;
  int abs_y = region->y + y;

  /* Clip width */
  if (x < 0) {
    width += x;
    abs_x = region->x;
  }
  if (x + width > region->width) {
    width = region->width - x;
  }
  if (width <= 0)
    return;

  const RenderBackend *backend = render_backend_current();
  if (backend && backend->draw_hline) {
    backend->draw_hline(ctx, abs_x, abs_y, width, ACS_HLINE);
  }
}

void render_region_fill(RenderContext *ctx, RenderRegion *region, int ch) {
  if (!region)
    return;

  const RenderBackend *backend = render_backend_current();
  if (backend && backend->fill_rect) {
    backend->fill_rect(ctx, region->x, region->y, region->width, region->height,
                       ch);
  }
}

void render_region_clear(RenderContext *ctx, RenderRegion *region) {
  render_region_fill(ctx, region, ' ');
}

void render_region_background(RenderContext *ctx, RenderRegion *region,
                              UiColor color) {
  if (!region)
    return;

  render_set_color(ctx, color);
  render_region_fill(ctx, region, ' ');
}

/* ============================================================================
 * Box Drawing Characters
 * ============================================================================
 */

int render_acs_hline(RenderContext *ctx) {
  (void)ctx;
  return ACS_HLINE;
}

int render_acs_vline(RenderContext *ctx) {
  (void)ctx;
  return ACS_VLINE;
}

int render_acs_ulcorner(RenderContext *ctx) {
  (void)ctx;
  return ACS_ULCORNER;
}

int render_acs_urcorner(RenderContext *ctx) {
  (void)ctx;
  return ACS_URCORNER;
}

int render_acs_llcorner(RenderContext *ctx) {
  (void)ctx;
  return ACS_LLCORNER;
}

int render_acs_lrcorner(RenderContext *ctx) {
  (void)ctx;
  return ACS_LRCORNER;
}

/* ============================================================================
 * Cursor Control
 * ============================================================================
 */

void render_cursor_visible(RenderContext *ctx, bool visible) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->set_cursor_visible) {
    backend->set_cursor_visible(ctx, visible);
  }
}

void render_cursor_move(RenderContext *ctx, int x, int y) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->set_cursor_pos) {
    backend->set_cursor_pos(ctx, x, y);
  }
}

/* ============================================================================
 * Input Handling
 * ============================================================================
 */

bool render_wait_event(RenderContext *ctx, UiEvent *event) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->wait_event) {
    return backend->wait_event(ctx, event, -1);
  }
  return false;
}

bool render_poll_event(RenderContext *ctx, UiEvent *event) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->poll_event) {
    return backend->poll_event(ctx, event);
  }
  return false;
}

void render_mouse_enable(RenderContext *ctx, bool enable) {
  const RenderBackend *backend = render_backend_current();
  if (backend && backend->set_mouse_enabled) {
    backend->set_mouse_enabled(ctx, enable);
  }
}

/* ============================================================================
 * Input Translation Helpers
 * ============================================================================
 */

bool render_translate_key(int ncurses_key, UiEvent *event) {
  if (!event)
    return false;

  memset(event, 0, sizeof(UiEvent));

  /* Handle timeout/error */
  if (ncurses_key == ERR) {
    event->type = UI_EVENT_NONE;
    return false;
  }

  /* Handle resize */
  if (ncurses_key == KEY_RESIZE) {
    event->type = UI_EVENT_RESIZE;
    /* Size will be filled by caller if needed */
    return true;
  }

  /* Handle mouse */
  if (ncurses_key == KEY_MOUSE) {
    MEVENT mevent;
    if (getmouse(&mevent) == OK) {
      event->type = UI_EVENT_MOUSE;
      event->mouse.x = mevent.x;
      event->mouse.y = mevent.y;

      if (mevent.bstate & BUTTON1_CLICKED) {
        event->mouse.button = UI_MOUSE_LEFT;
        event->mouse.action = UI_MOUSE_CLICK;
      } else if (mevent.bstate & BUTTON1_DOUBLE_CLICKED) {
        event->mouse.button = UI_MOUSE_LEFT;
        event->mouse.action = UI_MOUSE_DOUBLE_CLICK;
      } else if (mevent.bstate & BUTTON4_PRESSED) {
        event->mouse.button = UI_MOUSE_SCROLL_UP;
        event->mouse.action = UI_MOUSE_PRESS;
      } else if (mevent.bstate & BUTTON5_PRESSED) {
        event->mouse.button = UI_MOUSE_SCROLL_DOWN;
        event->mouse.action = UI_MOUSE_PRESS;
      } else if (mevent.bstate & BUTTON3_CLICKED) {
        event->mouse.button = UI_MOUSE_RIGHT;
        event->mouse.action = UI_MOUSE_CLICK;
      }
      return true;
    }
    return false;
  }

  /* Key event */
  event->type = UI_EVENT_KEY;
  event->key.mods = UI_MOD_NONE;
  event->key.is_special = false;

  /* Handle special keys */
  switch (ncurses_key) {
  /* Navigation */
  case KEY_UP:
    event->key.key = UI_KEY_UP;
    event->key.is_special = true;
    break;
  case KEY_DOWN:
    event->key.key = UI_KEY_DOWN;
    event->key.is_special = true;
    break;
  case KEY_LEFT:
    event->key.key = UI_KEY_LEFT;
    event->key.is_special = true;
    break;
  case KEY_RIGHT:
    event->key.key = UI_KEY_RIGHT;
    event->key.is_special = true;
    break;
  case KEY_HOME:
    event->key.key = UI_KEY_HOME;
    event->key.is_special = true;
    break;
  case KEY_END:
    event->key.key = UI_KEY_END;
    event->key.is_special = true;
    break;
  case KEY_PPAGE:
    event->key.key = UI_KEY_PAGEUP;
    event->key.is_special = true;
    break;
  case KEY_NPAGE:
    event->key.key = UI_KEY_PAGEDOWN;
    event->key.is_special = true;
    break;

  /* Editing keys */
  case KEY_BACKSPACE:
  case 127: /* DEL character */
    event->key.key = UI_KEY_BACKSPACE;
    event->key.is_special = true;
    break;
  case KEY_DC:
    event->key.key = UI_KEY_DELETE;
    event->key.is_special = true;
    break;
  case KEY_IC:
    event->key.key = UI_KEY_INSERT;
    event->key.is_special = true;
    break;
  case KEY_ENTER:
  case '\n':
  case '\r':
    event->key.key = UI_KEY_ENTER;
    event->key.is_special = true;
    break;
  case '\t':
    event->key.key = UI_KEY_TAB;
    event->key.is_special = true;
    break;
  case 27: /* ESC */
    event->key.key = UI_KEY_ESCAPE;
    event->key.is_special = true;
    break;

  /* Function keys */
  case KEY_F(1):
    event->key.key = UI_KEY_F1;
    event->key.is_special = true;
    break;
  case KEY_F(2):
    event->key.key = UI_KEY_F2;
    event->key.is_special = true;
    break;
  case KEY_F(3):
    event->key.key = UI_KEY_F3;
    event->key.is_special = true;
    break;
  case KEY_F(4):
    event->key.key = UI_KEY_F4;
    event->key.is_special = true;
    break;
  case KEY_F(5):
    event->key.key = UI_KEY_F5;
    event->key.is_special = true;
    break;
  case KEY_F(6):
    event->key.key = UI_KEY_F6;
    event->key.is_special = true;
    break;
  case KEY_F(7):
    event->key.key = UI_KEY_F7;
    event->key.is_special = true;
    break;
  case KEY_F(8):
    event->key.key = UI_KEY_F8;
    event->key.is_special = true;
    break;
  case KEY_F(9):
    event->key.key = UI_KEY_F9;
    event->key.is_special = true;
    break;
  case KEY_F(10):
    event->key.key = UI_KEY_F10;
    event->key.is_special = true;
    break;
  case KEY_F(11):
    event->key.key = UI_KEY_F11;
    event->key.is_special = true;
    break;
  case KEY_F(12):
    event->key.key = UI_KEY_F12;
    event->key.is_special = true;
    break;

  default:
    /* Control characters (Ctrl+A through Ctrl+Z are 1-26) */
    if (ncurses_key >= 1 && ncurses_key <= 26) {
      event->key.key = 'A' + ncurses_key - 1;
      event->key.mods = UI_MOD_CTRL;
      event->key.is_special = false;
    } else {
      /* Regular character */
      event->key.key = ncurses_key;
      event->key.is_special = false;
    }
    break;
  }

  return true;
}

bool render_event_is_key(const UiEvent *event, int key, UiKeyMod mods) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  return event->key.key == key && event->key.mods == mods;
}

bool render_event_is_char(const UiEvent *event) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  /* Printable ASCII characters (32-126) */
  if (!event->key.is_special && event->key.mods == UI_MOD_NONE) {
    int ch = event->key.key;
    return ch >= 32 && ch <= 126;
  }
  return false;
}

int render_event_get_char(const UiEvent *event) {
  if (!event || event->type != UI_EVENT_KEY)
    return 0;

  if (!event->key.is_special && event->key.mods == UI_MOD_NONE) {
    return event->key.key;
  }
  return 0;
}

bool render_event_is_ctrl(const UiEvent *event, char letter) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  /* Ctrl keys are stored as the uppercase letter with UI_MOD_CTRL */
  char upper = (letter >= 'a' && letter <= 'z') ? letter - 32 : letter;
  return event->key.key == upper && (event->key.mods & UI_MOD_CTRL);
}

bool render_event_is_special(const UiEvent *event, UiKeyCode code) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  return event->key.is_special && event->key.key == (int)code;
}

int render_event_get_fkey(const UiEvent *event) {
  if (!event || event->type != UI_EVENT_KEY || !event->key.is_special)
    return 0;

  switch (event->key.key) {
  case UI_KEY_F1:
    return 1;
  case UI_KEY_F2:
    return 2;
  case UI_KEY_F3:
    return 3;
  case UI_KEY_F4:
    return 4;
  case UI_KEY_F5:
    return 5;
  case UI_KEY_F6:
    return 6;
  case UI_KEY_F7:
    return 7;
  case UI_KEY_F8:
    return 8;
  case UI_KEY_F9:
    return 9;
  case UI_KEY_F10:
    return 10;
  case UI_KEY_F11:
    return 11;
  case UI_KEY_F12:
    return 12;
  default:
    return 0;
  }
}
