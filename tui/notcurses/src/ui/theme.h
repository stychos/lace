#ifndef LACE_NC_THEME_H
#define LACE_NC_THEME_H
#include <stdbool.h>
#include <stdint.h>

struct ncplane;
typedef enum { TC_BG, TC_FG, TC_DIM, TC_HEADER_BG, TC_HEADER_FG, TC_STATUS_BG, TC_STATUS_FG,
               TC_CURSOR_BG, TC_CURSOR_FG, TC_ROW_BG, TC_STRIPE_BG, TC_SELECTED_FG, TC_NULL_FG,
               TC_NUMBER_FG, TC_PK_FG, TC_ERROR_FG, TC_ACCENT, TC_OVERLAY_BG, TC_OVERLAY_FG,
               TC_OVERLAY_BORDER, TC_SHADOW_BG, TC_KEY_FG, TC_EDIT_BG, TC_COUNT } ThemeColor;
typedef struct { uint32_t rgb[TC_COUNT]; bool dark; } Theme;

/* color_scheme: 0 system (terminal_dark decides), 1 light, 2 dark */
void theme_init(Theme *t, int color_scheme, bool terminal_dark);
void nc_style(struct ncplane *p, const Theme *t, ThemeColor fg, ThemeColor bg, unsigned ncstyles);
/* Clip and pad to width columns; returns columns used */
int nc_put(struct ncplane *p, int y, int x, int width, const char *text);
int nc_put_right(struct ncplane *p, int y, int x, int width, const char *text);
int nc_text_width(const char *text);
#endif
