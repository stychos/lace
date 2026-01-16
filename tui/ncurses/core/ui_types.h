/*
 * Lace
 * Platform-Independent UI Types
 *
 * These types define the abstract interface for UI events, colors, and input.
 * They are used by both the core application (hotkeys, config) and UI backends.
 * This header has NO platform-specific dependencies.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_UI_TYPES_H
#define LACE_UI_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Color Definitions
 * ============================================================================
 */

/* Logical colors (mapped to actual colors by backend) */
typedef enum {
  UI_COLOR_DEFAULT = 0,
  UI_COLOR_HEADER,
  UI_COLOR_SELECTED,
  UI_COLOR_STATUS,
  UI_COLOR_ERROR,
  UI_COLOR_BORDER,
  UI_COLOR_TITLE,
  UI_COLOR_NULL,
  UI_COLOR_NUMBER,
  UI_COLOR_EDIT,
  UI_COLOR_ERROR_TEXT, /* Error message text (distinct from error background) */
  UI_COLOR_PK,         /* Primary key column indicator */
  UI_COLOR_COUNT
} UiColor;

/* Text attributes */
typedef enum {
  UI_ATTR_NORMAL = 0,
  UI_ATTR_BOLD = (1 << 0),
  UI_ATTR_UNDERLINE = (1 << 1),
  UI_ATTR_REVERSE = (1 << 2),
  UI_ATTR_DIM = (1 << 3),
} UiAttr;

/* Line drawing characters (abstract, mapped by backend) */
typedef enum {
  UI_LINE_HLINE = 0,  /* Horizontal line ─ */
  UI_LINE_VLINE,      /* Vertical line │ */
  UI_LINE_ULCORNER,   /* Upper-left corner ┌ */
  UI_LINE_URCORNER,   /* Upper-right corner ┐ */
  UI_LINE_LLCORNER,   /* Lower-left corner └ */
  UI_LINE_LRCORNER,   /* Lower-right corner ┘ */
  UI_LINE_LTEE,       /* Left tee ├ */
  UI_LINE_RTEE,       /* Right tee ┤ */
  UI_LINE_TTEE,       /* Top tee ┬ */
  UI_LINE_BTEE,       /* Bottom tee ┴ */
  UI_LINE_PLUS,       /* Plus/cross ┼ */
  UI_LINE_COUNT
} UiLineChar;

/* ============================================================================
 * Input Events
 * ============================================================================
 */

/* Input event types */
typedef enum {
  UI_EVENT_NONE = 0,
  UI_EVENT_KEY,
  UI_EVENT_MOUSE,
  UI_EVENT_RESIZE,
  UI_EVENT_QUIT,
} UiEventType;

/* Special key codes (normalized across backends) */
typedef enum {
  UI_KEY_NONE = 0,

  /* Navigation */
  UI_KEY_UP = 256,
  UI_KEY_DOWN,
  UI_KEY_LEFT,
  UI_KEY_RIGHT,
  UI_KEY_HOME,
  UI_KEY_END,
  UI_KEY_PAGEUP,
  UI_KEY_PAGEDOWN,

  /* Editing */
  UI_KEY_BACKSPACE,
  UI_KEY_DELETE,
  UI_KEY_INSERT,
  UI_KEY_ENTER,
  UI_KEY_TAB,
  UI_KEY_ESCAPE,

  /* Function keys */
  UI_KEY_F1,
  UI_KEY_F2,
  UI_KEY_F3,
  UI_KEY_F4,
  UI_KEY_F5,
  UI_KEY_F6,
  UI_KEY_F7,
  UI_KEY_F8,
  UI_KEY_F9,
  UI_KEY_F10,
  UI_KEY_F11,
  UI_KEY_F12,
} UiKeyCode;

/* Key modifiers */
typedef enum {
  UI_MOD_NONE = 0,
  UI_MOD_CTRL = (1 << 0),
  UI_MOD_ALT = (1 << 1),
  UI_MOD_SHIFT = (1 << 2),
} UiKeyMod;

/* Mouse button */
typedef enum {
  UI_MOUSE_NONE = 0,
  UI_MOUSE_LEFT,
  UI_MOUSE_MIDDLE,
  UI_MOUSE_RIGHT,
  UI_MOUSE_SCROLL_UP,
  UI_MOUSE_SCROLL_DOWN,
} UiMouseButton;

/* Mouse action */
typedef enum {
  UI_MOUSE_PRESS = 0,
  UI_MOUSE_RELEASE,
  UI_MOUSE_CLICK,
  UI_MOUSE_DOUBLE_CLICK,
  UI_MOUSE_DRAG,
} UiMouseAction;

/* Input event structure */
typedef struct UiEvent {
  UiEventType type;

  union {
    /* Key event */
    struct {
      int key;         /* Character code or UiKeyCode */
      UiKeyMod mods;   /* Modifier keys */
      bool is_special; /* true if key is UiKeyCode, false for character */
    } key;

    /* Mouse event */
    struct {
      int x, y;
      UiMouseButton button;
      UiMouseAction action;
      UiKeyMod mods;
    } mouse;

    /* Resize event */
    struct {
      int width, height;
    } resize;
  };
} UiEvent;

#endif /* LACE_UI_TYPES_H */
