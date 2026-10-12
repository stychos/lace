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
#include <menu.h>
#include <stdlib.h>
#include <string.h>

int tui_pick_from_list(const Config *cfg, const char *title,
                       const char *const *labels, size_t n, size_t current,
                       int y, int x, int min_width, int max_width,
                       int max_height) {
  if (n == 0)
    return -1;

  /* Size to the labels: 2 for the "> " mark, 2 for the borders */
  int widest = utf8_width(title, strlen(title)) + 2;
  for (size_t i = 0; i < n; i++) {
    int w = utf8_width(labels[i], strlen(labels[i]));
    if (w > widest)
      widest = w;
  }
  int width = widest + 6;
  if (width < min_width)
    width = min_width;
  if (width > max_width)
    width = max_width;
  int height = (int)n + 2;
  if (height > max_height)
    height = max_height;

  int screen_h, screen_w;
  getmaxyx(stdscr, screen_h, screen_w);
  if (height > screen_h)
    height = screen_h;
  if (width > screen_w)
    width = screen_w;
  if (height < 3 || width < 6)
    return -1;
  if (y + height > screen_h)
    y = screen_h - height;
  if (x + width > screen_w)
    x = screen_w - width;
  if (y < 0)
    y = 0;
  if (x < 0)
    x = 0;

  WINDOW *win = newwin(height, width, y, x);
  if (!win)
    return -1;
  keypad(win, TRUE);
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
  DRAW_BOX(win, COLOR_BORDER);
  WITH_ATTR(win, A_BOLD, mvwprintw(win, 0, 2, " %s ", title));

  ITEM **items = safe_calloc(n + 1, sizeof(ITEM *));
  for (size_t i = 0; i < n; i++)
    items[i] = new_item(labels[i], "");
  MENU *menu = new_menu(items);
  if (!menu) {
    for (size_t i = 0; i < n; i++)
      free_item(items[i]);
    free(items);
    delwin(win);
    return -1;
  }
  WINDOW *sub = menu_setup(menu, win, height, width, 1);
  if (current < n)
    set_current_item(menu, items[current]);
  wrefresh(win);

  int result = -1;
  bool running = true;
  while (running) {
    int ch = wgetch(win);
    if (ch == KEY_MOUSE) {
      MEVENT m;
      if (getmouse(&m) != OK)
        continue;
      if (!wenclose(win, m.y, m.x)) {
        running = false; /* A click outside closes */
      } else if (m.bstate & BUTTON4_PRESSED) {
        menu_driver(menu, REQ_UP_ITEM);
      } else if (m.bstate & BUTTON5_PRESSED) {
        menu_driver(menu, REQ_DOWN_ITEM);
      } else if (m.bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)) {
        int row = m.y - y - 1; /* Below the top border */
        int target = top_row(menu) + row;
        if (row >= 0 && row < height - 2 && target < (int)n) {
          result = target; /* A click picks */
          running = false;
        }
      }
    } else {
      UiEvent event;
      render_translate_key(ch, &event);
      if (render_event_is_special(&event, UI_KEY_ENTER)) {
        ITEM *cur = current_item(menu);
        if (cur)
          result = item_index(cur);
        running = false;
      } else if (render_event_is_special(&event, UI_KEY_ESCAPE) ||
                 render_event_get_char(&event) == 'q') {
        running = false;
      } else if (hotkey_matches(cfg, &event, HOTKEY_MOVE_DOWN)) {
        menu_driver(menu, REQ_DOWN_ITEM);
      } else if (hotkey_matches(cfg, &event, HOTKEY_MOVE_UP)) {
        menu_driver(menu, REQ_UP_ITEM);
      } else if (hotkey_matches(cfg, &event, HOTKEY_PAGE_DOWN)) {
        menu_driver(menu, REQ_SCR_DPAGE);
      } else if (hotkey_matches(cfg, &event, HOTKEY_PAGE_UP)) {
        menu_driver(menu, REQ_SCR_UPAGE);
      } else if (hotkey_matches(cfg, &event, HOTKEY_FIRST_ROW)) {
        menu_driver(menu, REQ_FIRST_ITEM);
      } else if (hotkey_matches(cfg, &event, HOTKEY_LAST_ROW)) {
        menu_driver(menu, REQ_LAST_ITEM);
      }
    }
    wrefresh(win);
  }

  unpost_menu(menu);
  free_menu(menu);
  for (size_t i = 0; i < n; i++)
    free_item(items[i]);
  free(items);
  delwin(sub);
  delwin(win);
  touchwin(stdscr);
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
