/*
 * Lace
 * TUI Render Helpers - Bridge between TUI and RenderBackend
 *
 * Provides high-level drawing functions that use RenderBackend internally,
 * allowing gradual migration from direct ncurses calls.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_RENDER_HELPERS_H
#define LACE_TUI_RENDER_HELPERS_H

#include "backend.h"
#include <stdbool.h>
#include <stddef.h>

/* Forward declaration */
struct TuiState;

/* ============================================================================
 * Initialization
 * ============================================================================
 */

/* Initialize render context (call during TUI init) */
RenderContext *render_init(void);

/* Shutdown render context (call during TUI cleanup) */
void render_shutdown(RenderContext *ctx);

/* ============================================================================
 * Frame Management
 * ============================================================================
 */

/* Begin a new frame (clears screen) */
void render_begin_frame(RenderContext *ctx);

/* End frame and flush to display */
void render_end_frame(RenderContext *ctx);

/* Get current screen dimensions */
void render_get_size(RenderContext *ctx, int *width, int *height);

/* ============================================================================
 * Color and Attribute Helpers
 * ============================================================================
 */

/* Set color using logical color enum */
void render_set_color(RenderContext *ctx, UiColor color);

/* Set color with additional attributes */
void render_set_color_attrs(RenderContext *ctx, UiColor color, UiAttr attrs);

/* Set bold attribute (adds to current) */
void render_set_bold(RenderContext *ctx, bool bold);

/* Set reverse video attribute */
void render_set_reverse(RenderContext *ctx, bool reverse);

/* Reset all attributes to default */
void render_reset(RenderContext *ctx);

/* ============================================================================
 * Text Drawing
 * ============================================================================
 */

/* Draw a character at position */
void render_char(RenderContext *ctx, int x, int y, int ch);

/* Draw a string at position */
void render_string(RenderContext *ctx, int x, int y, const char *str);

/* Draw a formatted string at position */
void render_printf(RenderContext *ctx, int x, int y, const char *fmt, ...);

/* Draw a string with fixed width (truncate or pad) */
void render_string_fixed(RenderContext *ctx, int x, int y, const char *str,
                         int width);

/* Draw a string with fixed width, left-aligned */
void render_string_left(RenderContext *ctx, int x, int y, const char *str,
                        int width);

/* Draw a string with fixed width, right-aligned */
void render_string_right(RenderContext *ctx, int x, int y, const char *str,
                         int width);

/* Draw a string with fixed width, centered */
void render_string_center(RenderContext *ctx, int x, int y, const char *str,
                          int width);

/* ============================================================================
 * Line Drawing
 * ============================================================================
 */

/* Draw horizontal line */
void render_hline(RenderContext *ctx, int x, int y, int width);

/* Draw vertical line */
void render_vline(RenderContext *ctx, int x, int y, int height);

/* Draw a box outline */
void render_box(RenderContext *ctx, int x, int y, int width, int height);

/* Fill a rectangle with spaces */
void render_fill(RenderContext *ctx, int x, int y, int width, int height);

/* Fill a rectangle with character */
void render_fill_char(RenderContext *ctx, int x, int y, int width, int height,
                      int ch);

/* ============================================================================
 * Region/Clipping Helpers
 * ============================================================================
 */

/* Defines a drawing region (for logical window replacement) */
typedef struct {
  int x, y;          /* Top-left corner */
  int width, height; /* Dimensions */
} RenderRegion;

/* Create a region */
RenderRegion render_region(int x, int y, int width, int height);

/* Draw character within region (clips to bounds) */
void render_region_char(RenderContext *ctx, RenderRegion *region, int x, int y,
                        int ch);

/* Draw string within region (clips to bounds) */
void render_region_string(RenderContext *ctx, RenderRegion *region, int x,
                          int y, const char *str);

/* Draw formatted string within region */
void render_region_printf(RenderContext *ctx, RenderRegion *region, int x,
                          int y, const char *fmt, ...);

/* Draw horizontal line within region */
void render_region_hline(RenderContext *ctx, RenderRegion *region, int x, int y,
                         int width);

/* Fill region with character */
void render_region_fill(RenderContext *ctx, RenderRegion *region, int ch);

/* Clear region (fill with spaces) */
void render_region_clear(RenderContext *ctx, RenderRegion *region);

/* Set background color for region */
void render_region_background(RenderContext *ctx, RenderRegion *region,
                              UiColor color);

/* ============================================================================
 * Box Drawing Characters
 * ============================================================================
 */

/* Box drawing character types */
#define RENDER_HLINE '-'
#define RENDER_VLINE '|'
#define RENDER_ULCORNER '+'
#define RENDER_URCORNER '+'
#define RENDER_LLCORNER '+'
#define RENDER_LRCORNER '+'
#define RENDER_LTEE '+'
#define RENDER_RTEE '+'
#define RENDER_TTEE '+'
#define RENDER_BTEE '+'
#define RENDER_PLUS '+'

/* Get ACS character for current backend (falls back to ASCII) */
int render_acs_hline(RenderContext *ctx);
int render_acs_vline(RenderContext *ctx);
int render_acs_ulcorner(RenderContext *ctx);
int render_acs_urcorner(RenderContext *ctx);
int render_acs_llcorner(RenderContext *ctx);
int render_acs_lrcorner(RenderContext *ctx);

/* ============================================================================
 * Cursor Control
 * ============================================================================
 */

/* Show or hide the cursor */
void render_cursor_visible(RenderContext *ctx, bool visible);

/* Set cursor position */
void render_cursor_move(RenderContext *ctx, int x, int y);

/* ============================================================================
 * Input Handling
 * ============================================================================
 */

/* Wait for input event (blocking) */
bool render_wait_event(RenderContext *ctx, UiEvent *event);

/* Poll for input event (non-blocking) */
bool render_poll_event(RenderContext *ctx, UiEvent *event);

/* Enable/disable mouse support */
void render_mouse_enable(RenderContext *ctx, bool enable);

/* ============================================================================
 * Input Translation Helpers
 * ============================================================================
 * These functions translate ncurses input to platform-independent UiEvent
 * structures, enabling gradual migration from direct ncurses input handling.
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
