/*
 * Lace
 * TUI Input Helpers Implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "render_helpers.h"
#include <ncurses.h>
#include <string.h>

/* ============================================================================
 * Input Translation
 * ============================================================================
 */

bool render_translate_key(int ncurses_key, UiEvent *event) {
  if (!event)
    return false;

  memset(event, 0, sizeof(UiEvent));

  /* Handle timeout/error */
  if (ncurses_key == ERR) {
    event->type = UI_EVENT_NONE;
    return false;
  }

  /* Handle resize */
  if (ncurses_key == KEY_RESIZE) {
    event->type = UI_EVENT_RESIZE;
    return true;
  }

  /* Handle mouse */
  if (ncurses_key == KEY_MOUSE) {
    MEVENT mevent;
    if (getmouse(&mevent) == OK) {
      event->type = UI_EVENT_MOUSE;
      event->mouse.x = mevent.x;
      event->mouse.y = mevent.y;

      if (mevent.bstate & BUTTON1_CLICKED) {
        event->mouse.button = UI_MOUSE_LEFT;
        event->mouse.action = UI_MOUSE_CLICK;
      } else if (mevent.bstate & BUTTON1_DOUBLE_CLICKED) {
        event->mouse.button = UI_MOUSE_LEFT;
        event->mouse.action = UI_MOUSE_DOUBLE_CLICK;
      } else if (mevent.bstate & BUTTON4_PRESSED) {
        event->mouse.button = UI_MOUSE_SCROLL_UP;
        event->mouse.action = UI_MOUSE_PRESS;
      } else if (mevent.bstate & BUTTON5_PRESSED) {
        event->mouse.button = UI_MOUSE_SCROLL_DOWN;
        event->mouse.action = UI_MOUSE_PRESS;
      } else if (mevent.bstate & BUTTON3_CLICKED) {
        event->mouse.button = UI_MOUSE_RIGHT;
        event->mouse.action = UI_MOUSE_CLICK;
      }
      return true;
    }
    return false;
  }

  /* Key event */
  event->type = UI_EVENT_KEY;
  event->key.mods = UI_MOD_NONE;
  event->key.is_special = false;

  /* Handle special keys */
  switch (ncurses_key) {
  /* Navigation */
  case KEY_UP:
    event->key.key = UI_KEY_UP;
    event->key.is_special = true;
    break;
  case KEY_DOWN:
    event->key.key = UI_KEY_DOWN;
    event->key.is_special = true;
    break;
  case KEY_LEFT:
    event->key.key = UI_KEY_LEFT;
    event->key.is_special = true;
    break;
  case KEY_RIGHT:
    event->key.key = UI_KEY_RIGHT;
    event->key.is_special = true;
    break;
  case KEY_HOME:
    event->key.key = UI_KEY_HOME;
    event->key.is_special = true;
    break;
  case KEY_END:
    event->key.key = UI_KEY_END;
    event->key.is_special = true;
    break;
  case KEY_PPAGE:
    event->key.key = UI_KEY_PAGEUP;
    event->key.is_special = true;
    break;
  case KEY_NPAGE:
    event->key.key = UI_KEY_PAGEDOWN;
    event->key.is_special = true;
    break;

  /* Editing keys */
  case KEY_BACKSPACE:
  case 127: /* DEL character */
    event->key.key = UI_KEY_BACKSPACE;
    event->key.is_special = true;
    break;
  case KEY_DC:
    event->key.key = UI_KEY_DELETE;
    event->key.is_special = true;
    break;
  case KEY_IC:
    event->key.key = UI_KEY_INSERT;
    event->key.is_special = true;
    break;
  case KEY_ENTER:
  case '\n':
  case '\r':
    event->key.key = UI_KEY_ENTER;
    event->key.is_special = true;
    break;
  case '\t':
    event->key.key = UI_KEY_TAB;
    event->key.is_special = true;
    break;
  case 27: /* ESC */
    event->key.key = UI_KEY_ESCAPE;
    event->key.is_special = true;
    break;

  /* Function keys */
  case KEY_F(1):
    event->key.key = UI_KEY_F1;
    event->key.is_special = true;
    break;
  case KEY_F(2):
    event->key.key = UI_KEY_F2;
    event->key.is_special = true;
    break;
  case KEY_F(3):
    event->key.key = UI_KEY_F3;
    event->key.is_special = true;
    break;
  case KEY_F(4):
    event->key.key = UI_KEY_F4;
    event->key.is_special = true;
    break;
  case KEY_F(5):
    event->key.key = UI_KEY_F5;
    event->key.is_special = true;
    break;
  case KEY_F(6):
    event->key.key = UI_KEY_F6;
    event->key.is_special = true;
    break;
  case KEY_F(7):
    event->key.key = UI_KEY_F7;
    event->key.is_special = true;
    break;
  case KEY_F(8):
    event->key.key = UI_KEY_F8;
    event->key.is_special = true;
    break;
  case KEY_F(9):
    event->key.key = UI_KEY_F9;
    event->key.is_special = true;
    break;
  case KEY_F(10):
    event->key.key = UI_KEY_F10;
    event->key.is_special = true;
    break;
  case KEY_F(11):
    event->key.key = UI_KEY_F11;
    event->key.is_special = true;
    break;
  case KEY_F(12):
    event->key.key = UI_KEY_F12;
    event->key.is_special = true;
    break;

  default:
    /* Control characters (Ctrl+A through Ctrl+Z are 1-26) */
    if (ncurses_key >= 1 && ncurses_key <= 26) {
      event->key.key = 'A' + ncurses_key - 1;
      event->key.mods = UI_MOD_CTRL;
      event->key.is_special = false;
    } else {
      /* Regular character */
      event->key.key = ncurses_key;
      event->key.is_special = false;
    }
    break;
  }

  return true;
}

bool render_event_is_key(const UiEvent *event, int key, UiKeyMod mods) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  return event->key.key == key && event->key.mods == mods;
}

bool render_event_is_char(const UiEvent *event) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  /* Printable ASCII characters (32-126) */
  if (!event->key.is_special && event->key.mods == UI_MOD_NONE) {
    int ch = event->key.key;
    return ch >= 32 && ch <= 126;
  }
  return false;
}

int render_event_get_char(const UiEvent *event) {
  if (!event || event->type != UI_EVENT_KEY)
    return 0;

  if (!event->key.is_special && event->key.mods == UI_MOD_NONE) {
    return event->key.key;
  }
  return 0;
}

bool render_event_is_ctrl(const UiEvent *event, char letter) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  /* Ctrl keys are stored as the uppercase letter with UI_MOD_CTRL */
  char upper = (letter >= 'a' && letter <= 'z') ? letter - 32 : letter;
  return event->key.key == upper && (event->key.mods & UI_MOD_CTRL);
}

bool render_event_is_special(const UiEvent *event, UiKeyCode code) {
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  return event->key.is_special && event->key.key == (int)code;
}

int render_event_get_fkey(const UiEvent *event) {
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
