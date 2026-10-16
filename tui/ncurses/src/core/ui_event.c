/*
 * Lace
 * UiEvent helpers (no ncurses)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "ui_event.h"

bool ui_event_is_char(const UiEvent *event) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  /* Printable ASCII (32-126), or one byte of a UTF-8 character: ncurses
   * delivers multi-byte characters a byte at a time (0x80-0xFF) */
  if (!event->key.is_special && event->key.mods == UI_MOD_NONE) {
    int ch = event->key.key;
    return (ch >= 32 && ch <= 126) || (ch >= 0x80 && ch <= 0xFF);
  }
  return false;
}

int ui_event_get_char(const UiEvent *event) {
  if (!event || event->type != UI_EVENT_KEY)
    return 0;

  if (!event->key.is_special && event->key.mods == UI_MOD_NONE) {
    return event->key.key;
  }
  return 0;
}

bool ui_event_is_ctrl(const UiEvent *event, char letter) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  /* Ctrl keys are stored as the uppercase letter with UI_MOD_CTRL */
  char upper = (letter >= 'a' && letter <= 'z') ? letter - 32 : letter;
  return event->key.key == upper && (event->key.mods & UI_MOD_CTRL);
}

bool ui_event_is_special(const UiEvent *event, UiKeyCode code) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  return event->key.is_special && event->key.key == (int)code;
}

int ui_event_get_fkey(const UiEvent *event) {
  if (!event || event->type != UI_EVENT_KEY || !event->key.is_special)
    return 0;

  switch (event->key.key) {
  case UI_KEY_F1:
    return 1;
  case UI_KEY_F2:
    return 2;
  case UI_KEY_F3:
    return 3;
  case UI_KEY_F4:
    return 4;
  case UI_KEY_F5:
    return 5;
  case UI_KEY_F6:
    return 6;
  case UI_KEY_F7:
    return 7;
  case UI_KEY_F8:
    return 8;
  case UI_KEY_F9:
    return 9;
  case UI_KEY_F10:
    return 10;
  case UI_KEY_F11:
    return 11;
  case UI_KEY_F12:
    return 12;
  default:
    return 0;
  }
}
