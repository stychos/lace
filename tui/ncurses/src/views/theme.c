/*
 * Lace
 * The ncurses TUI's colours: see theme.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "theme.h"
#include "../tui.h"
#include <fcntl.h>
#include <sys/select.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define FIRST_PAIR 32 /* below: the fixed pairs (COLOR_HEADER ...) */

static bool g_active, g_dark = true;
static const uint32_t *g_rgb;
static short g_pairs[TC_COUNT][TC_COUNT];
static int g_next_pair = FIRST_PAIR;

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* "rgb:RRRR/GGGG/BBBB" (1 to 4 hex digits each) in an OSC 11 reply */
static bool parse_rgb(const char *s, int *r, int *g, int *b) {
  const char *p = strstr(s, "rgb:");
  if (!p)
    return false;
  p += 4;
  int *out[3] = {r, g, b};
  for (int k = 0; k < 3; k++) {
    char *end;
    unsigned long v = strtoul(p, &end, 16);
    size_t digits = (size_t)(end - p);
    if (digits == 0 || digits > 4)
      return false;
    *out[k] = (int)(v * 255 / ((1ul << (4 * digits)) - 1));
    p = end;
    if (k < 2) {
      if (*p != '/')
        return false;
      p++;
    }
  }
  return true;
}

size_t tui_strip_replies(char *buf, size_t n) {
  size_t out = 0;
  for (size_t i = 0; i < n;) {
    if (buf[i] == '\033' && i + 1 < n && buf[i + 1] == ']') { /* OSC: up to BEL or ESC \ */
      size_t j = i + 2;
      while (j < n && buf[j] != '\007' && !(buf[j] == '\033' && j + 1 < n && buf[j + 1] == '\\'))
        j++;
      i = j < n ? (buf[j] == '\007' ? j + 1 : j + 2) : n;
      continue;
    }
    if (buf[i] == '\033' && i + 2 < n && buf[i + 1] == '[' && buf[i + 2] == '?') { /* DA1: up to 'c' */
      size_t j = i + 3;
      while (j < n && buf[j] != 'c' && ((buf[j] >= '0' && buf[j] <= '9') || buf[j] == ';'))
        j++;
      if (j < n && buf[j] == 'c') {
        i = j + 1;
        continue;
      }
    }
    buf[out++] = buf[i++];
  }
  return out;
}

bool tui_terminal_dark(char *typeahead, size_t cap, size_t *len) {
  *len = 0;
  /* Ask the terminal (OSC 11), with a primary device attributes query
   * after it that every terminal answers: once that reply is in, so is the
   * colour, if the terminal knows it; nothing is left for the screen to read */
  bool dark = true, known = false;
  int fd = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) ? open("/dev/tty", O_RDWR | O_NOCTTY) : -1;
  if (fd >= 0) {
    struct termios old, raw;
    if (tcgetattr(fd, &old) == 0) {
      raw = old;
      raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
      raw.c_cc[VMIN] = 0;
      raw.c_cc[VTIME] = 0;
      tcsetattr(fd, TCSANOW, &raw);
      static const char q[] = "\033]11;?\033\\\033[c";
      if (write(fd, q, sizeof q - 1) == (ssize_t)(sizeof q - 1)) {
        char buf[256];
        size_t n = 0;
        /* every terminal answers DA1 at once; one that does not is not
         * waited for longer than this */
        int64_t deadline = now_ms() + 1000;
        bool da = false;
        while (!da && n < sizeof buf - 1) {
          int left = (int)(deadline - now_ms());
          if (left <= 0)
            break;
          /* select, not poll: macOS's poll() does not work on terminals */
          fd_set set;
          FD_ZERO(&set);
          FD_SET(fd, &set);
          struct timeval tv = {left / 1000, (left % 1000) * 1000};
          if (select(fd + 1, &set, NULL, NULL, &tv) <= 0)
            break;
          ssize_t got = read(fd, buf + n, sizeof buf - 1 - n);
          if (got <= 0)
            break;
          n += (size_t)got;
          buf[n] = '\0';
          /* the DA1 reply: ESC [ ? ... c */
          char *d = strstr(buf, "\033[?");
          da = d && strchr(d, 'c');
        }
        buf[n] = '\0';
        int r, g, b;
        if (parse_rgb(buf, &r, &g, &b)) {
          dark = (r + g + b) / 3 <= 127;
          known = true;
        }
        /* What the user typed meanwhile is theirs: handed back */
        size_t keep = tui_strip_replies(buf, n);
        if (keep > cap)
          keep = cap;
        memcpy(typeahead, buf, keep);
        *len = keep;
      }
      tcsetattr(fd, TCSANOW, &old);
    }
    close(fd);
  }
  if (!known) {
    /* "fg;bg" (or "fg;default;bg"): the last number is the background */
    const char *c = getenv("COLORFGBG");
    const char *semi = c ? strrchr(c, ';') : NULL;
    if (semi) {
      int bg = atoi(semi + 1);
      dark = !(bg == 7 || bg == 15); /* white or bright white */
    }
  }
  return dark;
}

