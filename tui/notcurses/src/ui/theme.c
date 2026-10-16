#include "theme.h"
#include "core/utf8.h"
#include <notcurses/notcurses.h>
#include <string.h>

static const uint32_t DARK[TC_COUNT] = {
    [TC_BG] = 0x1e1e2e,          [TC_FG] = 0xcdd6f4,         [TC_DIM] = 0x7f849c,
    [TC_HEADER_BG] = 0x313244,   [TC_HEADER_FG] = 0xcdd6f4,  [TC_STATUS_BG] = 0x313244,
    [TC_STATUS_FG] = 0xbac2de,   [TC_CURSOR_BG] = 0x45475a,  [TC_CURSOR_FG] = 0xffffff,
    [TC_ROW_BG] = 0x1e1e2e,      [TC_STRIPE_BG] = 0x24243a,  [TC_SELECTED_FG] = 0xf5c2e7,
    [TC_NULL_FG] = 0x6c7086,     [TC_NUMBER_FG] = 0x89b4fa,  [TC_PK_FG] = 0xf9e2af,
    [TC_ERROR_FG] = 0xf38ba8,    [TC_ACCENT] = 0x89dceb,     [TC_OVERLAY_BG] = 0x181825,
    [TC_OVERLAY_FG] = 0xcdd6f4,  [TC_OVERLAY_BORDER] = 0x585b70, [TC_SHADOW_BG] = 0x11111b,
    [TC_KEY_FG] = 0xa6e3a1,      [TC_EDIT_BG] = 0x3b3b5a,
};

static const uint32_t LIGHT[TC_COUNT] = {
    [TC_BG] = 0xeff1f5,          [TC_FG] = 0x4c4f69,         [TC_DIM] = 0x7c7f93,
    [TC_HEADER_BG] = 0xccd0da,   [TC_HEADER_FG] = 0x4c4f69,  [TC_STATUS_BG] = 0xccd0da,
    [TC_STATUS_FG] = 0x5c5f77,   [TC_CURSOR_BG] = 0xbcc0cc,  [TC_CURSOR_FG] = 0x000000,
    [TC_ROW_BG] = 0xeff1f5,      [TC_STRIPE_BG] = 0xe6e9ef,  [TC_SELECTED_FG] = 0x8839ef,
    [TC_NULL_FG] = 0x9ca0b0,     [TC_NUMBER_FG] = 0x1e66f5,  [TC_PK_FG] = 0xdf8e1d,
    [TC_ERROR_FG] = 0xd20f39,    [TC_ACCENT] = 0x04a5e5,     [TC_OVERLAY_BG] = 0xe6e9ef,
    [TC_OVERLAY_FG] = 0x4c4f69,  [TC_OVERLAY_BORDER] = 0x9ca0b0, [TC_SHADOW_BG] = 0xdce0e8,
    [TC_KEY_FG] = 0x40a02b,      [TC_EDIT_BG] = 0xdcdff0,
};

void theme_init(Theme *t, int color_scheme, bool terminal_dark) {
  t->dark = color_scheme == 2 || (color_scheme != 1 && terminal_dark);
  memcpy(t->rgb, t->dark ? DARK : LIGHT, sizeof t->rgb);
}

void nc_style(struct ncplane *p, const Theme *t, ThemeColor fg, ThemeColor bg, unsigned ncstyles) {
  ncplane_set_fg_rgb(p, t->rgb[fg]);
  ncplane_set_bg_rgb(p, t->rgb[bg]);
  ncplane_set_styles(p, ncstyles);
}

int nc_text_width(const char *text) {
  int w = ncstrwidth(text, NULL, NULL);
  return w < 0 ? (int)strlen(text) : w;
}

int nc_put(struct ncplane *p, int y, int x, int width, const char *text) {
  if (width <= 0) return 0;
  int cols = 0;
  size_t n = utf8_fit(text, strlen(text), width, &cols);
  char *clip = strndup(text, n);
  if (!clip) return 0;
  ncplane_putstr_yx(p, y, x, clip);
  free(clip);
  if (cols < width) ncplane_printf_yx(p, y, x + cols, "%*s", width - cols, "");
  return width;
}

int nc_put_right(struct ncplane *p, int y, int x, int width, const char *text) {
  if (width <= 0) return 0;
  int tw = nc_text_width(text);
  if (tw >= width) return nc_put(p, y, x, width, text);
  ncplane_printf_yx(p, y, x, "%*s", width - tw, "");
  ncplane_putstr_yx(p, y, x + width - tw, text);
  return width;
}
