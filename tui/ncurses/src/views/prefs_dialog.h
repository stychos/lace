/*
 * Lace
 * Preferences, as the notcurses TUI's: the shared settings table
 * (core/prefs_model.c) this frontend shows, changed in place and saved at
 * once
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_PREFS_DIALOG_H
#define LACE_TUI_PREFS_DIALOG_H

#include "../tui.h"

/* The overlay: Space, Enter or Right change a row forward, Left back; the
 * last row opens the hotkeys editor */
void tui_prefs_open(TuiState *state);

/* Change setting i of the shared table by dir and save config.json; what
 * it affects on screen follows (the bars, the colours). The palette's
 * preference rows come here too. False when the save failed (said so). */
bool tui_prefs_change(TuiState *state, int i, int dir);

#endif /* LACE_TUI_PREFS_DIALOG_H */
