/*
 * Lace
 * ncurses Render Backend Implementation
 *
 * Implements the RenderBackend interface using ncurses for TUI rendering.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "backend.h"

#include <locale.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ============================================================================
 * Render Context (ncurses-specific)
 * ============================================================================
 */

struct RenderContext {
  WINDOW *main_win;    /* Main window (stdscr) */
  int width, height;   /* Current dimensions */
  bool mouse_enabled;  /* Mouse support active */
  bool colors_enabled; /* Color support active */
  UiColor cur_color;   /* Current color */
  UiAttr cur_attrs;    /* Current attributes */
};

/* ============================================================================
 * Color Pair Mapping
 * ============================================================================
 */

/* Map UiColor to ncurses color pair index (1-based, 0 is default) */
static int color_pair_map[UI_COLOR_COUNT] = {0};

static void init_colors(RenderContext *ctx) {
  if (!has_colors()) {
    ctx->colors_enabled = false;
    return;
  }

  start_color();
  use_default_colors();
  ctx->colors_enabled = true;

  /* Initialize color pairs matching existing TUI colors */
  /* Format: init_pair(pair_number, foreground, background) */

  /* UI_COLOR_DEFAULT - default terminal colors */
  init_pair(1, -1, -1);
  color_pair_map[UI_COLOR_DEFAULT] = 1;

  /* UI_COLOR_HEADER - white on blue */
  init_pair(2, COLOR_WHITE, COLOR_BLUE);
  color_pair_map[UI_COLOR_HEADER] = 2;

  /* UI_COLOR_SELECTED - black on cyan */
  init_pair(3, COLOR_BLACK, COLOR_CYAN);
  color_pair_map[UI_COLOR_SELECTED] = 3;

  /* UI_COLOR_STATUS - white on blue */
  init_pair(4, COLOR_WHITE, COLOR_BLUE);
  color_pair_map[UI_COLOR_STATUS] = 4;

  /* UI_COLOR_ERROR - white on red */
  init_pair(5, COLOR_WHITE, COLOR_RED);
  color_pair_map[UI_COLOR_ERROR] = 5;

  /* UI_COLOR_BORDER - cyan on default */
  init_pair(6, COLOR_CYAN, -1);
  color_pair_map[UI_COLOR_BORDER] = 6;

  /* UI_COLOR_TITLE - yellow on default, bold */
  init_pair(7, COLOR_YELLOW, -1);
  color_pair_map[UI_COLOR_TITLE] = 7;

  /* UI_COLOR_NULL - bright black (gray) on default */
  init_pair(8, COLOR_BLACK, -1);
  color_pair_map[UI_COLOR_NULL] = 8;

  /* UI_COLOR_NUMBER - cyan on default */
  init_pair(9, COLOR_CYAN, -1);
  color_pair_map[UI_COLOR_NUMBER] = 9;

  /* UI_COLOR_EDIT - black on yellow */
  init_pair(10, COLOR_BLACK, COLOR_YELLOW);
  color_pair_map[UI_COLOR_EDIT] = 10;
}

/* ============================================================================
 * Attribute Conversion
 * ============================================================================
 */

static attr_t ui_attrs_to_ncurses(UiAttr attrs) {
  attr_t result = A_NORMAL;

  if (attrs & UI_ATTR_BOLD) {
    result |= A_BOLD;
  }
  if (attrs & UI_ATTR_UNDERLINE) {
    result |= A_UNDERLINE;
  }
  if (attrs & UI_ATTR_REVERSE) {
    result |= A_REVERSE;
  }
  if (attrs & UI_ATTR_DIM) {
    result |= A_DIM;
  }

  return result;
}

/* ============================================================================
 * Key Code Translation
 * ============================================================================
 */

