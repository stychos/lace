/*
 * Lace
 * Pick-one list popup (filter column and operator dropdowns)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_LIST_PICKER_H
#define LACE_LIST_PICKER_H

#include "../config/config.h"
#include <stddef.h>

/* Show labels[0..n) in a bordered popup with its top-left corner at screen
 * (y, x), moved to fit the screen, at least min_width and at most
 * max_width columns wide and max_height rows tall (borders included). The
 * user's movement, page and first/last-row hotkeys move the selection;
 * Enter or a click picks; Esc, q or a click outside cancels. Starts on
 * current. Returns the picked index, or -1. */
int tui_pick_from_list(const Config *cfg, const char *title,
                       const char *const *labels, size_t n, size_t current,
                       int y, int x, int min_width, int max_width,
                       int max_height);

/* Move a list's selection with the user's movement hotkeys (up/down, page
 * up/down, first/last row), scrolling so it stays within the visible rows.
 * Returns true if the key was one of them. */
bool tui_list_nav_key(const Config *cfg, const UiEvent *event, size_t count,
                      size_t visible, size_t *selected, size_t *scroll);

#endif /* LACE_LIST_PICKER_H */
