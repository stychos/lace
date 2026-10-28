#ifndef LACE_NC_THEME_H
#define LACE_NC_THEME_H
#include "core/theme_rgb.h" /* the roles and their colours, shared with the ncurses TUI */
#include <stdbool.h>
#include <stdint.h>

struct ncplane;
typedef struct { uint32_t rgb[TC_COUNT]; bool dark; } Theme;

/* color_scheme: 0 system (terminal_dark decides), 1 light, 2 dark */
void theme_init(Theme *t, int color_scheme, bool terminal_dark);
void nc_style(struct ncplane *p, const Theme *t, ThemeColor fg, ThemeColor bg, unsigned ncstyles);
/* Clip and pad to width columns; returns columns used */
int nc_put(struct ncplane *p, int y, int x, int width, const char *text);
int nc_put_right(struct ncplane *p, int y, int x, int width, const char *text);
int nc_text_width(const char *text);
#endif