/* Translate ncurses key code to UiKeyCode */
static void translate_key(int ch, UiEvent *event) {
  event->type = UI_EVENT_KEY;
  event->key.mods = UI_MOD_NONE;
  event->key.is_special = false;
  event->key.key = ch;

  /* Check for control keys (1-26 = Ctrl+A through Ctrl+Z) */
  if (ch >= 1 && ch <= 26) {
    event->key.mods = UI_MOD_CTRL;
    event->key.key = 'A' + ch - 1; /* Uppercase to match render_translate_key */
    return;
  }

  /* Check for special keys */
  switch (ch) {
  case KEY_UP:
    event->key.is_special = true;
    event->key.key = UI_KEY_UP;
    break;
  case KEY_DOWN:
    event->key.is_special = true;
    event->key.key = UI_KEY_DOWN;
    break;
  case KEY_LEFT:
    event->key.is_special = true;
    event->key.key = UI_KEY_LEFT;
    break;
  case KEY_RIGHT:
    event->key.is_special = true;
    event->key.key = UI_KEY_RIGHT;
    break;
  case KEY_HOME:
    event->key.is_special = true;
    event->key.key = UI_KEY_HOME;
    break;
  case KEY_END:
    event->key.is_special = true;
    event->key.key = UI_KEY_END;
    break;
  case KEY_PPAGE:
    event->key.is_special = true;
    event->key.key = UI_KEY_PAGEUP;
    break;
  case KEY_NPAGE:
    event->key.is_special = true;
    event->key.key = UI_KEY_PAGEDOWN;
    break;
  case KEY_BACKSPACE:
  case 127:
  case 8:
    event->key.is_special = true;
    event->key.key = UI_KEY_BACKSPACE;
    break;
  case KEY_DC:
    event->key.is_special = true;
    event->key.key = UI_KEY_DELETE;
    break;
  case KEY_IC:
    event->key.is_special = true;
    event->key.key = UI_KEY_INSERT;
    break;
  case KEY_ENTER:
  case '\n':
  case '\r':
    event->key.is_special = true;
    event->key.key = UI_KEY_ENTER;
    break;
  case '\t':
    event->key.is_special = true;
    event->key.key = UI_KEY_TAB;
    break;
  case 27: /* ESC */
    event->key.is_special = true;
    event->key.key = UI_KEY_ESCAPE;
    break;
  case KEY_F(1):
    event->key.is_special = true;
    event->key.key = UI_KEY_F1;
    break;
  case KEY_F(2):
    event->key.is_special = true;
    event->key.key = UI_KEY_F2;
    break;
  case KEY_F(3):
    event->key.is_special = true;
    event->key.key = UI_KEY_F3;
    break;
  case KEY_F(4):
    event->key.is_special = true;
    event->key.key = UI_KEY_F4;
    break;
  case KEY_F(5):
    event->key.is_special = true;
    event->key.key = UI_KEY_F5;
    break;
  case KEY_F(6):
    event->key.is_special = true;
    event->key.key = UI_KEY_F6;
    break;
  case KEY_F(7):
    event->key.is_special = true;
    event->key.key = UI_KEY_F7;
    break;
  case KEY_F(8):
    event->key.is_special = true;
    event->key.key = UI_KEY_F8;
    break;
  case KEY_F(9):
    event->key.is_special = true;
    event->key.key = UI_KEY_F9;
    break;
  case KEY_F(10):
    event->key.is_special = true;
    event->key.key = UI_KEY_F10;
    break;
  case KEY_F(11):
    event->key.is_special = true;
    event->key.key = UI_KEY_F11;
    break;
  case KEY_F(12):
    event->key.is_special = true;
    event->key.key = UI_KEY_F12;
    break;
  case KEY_RESIZE:
    event->type = UI_EVENT_RESIZE;
    getmaxyx(stdscr, event->resize.height, event->resize.width);
    break;
  default:
    /* Regular character */
    event->key.is_special = false;
    event->key.key = ch;
    break;
  }
}

