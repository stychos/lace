/*
 * Lace
 * Pick-one list popup
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "list_picker.h"
#include "../render_helpers.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/mem.h"
#include "list_draw.h"
#include "overlay.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* As the notcurses TUI's pick list: an overlay of the labels (the shared
 * list: arrows and the navigation keys move, Enter or a click picks), its
 * footer "Enter pick  Esc cancel"; placed at (y, x), or centered when y is
 * negative */
int tui_pick_from_list(const Config *cfg, const char *title, const char *const *labels, size_t n, size_t current,
                       int y, int x, int min_width, int max_width, int max_height) {
  (void)cfg; /* the list takes the navigation keys (list_use_hotkeys) */
  if (n == 0)
    return -1;
  int widest = utf8_width(title, strlen(title)) + 4;
  for (size_t i = 0; i < n; i++) {
    int w = utf8_width(labels[i], strlen(labels[i])) + 2;
    if (w > widest)
      widest = w;
  }
  int cols = widest;
  if (cols < min_width - 2)
    cols = min_width - 2;
  if (cols > max_width - 2)
    cols = max_width - 2;
  int rows = (int)n < max_height - 2 ? (int)n : max_height - 2;
  if (rows < 1)
    rows = 1;
  ListRow *lr = safe_calloc(n, sizeof *lr);
  for (size_t i = 0; i < n; i++)
    lr[i] = (ListRow){str_dup(labels[i]), NULL, NULL, 0, (void *)(intptr_t)i, false};
  ListWidget list;
  list_init(&list, false);
  list_set_rows(&list, lr, n);
  if (current < n)
    list.cursor = current;
  TuiOverlay o;
  int result = -1;
  if (tui_overlay_open(&o, title, "Enter pick  Esc cancel", rows, cols, y, x)) {
    ListRow *r = tui_list_overlay_run(&o, &list, NULL, NULL);
    if (r)
      result = (int)(intptr_t)r->data;
    tui_overlay_close(&o);
  }
  list_free(&list);
  return result;
}

bool tui_list_nav_key(const Config *cfg, const UiEvent *event, size_t count,
                      size_t visible, size_t *selected, size_t *scroll) {
  size_t page = visible > 1 ? visible - 1 : 1;
  size_t sel = *selected;
  if (hotkey_matches(cfg, event, HOTKEY_MOVE_UP)) {
    sel = sel > 0 ? sel - 1 : 0;
  } else if (hotkey_matches(cfg, event, HOTKEY_MOVE_DOWN)) {
    sel = sel + 1 < count ? sel + 1 : sel;
  } else if (hotkey_matches(cfg, event, HOTKEY_PAGE_UP)) {
    sel = sel > page ? sel - page : 0;
  } else if (hotkey_matches(cfg, event, HOTKEY_PAGE_DOWN)) {
    sel = sel + page < count ? sel + page : (count > 0 ? count - 1 : 0);
  } else if (hotkey_matches(cfg, event, HOTKEY_FIRST_ROW)) {
    sel = 0;
  } else if (hotkey_matches(cfg, event, HOTKEY_LAST_ROW)) {
    sel = count > 0 ? count - 1 : 0;
  } else {
    return false;
  }
  *selected = sel;
  if (sel < *scroll)
    *scroll = sel;
  else if (visible > 0 && sel >= *scroll + visible)
    *scroll = sel - visible + 1;
  return true;
}
