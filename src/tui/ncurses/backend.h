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

/* Import platform-independent UI types from core layer */
#include "../../core/ui_types.h"

/* ============================================================================
 * Render Context (opaque, backend-specific)
 * ============================================================================
 */

typedef struct RenderContext RenderContext;

/* ============================================================================
 * Render Regions
 *
 * Regions provide a platform-independent way to manage layout areas.
 * Each region has a position, size, and can be drawn to independently.
 * The backend implementation maps regions to platform-specific windows/areas.
 * ============================================================================
 */

/* Logical region identifiers */
typedef enum {
  UI_REGION_MAIN = 0,   /* Main content area (table/query results) */
  UI_REGION_HEADER,     /* Column headers / title bar */
  UI_REGION_STATUS,     /* Status bar at bottom */
  UI_REGION_SIDEBAR,    /* Table list sidebar */
  UI_REGION_TABS,       /* Tab bar */
  UI_REGION_DIALOG,     /* Modal dialog overlay */
  UI_REGION_COUNT
} UiRegionId;

/* Region bounds (for layout calculations) */
typedef struct {
  int x, y;            /* Top-left position */
  int width, height;   /* Dimensions */
} UiRegionBounds;

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

  /* -------------------------------------------------------------------------
   * Line Drawing Characters
   * -------------------------------------------------------------------------
   */

  /* Get platform-specific character for line drawing */
  int (*get_line_char)(RenderContext *ctx, UiLineChar ch);

  /* -------------------------------------------------------------------------
   * Region Management
   * -------------------------------------------------------------------------
   */

  /* Create or resize a region */
  bool (*set_region)(RenderContext *ctx, UiRegionId id, int x, int y, int width,
                     int height);

  /* Get region bounds */
  bool (*get_region)(RenderContext *ctx, UiRegionId id, UiRegionBounds *bounds);

  /* Begin drawing to a region (sets clipping, positions cursor) */
  void (*begin_region)(RenderContext *ctx, UiRegionId id);

  /* End drawing to current region */
  void (*end_region)(RenderContext *ctx);

  /* Clear a region */
  void (*clear_region)(RenderContext *ctx, UiRegionId id);

  /* Refresh/update a specific region */
  void (*refresh_region)(RenderContext *ctx, UiRegionId id);

  /* -------------------------------------------------------------------------
   * Native Handle Access (for gradual migration)
   * -------------------------------------------------------------------------
   */

  /* Get native window handle for a region (ncurses: WINDOW*, others: void*).
   * This allows gradual migration - code can get the native handle and use
   * platform-specific calls while migrating to render_* functions.
   * Returns NULL if region not set up. */
  void *(*get_region_handle)(RenderContext *ctx, UiRegionId id);

  /* Set native handle for a region (for integrating existing windows).
   * This allows TUI code to register its existing windows with the backend. */
  void (*set_region_handle)(RenderContext *ctx, UiRegionId id, void *handle);

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

/* Create a render context that wraps an existing ncurses session.
 * Use this when ncurses has already been initialized (e.g., by legacy code).
 * The context will use the existing ncurses setup without calling initscr(). */
RenderContext *render_context_wrap_ncurses(void);

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
