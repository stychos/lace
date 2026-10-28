/*
 * Lace
 * Overlays: the notcurses TUI's dialog frame for the ncurses dialogs. A
 * frame of rounded corners in the theme's overlay colours over a shadow,
 * the title in its top edge, the key hints in its bottom edge and on the
 * screen's status row; content of rows x cols inside it. Layout as
 * notcurses' (ui/overlay.c): at most the screen's rows - 5 by cols - 4,
 * centred unless a corner is given.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_OVERLAY_H
#define LACE_TUI_OVERLAY_H

#include "theme.h"
#include <ncurses.h>
#include <stdbool.h>

typedef struct {
  WINDOW *win;    /* the frame: content at (1, 1) */
  WINDOW *shadow; /* one row lower, two columns right */
  WINDOW *status; /* the screen's last row, showing the footer */
  int rows, cols; /* content size, the frame excluded */
  int y, x;       /* the frame's screen origin */
  char *title;
  char *footer;   /* key hints */
} TuiOverlay;

/* Make the windows: content rows x cols, frame origin (y, x) or centred
 * for -1. False when the screen has no room for even one row of it. */
bool tui_overlay_open(TuiOverlay *o, const char *title, const char *footer, int rows, int cols, int y, int x);
/* The shadow, the background, the frame, the title and the footer (also on
 * the status row), then the content colours set for drawing */
void tui_overlay_frame(TuiOverlay *o);

/* The overlay frame on a dialog's own window (erased): the rounded border,
 * the title in the top edge, the keys in the bottom edge, the overlay's
 * colours; for the dialogs that keep their layout inside */
void tui_frame_window(WINDOW *win, const char *title, const char *footer);
/* The same on a rectangle of a window (its inside cleared) */
void tui_frame_rect(WINDOW *win, int y, int x, int h, int w, const char *title, const char *footer);
/* Change the hints (redrawn by the next tui_overlay_frame) */
void tui_overlay_set_footer(TuiOverlay *o, const char *footer);
/* Text at content (row, col), clipped and padded to width, in fg on the
 * overlay's background with attrs */
void tui_overlay_put(TuiOverlay *o, int row, int col, int width, ThemeColor fg, attr_t attrs, const char *text);
/* Everything shown: the shadow, the frame and the status row */
void tui_overlay_refresh(TuiOverlay *o);
/* The windows go; the screen behind is marked for a full redraw */
void tui_overlay_close(TuiOverlay *o);
/* A screen (y, x) inside the frame */
bool tui_overlay_contains(const TuiOverlay *o, int y, int x);

#endif /* LACE_TUI_OVERLAY_H */
