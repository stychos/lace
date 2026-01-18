/*
 * Lace
 * TUI Input Helpers
 *
 * Provides input translation functions for converting ncurses input
 * to platform-independent UiEvent structures.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_RENDER_HELPERS_H
#define LACE_TUI_RENDER_HELPERS_H

#include "core/ui_types.h"
#include <stdbool.h>

/* ============================================================================
 * Input Translation Helpers
 * ============================================================================
 * These functions translate ncurses input to platform-independent UiEvent
 * structures, enabling clean separation of input handling from ncurses.
 */

/* Translate ncurses key code to UiEvent
 * Returns true if the event was successfully translated.
 * For printable characters, key.is_special will be false and key.key
 * contains the character code. For special keys, key.is_special is true
 * and key.key contains a UiKeyCode value. */
bool render_translate_key(int ncurses_key, UiEvent *event);

/* Helper to check if event matches a specific key with optional modifiers */
bool render_event_is_key(const UiEvent *event, int key, UiKeyMod mods);

/* Helper to check if event is a printable character */
bool render_event_is_char(const UiEvent *event);

/* Helper to get character from event (returns 0 if not a character) */
int render_event_get_char(const UiEvent *event);

/* Helper to check for Ctrl+key combinations
 * letter should be uppercase (e.g., 'A' for Ctrl+A) */
bool render_event_is_ctrl(const UiEvent *event, char letter);

/* Helper to check for specific special key */
bool render_event_is_special(const UiEvent *event, UiKeyCode code);

/* Helper to check for function key (F1-F12)
 * Returns function key number (1-12) or 0 if not a function key */
int render_event_get_fkey(const UiEvent *event);

#endif /* LACE_TUI_RENDER_HELPERS_H */
