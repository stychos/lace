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

  /* Region management */
  WINDOW *regions[UI_REGION_COUNT];        /* ncurses windows for each region */
  UiRegionBounds region_bounds[UI_REGION_COUNT]; /* Stored bounds */
  UiRegionId current_region;               /* Currently active region */
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

  /* Initialize color pairs matching TUI color scheme in tui.c */
  /* Format: init_pair(pair_number, foreground, background) */

  /* UI_COLOR_DEFAULT - default terminal colors */
  init_pair(1, -1, -1);
  color_pair_map[UI_COLOR_DEFAULT] = 1;

  /* UI_COLOR_HEADER - black on cyan */
  init_pair(2, COLOR_BLACK, COLOR_CYAN);
  color_pair_map[UI_COLOR_HEADER] = 2;

  /* UI_COLOR_SELECTED - black on cyan */
  init_pair(3, COLOR_BLACK, COLOR_CYAN);
  color_pair_map[UI_COLOR_SELECTED] = 3;

  /* UI_COLOR_STATUS - black on cyan */
  init_pair(4, COLOR_BLACK, COLOR_CYAN);
  color_pair_map[UI_COLOR_STATUS] = 4;

  /* UI_COLOR_ERROR - white on red */
  init_pair(5, COLOR_WHITE, COLOR_RED);
  color_pair_map[UI_COLOR_ERROR] = 5;

  /* UI_COLOR_BORDER - cyan on default */
  init_pair(6, COLOR_CYAN, -1);
  color_pair_map[UI_COLOR_BORDER] = 6;

  /* UI_COLOR_TITLE - yellow on default */
  init_pair(7, COLOR_YELLOW, -1);
  color_pair_map[UI_COLOR_TITLE] = 7;

  /* UI_COLOR_NULL - magenta on default */
  init_pair(8, COLOR_MAGENTA, -1);
  color_pair_map[UI_COLOR_NULL] = 8;

  /* UI_COLOR_NUMBER - cyan on default */
  init_pair(9, COLOR_CYAN, -1);
  color_pair_map[UI_COLOR_NUMBER] = 9;

  /* UI_COLOR_EDIT - black on yellow */
  init_pair(10, COLOR_BLACK, COLOR_YELLOW);
  color_pair_map[UI_COLOR_EDIT] = 10;

  /* UI_COLOR_ERROR_TEXT - red on default */
  init_pair(11, COLOR_RED, -1);
  color_pair_map[UI_COLOR_ERROR_TEXT] = 11;

  /* UI_COLOR_PK - yellow on default (primary key indicator) */
  init_pair(12, COLOR_YELLOW, -1);
  color_pair_map[UI_COLOR_PK] = 12;
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

  /* Clean up region windows */
  for (int i = 0; i < UI_REGION_COUNT; i++) {
    if (ctx->regions[i]) {
      delwin(ctx->regions[i]);
      ctx->regions[i] = NULL;
    }
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

/* Get the WINDOW to draw to (current region or stdscr) */
static WINDOW *get_draw_window(RenderContext *ctx) {
  if (!ctx)
    return stdscr;
  WINDOW *win = ctx->regions[ctx->current_region];
  return win ? win : stdscr;
}

static void ncurses_set_color(RenderContext *ctx, UiColor color, UiAttr attrs) {
  if (!ctx) {
    return;
  }

  ctx->cur_color = color;
  ctx->cur_attrs = attrs;

  WINDOW *win = get_draw_window(ctx);

  /* Apply color pair */
  if (ctx->colors_enabled && color < UI_COLOR_COUNT) {
    wattron(win, COLOR_PAIR(color_pair_map[color]));
  }

  /* Apply attributes */
  attr_t ncurses_attrs = ui_attrs_to_ncurses(attrs);
  if (ncurses_attrs != A_NORMAL) {
    wattron(win, ncurses_attrs);
  }
}

static void ncurses_reset_attrs(RenderContext *ctx) {
  if (!ctx) {
    return;
  }

  WINDOW *win = get_draw_window(ctx);
  wattrset(win, A_NORMAL);
  if (ctx->colors_enabled) {
    wattron(win, COLOR_PAIR(color_pair_map[UI_COLOR_DEFAULT]));
  }

  ctx->cur_color = UI_COLOR_DEFAULT;
  ctx->cur_attrs = UI_ATTR_NORMAL;
}

static void ncurses_move_to(RenderContext *ctx, int x, int y) {
  WINDOW *win = get_draw_window(ctx);
  wmove(win, y, x);
}

static void ncurses_draw_char(RenderContext *ctx, int x, int y, int ch) {
  WINDOW *win = get_draw_window(ctx);
  mvwaddch(win, y, x, ch);
}

static void ncurses_draw_string(RenderContext *ctx, int x, int y,
                                const char *str) {
  WINDOW *win = get_draw_window(ctx);
  if (!str) {
    return;
  }
  mvwaddstr(win, y, x, str);
}

static void ncurses_draw_string_n(RenderContext *ctx, int x, int y,
                                  const char *str, int max_width) {
  WINDOW *win = get_draw_window(ctx);
  if (!str || max_width <= 0) {
    return;
  }

  wmove(win, y, x);

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
    waddnstr(win, p, len);
    printed += char_width;
    p += len;
  }

  /* Pad with spaces if needed */
  while (printed < max_width) {
    waddch(win, ' ');
    printed++;
  }
}