/* Translate ncurses mouse event */
static bool translate_mouse(MEVENT *mevent, UiEvent *event) {
  event->type = UI_EVENT_MOUSE;
  event->mouse.x = mevent->x;
  event->mouse.y = mevent->y;
  event->mouse.mods = UI_MOD_NONE;

  /* Determine button */
  if (mevent->bstate & (BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON1_CLICKED |
                        BUTTON1_DOUBLE_CLICKED)) {
    event->mouse.button = UI_MOUSE_LEFT;
  } else if (mevent->bstate & (BUTTON2_PRESSED | BUTTON2_RELEASED |
                               BUTTON2_CLICKED | BUTTON2_DOUBLE_CLICKED)) {
    event->mouse.button = UI_MOUSE_MIDDLE;
  } else if (mevent->bstate & (BUTTON3_PRESSED | BUTTON3_RELEASED |
                               BUTTON3_CLICKED | BUTTON3_DOUBLE_CLICKED)) {
    event->mouse.button = UI_MOUSE_RIGHT;
#ifdef BUTTON4_PRESSED
  } else if (mevent->bstate & BUTTON4_PRESSED) {
    event->mouse.button = UI_MOUSE_SCROLL_UP;
    event->mouse.action = UI_MOUSE_PRESS;
    return true;
#endif
#ifdef BUTTON5_PRESSED
  } else if (mevent->bstate & BUTTON5_PRESSED) {
    event->mouse.button = UI_MOUSE_SCROLL_DOWN;
    event->mouse.action = UI_MOUSE_PRESS;
    return true;
#endif
  } else {
    event->mouse.button = UI_MOUSE_NONE;
  }

  /* Determine action */
  if (mevent->bstate & (BUTTON1_PRESSED | BUTTON2_PRESSED | BUTTON3_PRESSED)) {
    event->mouse.action = UI_MOUSE_PRESS;
  } else if (mevent->bstate &
             (BUTTON1_RELEASED | BUTTON2_RELEASED | BUTTON3_RELEASED)) {
    event->mouse.action = UI_MOUSE_RELEASE;
  } else if (mevent->bstate &
             (BUTTON1_CLICKED | BUTTON2_CLICKED | BUTTON3_CLICKED)) {
    event->mouse.action = UI_MOUSE_CLICK;
  } else if (mevent->bstate & (BUTTON1_DOUBLE_CLICKED | BUTTON2_DOUBLE_CLICKED |
                               BUTTON3_DOUBLE_CLICKED)) {
    event->mouse.action = UI_MOUSE_DOUBLE_CLICK;
  } else {
    event->mouse.action = UI_MOUSE_PRESS;
  }

  return true;
}

/* ============================================================================
 * Backend Implementation - Lifecycle
 * ============================================================================
 */

static RenderContext *ncurses_init(void) {
  RenderContext *ctx = calloc(1, sizeof(RenderContext));
  if (!ctx) {
    return NULL;
  }

  /* Set locale for UTF-8 support */
  setlocale(LC_ALL, "");

  /* Initialize ncurses */
  ctx->main_win = initscr();
  if (!ctx->main_win) {
    free(ctx);
    return NULL;
  }

  /* Configure ncurses */
  cbreak();               /* Disable line buffering */
  noecho();               /* Don't echo input */
  keypad(stdscr, TRUE);   /* Enable special keys */
  nodelay(stdscr, FALSE); /* Blocking input by default */
  curs_set(1);            /* Show cursor */

  /* Initialize colors */
  init_colors(ctx);

  /* Get initial size */
  getmaxyx(stdscr, ctx->height, ctx->width);

  ctx->cur_color = UI_COLOR_DEFAULT;
  ctx->cur_attrs = UI_ATTR_NORMAL;

  return ctx;
}

static void ncurses_shutdown(RenderContext *ctx) {
  if (!ctx) {
    return;
  }

  /* Disable mouse if enabled */
  if (ctx->mouse_enabled) {
    mousemask(0, NULL);
  }

  /* End ncurses mode */
  endwin();

  free(ctx);
}

/* ============================================================================
 * Backend Implementation - Display Management
 * ============================================================================
 */

static void ncurses_get_size(RenderContext *ctx, int *width, int *height) {
  if (!ctx) {
    return;
  }

  getmaxyx(stdscr, ctx->height, ctx->width);

  if (width) {
    *width = ctx->width;
  }
  if (height) {
    *height = ctx->height;
  }
}

static void ncurses_begin_frame(RenderContext *ctx) {
  if (!ctx) {
    return;
  }

  /* Update size in case of resize */
  getmaxyx(stdscr, ctx->height, ctx->width);

  /* Clear screen */
  erase();
}

static void ncurses_end_frame(RenderContext *ctx) {
  (void)ctx;

  /* Refresh the screen */
  refresh();
}

static void ncurses_handle_resize(RenderContext *ctx) {
  if (!ctx) {
    return;
  }

  /* ncurses handles resize via KEY_RESIZE */
  getmaxyx(stdscr, ctx->height, ctx->width);
}

/* ============================================================================
 * Backend Implementation - Drawing Primitives
 * ============================================================================
 */

