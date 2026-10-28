#include "theme.h"
#include "core/utf8.h"
#include <notcurses/notcurses.h>
#include <string.h>

void theme_init(Theme *t, int color_scheme, bool terminal_dark) {
  t->dark = theme_is_dark(color_scheme, terminal_dark);
  memcpy(t->rgb, theme_palette(t->dark), sizeof t->rgb);
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