static void ncurses_draw_hline(RenderContext *ctx, int x, int y, int width,
                               int ch) {
  WINDOW *win = get_draw_window(ctx);
  mvwhline(win, y, x, ch, width);
}

static void ncurses_draw_vline(RenderContext *ctx, int x, int y, int height,
                               int ch) {
  WINDOW *win = get_draw_window(ctx);
  mvwvline(win, y, x, ch, height);
}

static void ncurses_draw_box(RenderContext *ctx, int x, int y, int width,
                             int height) {
  WINDOW *win = get_draw_window(ctx);
  if (width < 2 || height < 2) {
    return;
  }

  /* Draw corners */
  mvwaddch(win, y, x, ACS_ULCORNER);
  mvwaddch(win, y, x + width - 1, ACS_URCORNER);
  mvwaddch(win, y + height - 1, x, ACS_LLCORNER);
  mvwaddch(win, y + height - 1, x + width - 1, ACS_LRCORNER);

  /* Draw horizontal lines */
  mvwhline(win, y, x + 1, ACS_HLINE, width - 2);
  mvwhline(win, y + height - 1, x + 1, ACS_HLINE, width - 2);

  /* Draw vertical lines */
  mvwvline(win, y + 1, x, ACS_VLINE, height - 2);
  mvwvline(win, y + 1, x + width - 1, ACS_VLINE, height - 2);
}