static void ncurses_set_color(RenderContext *ctx, UiColor color, UiAttr attrs) {
  if (!ctx) {
    return;
  }

  ctx->cur_color = color;
  ctx->cur_attrs = attrs;

  /* Apply color pair */
  if (ctx->colors_enabled && color < UI_COLOR_COUNT) {
    attron(COLOR_PAIR(color_pair_map[color]));
  }

  /* Apply attributes */
  attr_t ncurses_attrs = ui_attrs_to_ncurses(attrs);
  if (ncurses_attrs != A_NORMAL) {
    attron(ncurses_attrs);
  }
}

static void ncurses_reset_attrs(RenderContext *ctx) {
  if (!ctx) {
    return;
  }

  attrset(A_NORMAL);
  if (ctx->colors_enabled) {
    attron(COLOR_PAIR(color_pair_map[UI_COLOR_DEFAULT]));
  }

  ctx->cur_color = UI_COLOR_DEFAULT;
  ctx->cur_attrs = UI_ATTR_NORMAL;
}

static void ncurses_move_to(RenderContext *ctx, int x, int y) {
  (void)ctx;
  move(y, x);
}

static void ncurses_draw_char(RenderContext *ctx, int x, int y, int ch) {
  (void)ctx;
  mvaddch(y, x, ch);
}

static void ncurses_draw_string(RenderContext *ctx, int x, int y,
                                const char *str) {
  (void)ctx;
  if (!str) {
    return;
  }
  mvaddstr(y, x, str);
}

static void ncurses_draw_string_n(RenderContext *ctx, int x, int y,
                                  const char *str, int max_width) {
  (void)ctx;
  if (!str || max_width <= 0) {
    return;
  }

  move(y, x);

  /* Calculate visible width (handling UTF-8) */
  int printed = 0;
  const char *p = str;

  while (*p && printed < max_width) {
    /* Get next UTF-8 character */
    wchar_t wc;
    int len = mbtowc(&wc, p, MB_CUR_MAX);
    if (len <= 0) {
      /* Invalid UTF-8, skip byte */
      p++;
      continue;
    }

    /* Get display width of character */
    int char_width = wcwidth(wc);
    if (char_width < 0) {
      char_width = 1; /* Non-printable, assume width 1 */
    }

    if (printed + char_width > max_width) {
      break;
    }

    /* Print character */
    addnstr(p, len);
    printed += char_width;
    p += len;
  }

  /* Pad with spaces if needed */
  while (printed < max_width) {
    addch(' ');
    printed++;
  }
}

static void ncurses_draw_hline(RenderContext *ctx, int x, int y, int width,
                               int ch) {
  (void)ctx;
  mvhline(y, x, ch, width);
}

static void ncurses_draw_vline(RenderContext *ctx, int x, int y, int height,
                               int ch) {
  (void)ctx;
  mvvline(y, x, ch, height);
}

static void ncurses_draw_box(RenderContext *ctx, int x, int y, int width,
                             int height) {
  (void)ctx;
  if (width < 2 || height < 2) {
    return;
  }

  /* Draw corners */
  mvaddch(y, x, ACS_ULCORNER);
  mvaddch(y, x + width - 1, ACS_URCORNER);
  mvaddch(y + height - 1, x, ACS_LLCORNER);
  mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);

  /* Draw horizontal lines */
  mvhline(y, x + 1, ACS_HLINE, width - 2);
  mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);

  /* Draw vertical lines */
  mvvline(y + 1, x, ACS_VLINE, height - 2);
  mvvline(y + 1, x + width - 1, ACS_VLINE, height - 2);
}

static void ncurses_fill_rect(RenderContext *ctx, int x, int y, int width,
                              int height, int ch) {
  (void)ctx;
  for (int row = 0; row < height; row++) {
    mvhline(y + row, x, ch, width);
  }
}

static void ncurses_clear_rect(RenderContext *ctx, int x, int y, int width,
                               int height) {
  ncurses_fill_rect(ctx, x, y, width, height, ' ');
}

/* ============================================================================
 * Backend Implementation - Input Handling
 * ============================================================================
 */

static bool ncurses_poll_event(RenderContext *ctx, UiEvent *event) {
  if (!ctx || !event) {
    return false;
  }

  memset(event, 0, sizeof(UiEvent));

  /* Set non-blocking mode temporarily */
  nodelay(stdscr, TRUE);
  int ch = getch();
  nodelay(stdscr, FALSE);

  if (ch == ERR) {
    event->type = UI_EVENT_NONE;
    return false;
  }

  /* Handle mouse events */
  if (ch == KEY_MOUSE) {
    MEVENT mevent;
    if (getmouse(&mevent) == OK) {
      return translate_mouse(&mevent, event);
    }
    event->type = UI_EVENT_NONE;
    return false;
  }

  /* Handle keyboard events */
  translate_key(ch, event);
  return true;
}

