/*
 * Lace
 * UI Render Backend Abstraction
 *
 * Provides a platform-independent interface for rendering UI elements.
 * Implementations: ncurses (TUI), and future GUI backends (GTK, Qt, etc.)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_RENDER_BACKEND_H
#define LACE_RENDER_BACKEND_H

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
struct UiEvent {
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
};
typedef struct UiEvent UiEvent;

/* ============================================================================
 * Render Context (opaque, backend-specific)
 * ============================================================================
 */

typedef struct RenderContext RenderContext;

/* ============================================================================
 * Render Backend Interface
 *
 * All rendering operations go through this interface. Each backend (ncurses,
 * GTK, Qt, etc.) provides its own implementation.
 * ============================================================================
 */

typedef struct RenderBackend {
  /* Backend name (for debugging) */
  const char *name;

  /* -------------------------------------------------------------------------
   * Lifecycle
   * -------------------------------------------------------------------------
   */

  /* Initialize the backend (returns context, NULL on failure) */
  RenderContext *(*init)(void);

  /* Shutdown and cleanup */
  void (*shutdown)(RenderContext *ctx);

  /* -------------------------------------------------------------------------
   * Display Management
   * -------------------------------------------------------------------------
   */

  /* Get terminal/window dimensions */
  void (*get_size)(RenderContext *ctx, int *width, int *height);

  /* Begin frame (clear, prepare for drawing) */
  void (*begin_frame)(RenderContext *ctx);

  /* End frame (flush to display) */
  void (*end_frame)(RenderContext *ctx);

  /* Handle terminal resize */
  void (*handle_resize)(RenderContext *ctx);

  /* -------------------------------------------------------------------------
   * Drawing Primitives
   * -------------------------------------------------------------------------
   */

  /* Set current color and attributes */
  void (*set_color)(RenderContext *ctx, UiColor color, UiAttr attrs);

  /* Reset to default color/attributes */
  void (*reset_attrs)(RenderContext *ctx);

  /* Move cursor to position */
  void (*move_to)(RenderContext *ctx, int x, int y);

  /* Draw a single character */
  void (*draw_char)(RenderContext *ctx, int x, int y, int ch);

  /* Draw a string (UTF-8) */
  void (*draw_string)(RenderContext *ctx, int x, int y, const char *str);

  /* Draw a string with max width (truncates or pads) */
  void (*draw_string_n)(RenderContext *ctx, int x, int y, const char *str,
                        int max_width);

  /* Draw horizontal line */
  void (*draw_hline)(RenderContext *ctx, int x, int y, int width, int ch);

  /* Draw vertical line */
  void (*draw_vline)(RenderContext *ctx, int x, int y, int height, int ch);

  /* Draw box (border rectangle) */
  void (*draw_box)(RenderContext *ctx, int x, int y, int width, int height);

  /* Fill rectangle with character */
  void (*fill_rect)(RenderContext *ctx, int x, int y, int width, int height,
                    int ch);

  /* Clear a rectangular region */
  void (*clear_rect)(RenderContext *ctx, int x, int y, int width, int height);

  /* -------------------------------------------------------------------------
   * Input Handling
   * -------------------------------------------------------------------------
   */

  /* Poll for input event (non-blocking, returns false if no event) */
  bool (*poll_event)(RenderContext *ctx, UiEvent *event);

  /* Wait for input event (blocking with timeout in ms, -1 = forever) */
  bool (*wait_event)(RenderContext *ctx, UiEvent *event, int timeout_ms);

  /* Enable/disable mouse support */
  void (*set_mouse_enabled)(RenderContext *ctx, bool enabled);

  /* -------------------------------------------------------------------------
   * Cursor
   * -------------------------------------------------------------------------
   */

  /* Show/hide cursor */
  void (*set_cursor_visible)(RenderContext *ctx, bool visible);

  /* Set cursor position */
  void (*set_cursor_pos)(RenderContext *ctx, int x, int y);

} RenderBackend;

/* ============================================================================
 * Backend Registration and Access
 * ============================================================================
 */

/* Get the ncurses TUI backend */
const RenderBackend *render_backend_ncurses(void);

/* Get the current/active backend */
const RenderBackend *render_backend_current(void);

/* Set the active backend */
void render_backend_set(const RenderBackend *backend);

/* ============================================================================
 * Convenience Functions (use current backend)
 * ============================================================================
 */

/* These use the currently active backend for simpler calling convention */

static inline RenderContext *ui_init(void) {
  const RenderBackend *b = render_backend_current();
  return b ? b->init() : NULL;
}

static inline void ui_shutdown(RenderContext *ctx) {
  const RenderBackend *b = render_backend_current();
  if (b)
    b->shutdown(ctx);
}

static inline void ui_get_size(RenderContext *ctx, int *w, int *h) {
  const RenderBackend *b = render_backend_current();
  if (b)
    b->get_size(ctx, w, h);
}

static inline void ui_begin_frame(RenderContext *ctx) {
  const RenderBackend *b = render_backend_current();
  if (b)
    b->begin_frame(ctx);
}

static inline void ui_end_frame(RenderContext *ctx) {
  const RenderBackend *b = render_backend_current();
  if (b)
    b->end_frame(ctx);
}

#endif /* LACE_RENDER_BACKEND_H */
