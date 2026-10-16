/*
 * Lace
 * UiEvent helpers (no ncurses)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_UI_EVENT_H
#define LACE_UI_EVENT_H

#include "ui_types.h"
#include <stdbool.h>

/* Printable character (ASCII, or one byte of a UTF-8 character) */
bool ui_event_is_char(const UiEvent *event);

/* The character of the event (0 if not a character) */
int ui_event_get_char(const UiEvent *event);

/* Ctrl+letter; letter is case-insensitive */
bool ui_event_is_ctrl(const UiEvent *event, char letter);

/* A specific special key */
bool ui_event_is_special(const UiEvent *event, UiKeyCode code);

/* Function key number 1-12, or 0 */
int ui_event_get_fkey(const UiEvent *event);

#endif /* LACE_UI_EVENT_H */
