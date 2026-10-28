/*
 * Lace
 * The notcurses TUI's overlays this TUI has too: the log and the view
 * switcher
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_PANELS_H
#define LACE_TUI_PANELS_H

#include "../tui.h"

/* The log (toggle_log: Ctrl+`, F8): every status message, newest at the
 * bottom; Enter copies a line */
void tui_log_open(TuiState *state);

/* The view switcher (view_switcher: Ctrl+E): the workspace's tabs as the
 * strip has them, with their filters and sorts; Enter switches, x closes */
void tui_view_switcher_open(TuiState *state);

#endif /* LACE_TUI_PANELS_H */
