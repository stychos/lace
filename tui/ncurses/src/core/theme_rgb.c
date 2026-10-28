/*
 * Lace
 * The TUIs' colour theme: see theme_rgb.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "theme_rgb.h"
#include <stdlib.h>

static const uint32_t DARK[TC_COUNT] = {
    [TC_BG] = 0x1e1e2e,          [TC_FG] = 0xcdd6f4,         [TC_DIM] = 0x7f849c,
    [TC_HEADER_BG] = 0x313244,   [TC_HEADER_FG] = 0xcdd6f4,  [TC_STATUS_BG] = 0x313244,
    [TC_STATUS_FG] = 0xbac2de,   [TC_CURSOR_BG] = 0x45475a,  [TC_CURSOR_FG] = 0xffffff,
    [TC_ROW_BG] = 0x1e1e2e,      [TC_STRIPE_BG] = 0x24243a,  [TC_SELECTED_FG] = 0xf5c2e7,
    [TC_NULL_FG] = 0x6c7086,     [TC_NUMBER_FG] = 0x89b4fa,  [TC_PK_FG] = 0xf9e2af,
    [TC_ERROR_FG] = 0xf38ba8,    [TC_ACCENT] = 0x89dceb,     [TC_OVERLAY_BG] = 0x181825,
    [TC_OVERLAY_FG] = 0xcdd6f4,  [TC_OVERLAY_BORDER] = 0x585b70, [TC_SHADOW_BG] = 0x11111b,
    [TC_KEY_FG] = 0xa6e3a1,      [TC_EDIT_BG] = 0x3b3b5a,
};

static const uint32_t LIGHT[TC_COUNT] = {
    [TC_BG] = 0xeff1f5,          [TC_FG] = 0x4c4f69,         [TC_DIM] = 0x7c7f93,
    [TC_HEADER_BG] = 0xccd0da,   [TC_HEADER_FG] = 0x4c4f69,  [TC_STATUS_BG] = 0xccd0da,
    [TC_STATUS_FG] = 0x5c5f77,   [TC_CURSOR_BG] = 0xbcc0cc,  [TC_CURSOR_FG] = 0x000000,
    [TC_ROW_BG] = 0xeff1f5,      [TC_STRIPE_BG] = 0xe6e9ef,  [TC_SELECTED_FG] = 0x8839ef,
    [TC_NULL_FG] = 0x9ca0b0,     [TC_NUMBER_FG] = 0x1e66f5,  [TC_PK_FG] = 0xdf8e1d,
    [TC_ERROR_FG] = 0xd20f39,    [TC_ACCENT] = 0x04a5e5,     [TC_OVERLAY_BG] = 0xe6e9ef,
    [TC_OVERLAY_FG] = 0x4c4f69,  [TC_OVERLAY_BORDER] = 0x9ca0b0, [TC_SHADOW_BG] = 0xdce0e8,
    [TC_KEY_FG] = 0x40a02b,      [TC_EDIT_BG] = 0xdcdff0,
};

const uint32_t *theme_palette(bool dark) { return dark ? DARK : LIGHT; }

bool theme_is_dark(int color_scheme, bool terminal_dark) {
  return color_scheme == 2 || (color_scheme != 1 && terminal_dark);
}

static int dist2(int r1, int g1, int b1, int r2, int g2, int b2) {
  int dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
  return dr * dr + dg * dg + db * db;
}

int theme_xterm256(uint32_t rgb) {
  int r = (int)(rgb >> 16 & 0xff), g = (int)(rgb >> 8 & 0xff), b = (int)(rgb & 0xff);
  static const int level[6] = {0, 95, 135, 175, 215, 255};
  /* the nearest cube level per channel */
  int ci[3], c[3] = {r, g, b};
  for (int k = 0; k < 3; k++) {
    int best = 0;
    for (int i = 1; i < 6; i++)
      if (abs(c[k] - level[i]) < abs(c[k] - level[best]))
        best = i;
    ci[k] = best;
  }
  int cube = 16 + 36 * ci[0] + 6 * ci[1] + ci[2];
  int cube_d = dist2(r, g, b, level[ci[0]], level[ci[1]], level[ci[2]]);
  /* the nearest grey: 8, 18, ..., 238 */
  int avg = (r + g + b) / 3;
  int gi = avg < 8 ? 0 : avg > 238 ? 23 : (avg - 8 + 5) / 10;
  int grey = 8 + 10 * gi;
  int grey_d = dist2(r, g, b, grey, grey, grey);
  return grey_d < cube_d ? 232 + gi : cube;
}

int theme_ansi8(uint32_t rgb) {
  int r = (int)(rgb >> 16 & 0xff), g = (int)(rgb >> 8 & 0xff), b = (int)(rgb & 0xff);
  static const int ansi[8][3] = {{0, 0, 0},     {205, 0, 0},   {0, 205, 0},   {205, 205, 0},
                                 {0, 0, 238},   {205, 0, 205}, {0, 205, 205}, {229, 229, 229}};
  int best = 0, best_d = -1;
  for (int i = 0; i < 8; i++) {
    int d = dist2(r, g, b, ansi[i][0], ansi[i][1], ansi[i][2]);
    if (best_d < 0 || d < best_d) {
      best = i;
      best_d = d;
    }
  }
  return best;
}
