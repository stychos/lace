/*
 * Lace
 * Sort by column: the notcurses TUI's sort picker
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_SORT_PICKER_H
#define LACE_TUI_SORT_PICKER_H

#include "../tui.h"

/* The table tab's columns with their sorts (↑ ascending, ↓ descending, a
 * number when there are several); Enter cycles one ↑, ↓, off, and closing
 * applies what changed with one reload */
void tui_sort_picker_open(TuiState *state);

#endif /* LACE_TUI_SORT_PICKER_H */
