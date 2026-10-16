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

#include "core/ui_event.h"
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

/* The pure UiEvent helpers live in core/ui_event.c; the old names stay */
#define render_event_is_char ui_event_is_char
#define render_event_get_char ui_event_get_char
#define render_event_is_ctrl ui_event_is_ctrl
#define render_event_is_special ui_event_is_special
#define render_event_get_fkey ui_event_get_fkey

#endif /* LACE_TUI_RENDER_HELPERS_H */
