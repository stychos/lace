/*
 * Lace
 * Single-line text input: ncurses drawing
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TEXT_INPUT_DRAW_H
#define LACE_TEXT_INPUT_DRAW_H

#include "text_input.h"
#include <ncurses.h>

/* Draw width columns at (y, x) with attr. Focused: scrolled so the cursor
 * shows, which is drawn in reverse video; returns its screen column (-1 if
 * not visible). Unfocused: the start of the text, no cursor; returns -1. */
int text_input_draw(WINDOW *win, const TextInput *in, int y, int x, int width,
                    attr_t attr, bool focused);

#endif /* LACE_TEXT_INPUT_DRAW_H */
