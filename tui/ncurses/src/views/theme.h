/*
 * Lace
 * The ncurses TUI's colours: the notcurses theme (core/theme_rgb.h) on the
 * terminal's xterm-256 colours. Pairs are made on first use for each
 * foreground/background role; the old fixed pairs (COLOR_HEADER, ...) are
 * pointed at theme roles too. A terminal of fewer than 256 colours keeps
 * the old ANSI pairs.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_THEME_H
#define LACE_TUI_THEME_H

#include "../core/theme_rgb.h"
#include <ncurses.h>
#include <stdbool.h>

/* The terminal's background is dark (asked with OSC 11 before initscr(),
 * else $COLORFGBG; dark when neither says). Keys typed before the answer
 * are kept: up to cap bytes in typeahead (*len), for ungetch() once the
 * screen is up. */
bool tui_terminal_dark(char *typeahead, size_t cap, size_t *len);
/* The terminal replies in buf (OSC 11's ESC ] ... BEL/ST, DA1's ESC [ ? ... c)
 * cut out, the rest moved to the front; the bytes left (tests) */
size_t tui_strip_replies(char *buf, size_t n);
/* After start_color(): general.color_scheme (0 system, 1 light, 2 dark) */
void tui_theme_init(int color_scheme, bool terminal_dark);
/* The theme is in use (a 256-colour terminal) */
bool tui_theme_active(void);
bool tui_theme_dark(void);
/* COLOR_PAIR() of fg on bg, for wattrset/wbkgd; 0 without the theme */
attr_t tui_theme_attr(ThemeColor fg, ThemeColor bg);
/* wattrset(w, fg on bg | attrs) */
void tui_style(WINDOW *w, ThemeColor fg, ThemeColor bg, attr_t attrs);

#endif /* LACE_TUI_THEME_H */
