/*
 * Lace
 * The TUIs' colour theme (notcurses' since it was made, ncurses' too): the
 * roles every screen element is drawn in and their colours, dark and
 * light; and the nearest xterm-256 / 8 colours for a terminal without RGB
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_THEME_RGB_H
#define LACE_THEME_RGB_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { TC_BG, TC_FG, TC_DIM, TC_HEADER_BG, TC_HEADER_FG, TC_STATUS_BG, TC_STATUS_FG,
               TC_CURSOR_BG, TC_CURSOR_FG, TC_ROW_BG, TC_STRIPE_BG, TC_SELECTED_FG, TC_NULL_FG,
               TC_NUMBER_FG, TC_PK_FG, TC_ERROR_FG, TC_ACCENT, TC_OVERLAY_BG, TC_OVERLAY_FG,
               TC_OVERLAY_BORDER, TC_SHADOW_BG, TC_KEY_FG, TC_EDIT_BG, TC_COUNT } ThemeColor;

/* The colours (0xRRGGBB) of every role */
const uint32_t *theme_palette(bool dark);
/* general.color_scheme: 0 system (the terminal decides), 1 light, 2 dark */
bool theme_is_dark(int color_scheme, bool terminal_dark);
/* The nearest of the xterm 256 colours: the 6x6x6 cube (16..231) or the
 * grey ramp (232..255) */
int theme_xterm256(uint32_t rgb);
/* The nearest of the 8 ANSI colours (0 black .. 7 white) */
int theme_ansi8(uint32_t rgb);

#endif /* LACE_THEME_RGB_H */
