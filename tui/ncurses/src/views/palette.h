/*
 * Lace
 * The command palette (Ctrl+P, :), as the notcurses TUI's
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_PALETTE_H
#define LACE_TUI_PALETTE_H

#include "../tui.h"

/* One filterable list of the actions that apply where the focus is, the
 * tables, the connection's tabs, the workspaces, the connections and the
 * on/off preferences; the picked entry runs once the palette closed */
void tui_palette_open(TuiState *state);

/* Whether the palette lists an action now: this frontend's, meaning
 * something with what is open (notcurses' rules), not only an editor's
 * or a dialog's, and bound to a key */
bool tui_palette_lists_action(TuiState *state, HotkeyAction a);

/* Run an action through its first key, as if pressed: the focus goes where
 * the key means that action first (the palette and the status line's
 * hints run actions this way) */
void tui_run_action(TuiState *state, HotkeyAction a);

#endif /* LACE_TUI_PALETTE_H */