/* Fewer than 256 colours: the nearest of eight would make most roles
 * black, so each role has a fixed one (-1 the terminal's default), the
 * black-on-cyan bars this TUI always had */
static const short ANSI_ROLE[TC_COUNT] = {
    [TC_BG] = -1,           [TC_FG] = -1,           [TC_DIM] = -1,
    [TC_HEADER_BG] = COLOR_CYAN, [TC_HEADER_FG] = COLOR_BLACK,
    [TC_STATUS_BG] = COLOR_CYAN, [TC_STATUS_FG] = COLOR_BLACK,
    [TC_CURSOR_BG] = COLOR_CYAN, [TC_CURSOR_FG] = COLOR_BLACK,
    [TC_ROW_BG] = -1,       [TC_STRIPE_BG] = -1,    [TC_SELECTED_FG] = COLOR_YELLOW,
    [TC_NULL_FG] = COLOR_MAGENTA, [TC_NUMBER_FG] = COLOR_CYAN, [TC_PK_FG] = COLOR_YELLOW,
    [TC_ERROR_FG] = COLOR_RED, [TC_ACCENT] = COLOR_YELLOW, [TC_OVERLAY_BG] = -1,
    [TC_OVERLAY_FG] = -1,   [TC_OVERLAY_BORDER] = COLOR_CYAN, [TC_SHADOW_BG] = COLOR_BLACK,
    [TC_KEY_FG] = COLOR_CYAN, [TC_EDIT_BG] = COLOR_YELLOW,
};
static bool g_ansi; /* the theme on 8 or 16 colours */

static short color(ThemeColor c) { return g_ansi ? ANSI_ROLE[c] : (short)theme_xterm256(g_rgb[c]); }

void tui_theme_init(int color_scheme, bool terminal_dark) {
  g_dark = theme_is_dark(color_scheme, terminal_dark);
  g_rgb = theme_palette(g_dark);
  memset(g_pairs, 0, sizeof g_pairs);
  g_next_pair = FIRST_PAIR;
  g_active = has_colors() && COLORS >= 256 && COLOR_PAIRS > FIRST_PAIR + 16;
  g_ansi = !g_active && has_colors() && COLORS >= 8 && COLOR_PAIRS > FIRST_PAIR + 16;
  if (g_ansi) { /* the legacy pairs stay as they are */
    g_active = true;
    return;
  }
  if (!g_active)
    return;
  assume_default_colors(color(TC_FG), color(TC_BG));
  /* The fixed pairs of the code drawn before the theme */
  init_pair(COLOR_HEADER, color(TC_HEADER_FG), color(TC_HEADER_BG));
  init_pair(COLOR_SELECTED, color(TC_CURSOR_FG), color(TC_CURSOR_BG));
  init_pair(COLOR_STATUS, color(TC_STATUS_FG), color(TC_STATUS_BG));
  init_pair(COLOR_ERROR, color(TC_ERROR_FG), color(TC_STATUS_BG));
  init_pair(COLOR_BORDER, color(TC_OVERLAY_BORDER), color(TC_BG));
  init_pair(COLOR_TITLE, color(TC_ACCENT), color(TC_BG));
  init_pair(COLOR_NULL, color(TC_NULL_FG), color(TC_BG));
  init_pair(COLOR_NUMBER, color(TC_NUMBER_FG), color(TC_BG));
  init_pair(COLOR_EDIT, color(TC_FG), color(TC_EDIT_BG));
  init_pair(COLOR_ERROR_TEXT, color(TC_ERROR_FG), color(TC_BG));
  init_pair(COLOR_PK, color(TC_PK_FG), color(TC_BG));
}

bool tui_theme_active(void) { return g_active; }
bool tui_theme_dark(void) { return g_dark; }

attr_t tui_theme_attr(ThemeColor fg, ThemeColor bg) {
  if (!g_active || fg >= TC_COUNT || bg >= TC_COUNT)
    return 0;
  short p = g_pairs[fg][bg];
  if (!p) {
    if (g_next_pair >= COLOR_PAIRS)
      return COLOR_PAIR(0);
    p = (short)g_next_pair++;
    init_pair(p, color(fg), color(bg));
    g_pairs[fg][bg] = p;
  }
  return COLOR_PAIR(p);
}

void tui_style(WINDOW *w, ThemeColor fg, ThemeColor bg, attr_t attrs) {
  if (w)
    wattrset(w, tui_theme_attr(fg, bg) | attrs);
}