static bool ncurses_wait_event(RenderContext *ctx, UiEvent *event,
                               int timeout_ms) {
  if (!ctx || !event) {
    return false;
  }

  memset(event, 0, sizeof(UiEvent));

  /* Set timeout */
  if (timeout_ms < 0) {
    /* Block forever */
    nodelay(stdscr, FALSE);
    timeout(-1);
  } else if (timeout_ms == 0) {
    /* Non-blocking */
    nodelay(stdscr, TRUE);
  } else {
    /* Timeout in milliseconds */
    nodelay(stdscr, FALSE);
    timeout(timeout_ms);
  }

  int ch = getch();

  /* Reset to default */
  nodelay(stdscr, FALSE);
  timeout(-1);

  if (ch == ERR) {
    event->type = UI_EVENT_NONE;
    return false;
  }

  /* Handle mouse events */
  if (ch == KEY_MOUSE) {
    MEVENT mevent;
    if (getmouse(&mevent) == OK) {
      return translate_mouse(&mevent, event);
    }
    event->type = UI_EVENT_NONE;
    return false;
  }

  /* Handle keyboard events */
  translate_key(ch, event);
  return true;
}

static void ncurses_set_mouse_enabled(RenderContext *ctx, bool enabled) {
  if (!ctx) {
    return;
  }

  if (enabled && !ctx->mouse_enabled) {
    /* Enable mouse */
    mmask_t mask = ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION;
    mousemask(mask, NULL);
    mouseinterval(0); /* No click delay */
    ctx->mouse_enabled = true;
  } else if (!enabled && ctx->mouse_enabled) {
    /* Disable mouse */
    mousemask(0, NULL);
    ctx->mouse_enabled = false;
  }
}

/* ============================================================================
 * Backend Implementation - Cursor
 * ============================================================================
 */

static void ncurses_set_cursor_visible(RenderContext *ctx, bool visible) {
  (void)ctx;
  curs_set(visible ? 1 : 0);
}

static void ncurses_set_cursor_pos(RenderContext *ctx, int x, int y) {
  (void)ctx;
  move(y, x);
}

/* ============================================================================
 * Backend Instance
 * ============================================================================
 */

static const RenderBackend s_ncurses_backend = {
    .name = "ncurses",

    /* Lifecycle */
    .init = ncurses_init,
    .shutdown = ncurses_shutdown,

    /* Display management */
    .get_size = ncurses_get_size,
    .begin_frame = ncurses_begin_frame,
    .end_frame = ncurses_end_frame,
    .handle_resize = ncurses_handle_resize,

    /* Drawing primitives */
    .set_color = ncurses_set_color,
    .reset_attrs = ncurses_reset_attrs,
    .move_to = ncurses_move_to,
    .draw_char = ncurses_draw_char,
    .draw_string = ncurses_draw_string,
    .draw_string_n = ncurses_draw_string_n,
    .draw_hline = ncurses_draw_hline,
    .draw_vline = ncurses_draw_vline,
    .draw_box = ncurses_draw_box,
    .fill_rect = ncurses_fill_rect,
    .clear_rect = ncurses_clear_rect,

    /* Input handling */
    .poll_event = ncurses_poll_event,
    .wait_event = ncurses_wait_event,
    .set_mouse_enabled = ncurses_set_mouse_enabled,

    /* Cursor */
    .set_cursor_visible = ncurses_set_cursor_visible,
    .set_cursor_pos = ncurses_set_cursor_pos,
};

/* ============================================================================
 * Public API
 * ============================================================================
 */

static const RenderBackend *s_current_backend = NULL;

const RenderBackend *render_backend_ncurses(void) { return &s_ncurses_backend; }

const RenderBackend *render_backend_current(void) {
  /* Default to ncurses if not set */
  if (!s_current_backend) {
    s_current_backend = &s_ncurses_backend;
  }
  return s_current_backend;
}

void render_backend_set(const RenderBackend *backend) {
  s_current_backend = backend;
}