static void ncurses_fill_rect(RenderContext *ctx, int x, int y, int width,
                              int height, int ch) {
  WINDOW *win = get_draw_window(ctx);
  for (int row = 0; row < height; row++) {
    mvwhline(win, y + row, x, ch, width);
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
  WINDOW *win = get_draw_window(ctx);
  wmove(win, y, x);
}

/* ============================================================================
 * Backend Implementation - Line Drawing Characters
 * ============================================================================
 */

static int ncurses_get_line_char(RenderContext *ctx, UiLineChar ch) {
  (void)ctx;

  switch (ch) {
  case UI_LINE_HLINE:
    return ACS_HLINE;
  case UI_LINE_VLINE:
    return ACS_VLINE;
  case UI_LINE_ULCORNER:
    return ACS_ULCORNER;
  case UI_LINE_URCORNER:
    return ACS_URCORNER;
  case UI_LINE_LLCORNER:
    return ACS_LLCORNER;
  case UI_LINE_LRCORNER:
    return ACS_LRCORNER;
  case UI_LINE_LTEE:
    return ACS_LTEE;
  case UI_LINE_RTEE:
    return ACS_RTEE;
  case UI_LINE_TTEE:
    return ACS_TTEE;
  case UI_LINE_BTEE:
    return ACS_BTEE;
  case UI_LINE_PLUS:
    return ACS_PLUS;
  default:
    return '-'; /* Fallback */
  }
}

/* ============================================================================
 * Backend Implementation - Region Management
 * ============================================================================
 */

static bool ncurses_set_region(RenderContext *ctx, UiRegionId id, int x, int y,
                               int width, int height) {
  if (!ctx || id < 0 || id >= UI_REGION_COUNT) {
    return false;
  }

  /* Validate dimensions */
  if (width <= 0 || height <= 0) {
    return false;
  }

  /* Store bounds */
  ctx->region_bounds[id].x = x;
  ctx->region_bounds[id].y = y;
  ctx->region_bounds[id].width = width;
  ctx->region_bounds[id].height = height;

  /* Create or resize the ncurses window */
  if (ctx->regions[id]) {
    /* Resize existing window */
    wresize(ctx->regions[id], height, width);
    mvwin(ctx->regions[id], y, x);
  } else {
    /* Create new window */
    ctx->regions[id] = newwin(height, width, y, x);
    if (ctx->regions[id]) {
      keypad(ctx->regions[id], TRUE);
    }
  }

  return ctx->regions[id] != NULL;
}

static bool ncurses_get_region(RenderContext *ctx, UiRegionId id,
                               UiRegionBounds *bounds) {
  if (!ctx || !bounds || id < 0 || id >= UI_REGION_COUNT) {
    return false;
  }

  *bounds = ctx->region_bounds[id];
  return ctx->regions[id] != NULL;
}

static void ncurses_begin_region(RenderContext *ctx, UiRegionId id) {
  if (!ctx || id < 0 || id >= UI_REGION_COUNT) {
    return;
  }

  ctx->current_region = id;

  /* If no region window exists, drawing will go to stdscr */
}

static void ncurses_end_region(RenderContext *ctx) {
  if (!ctx) {
    return;
  }

  ctx->current_region = UI_REGION_MAIN;
}

static void ncurses_clear_region(RenderContext *ctx, UiRegionId id) {
  if (!ctx || id < 0 || id >= UI_REGION_COUNT) {
    return;
  }

  WINDOW *win = ctx->regions[id];
  if (win) {
    werase(win);
  }
}

static void ncurses_refresh_region(RenderContext *ctx, UiRegionId id) {
  if (!ctx || id < 0 || id >= UI_REGION_COUNT) {
    return;
  }

  WINDOW *win = ctx->regions[id];
  if (win) {
    wnoutrefresh(win);
  }
}

/* ============================================================================
 * Backend Implementation - Native Handle Access
 * ============================================================================
 */

static void *ncurses_get_region_handle(RenderContext *ctx, UiRegionId id) {
  if (!ctx || id < 0 || id >= UI_REGION_COUNT) {
    return NULL;
  }
  return ctx->regions[id];
}

static void ncurses_set_region_handle(RenderContext *ctx, UiRegionId id,
                                      void *handle) {
  if (!ctx || id < 0 || id >= UI_REGION_COUNT) {
    return;
  }

  /* Store the external window handle */
  ctx->regions[id] = (WINDOW *)handle;

  /* Update bounds if handle is valid */
  if (handle) {
    int height, width;
    getmaxyx((WINDOW *)handle, height, width);
    int y, x;
    getbegyx((WINDOW *)handle, y, x);
    ctx->region_bounds[id].x = x;
    ctx->region_bounds[id].y = y;
    ctx->region_bounds[id].width = width;
    ctx->region_bounds[id].height = height;
  }
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

    /* Line drawing characters */
    .get_line_char = ncurses_get_line_char,

    /* Region management */
    .set_region = ncurses_set_region,
    .get_region = ncurses_get_region,
    .begin_region = ncurses_begin_region,
    .end_region = ncurses_end_region,
    .clear_region = ncurses_clear_region,
    .refresh_region = ncurses_refresh_region,

    /* Native handle access */
    .get_region_handle = ncurses_get_region_handle,
    .set_region_handle = ncurses_set_region_handle,
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

RenderContext *render_context_wrap_ncurses(void) {
  /* Create context that wraps existing ncurses session (no initscr call) */
  RenderContext *ctx = calloc(1, sizeof(RenderContext));
  if (!ctx) {
    return NULL;
  }

  /* Use stdscr as main window (ncurses already initialized) */
  ctx->main_win = stdscr;

  /* Get current dimensions */
  getmaxyx(stdscr, ctx->height, ctx->width);

  /* Check if colors are available */
  ctx->colors_enabled = has_colors();

  ctx->cur_color = UI_COLOR_DEFAULT;
  ctx->cur_attrs = UI_ATTR_NORMAL;
  ctx->current_region = UI_REGION_MAIN;

  /* Mouse may already be enabled by caller */
  ctx->mouse_enabled = true;

  return ctx;
}
