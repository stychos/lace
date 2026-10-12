/*
 * Lace
 * One-line prompt dialog
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "prompt.h"
#include "text_input.h"
#include "../render_helpers.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/str.h"
#include <stdlib.h>
#include <string.h>

/* Focus stops, in screen order */
enum { PROMPT_INPUT, PROMPT_OK, PROMPT_CANCEL, PROMPT_STOPS };

char *tui_prompt(const PromptOptions *opts) {
  int screen_h, screen_w;
  getmaxyx(stdscr, screen_h, screen_w);
  if (screen_h < 9 || screen_w < 24)
    return NULL;

  bool has_error = opts->error_msg && opts->error_msg[0];
  int height = has_error ? 9 : 8;
  int width = screen_w - 4 < 55 ? screen_w - 4 : 55;
  int y = (screen_h - height) / 2;
  int x = (screen_w - width) / 2;
  WINDOW *dlg = newwin(height, width, y > 0 ? y : 0, x > 0 ? x : 0);
  if (!dlg)
    return NULL;
  keypad(dlg, TRUE);

  TextInput input;
  text_input_init(&input, opts->initial, opts->secret);
  int focus = PROMPT_INPUT;
  int sep_y = has_error ? 5 : 4;
  int btn_y = sep_y + 2;
  char *result = NULL;
  bool running = true;

  while (running) {
    werase(dlg);
    DRAW_BOX(dlg, COLOR_BORDER);
    int title_w = utf8_width(opts->title, strlen(opts->title)) + 2;
    WITH_ATTR(dlg, A_BOLD,
              mvwprintw(dlg, 0, title_w < width ? (width - title_w) / 2 : 1,
                        " %s ", opts->title));
    tui_print_clip(dlg, 2, 2, width - 4, opts->label);

    attr_t field = focus == PROMPT_INPUT ? COLOR_PAIR(COLOR_SELECTED) : A_NORMAL;
    int cursor_x = text_input_draw(dlg, &input, 3, 2, width - 4, field,
                                   focus == PROMPT_INPUT);

    if (has_error) {
      WITH_ATTR(dlg, COLOR_PAIR(COLOR_ERROR_TEXT),
                tui_print_clip(dlg, 4, 2, width - 4, opts->error_msg));
    }

    wattron(dlg, A_DIM);
    mvwaddch(dlg, sep_y, 0, ACS_LTEE);
    mvwhline(dlg, sep_y, 1, ACS_HLINE, width - 2);
    mvwaddch(dlg, sep_y, width - 1, ACS_RTEE);
    wattroff(dlg, A_DIM);

    int btn_x = width / 2 - 10;
    WITH_ATTR(dlg, focus == PROMPT_OK ? A_REVERSE : A_NORMAL,
              mvwprintw(dlg, btn_y, btn_x, "[ OK ]"));
    WITH_ATTR(dlg, focus == PROMPT_CANCEL ? A_REVERSE : A_NORMAL,
              mvwprintw(dlg, btn_y, btn_x + 8, "[ Cancel ]"));

    if (focus == PROMPT_INPUT && cursor_x >= 0) {
      wmove(dlg, 3, cursor_x);
      curs_set(1);
    } else {
      curs_set(0);
    }
    wrefresh(dlg);

    UiEvent event;
    render_translate_key(wgetch(dlg), &event);

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_DOWN)) {
      focus = FOCUS_NEXT(focus, PROMPT_STOPS);
    } else if (render_event_is_special(&event, UI_KEY_BTAB) ||
               render_event_is_special(&event, UI_KEY_UP)) {
      focus = FOCUS_PREV(focus, PROMPT_STOPS);
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (focus != PROMPT_CANCEL && (input.len > 0 || opts->allow_empty))
        result = str_dup(input.buf);
      running = false;
    } else if (focus == PROMPT_INPUT) {
      text_input_handle_key(&input, &event);
    } else if (render_event_is_special(&event, UI_KEY_LEFT)) {
      focus = PROMPT_OK;
    } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
      focus = PROMPT_CANCEL;
    }
  }

  text_input_free(&input); /* Wiped when secret */
  curs_set(0);
  delwin(dlg);
  touchwin(stdscr);
  return result;
}
