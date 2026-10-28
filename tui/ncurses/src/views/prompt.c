/*
 * Lace
 * One-line prompt dialog
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "prompt.h"
#include "text_input.h"
#include "text_input_draw.h"
#include "overlay.h"
#include "../render_helpers.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/str.h"
#include <stdlib.h>
#include <string.h>

/* As the notcurses TUI's prompt: an overlay with the label, the field
 * under it (an error under that), "Enter accept  Esc cancel" */
char *tui_prompt(const PromptOptions *opts) {
  int screen_h, screen_w;
  getmaxyx(stdscr, screen_h, screen_w);
  bool has_error = opts->error_msg && opts->error_msg[0];
  int cols = screen_w - 8 < 60 ? screen_w - 8 : 60;
  if (cols < 20)
    cols = 20;
  TuiOverlay o;
  if (!tui_overlay_open(&o, opts->title ? opts->title : "", "Enter accept  Esc cancel", has_error ? 4 : 3, cols, -1,
                        -1))
    return NULL;
  (void)screen_h;
  TextInput input;
  text_input_init(&input, opts->initial, opts->secret);
  char *result = NULL;
  curs_set(1);
  for (;;) {
    tui_overlay_frame(&o);
    int w = o.cols - 2;
    tui_overlay_put(&o, 0, 1, w, TC_OVERLAY_FG, 0, opts->label ? opts->label : "");
    int cx = text_input_draw(o.win, &input, 2, 2, w, tui_theme_attr(TC_OVERLAY_FG, TC_EDIT_BG), true);
    if (has_error)
      tui_overlay_put(&o, 2, 1, w, TC_ERROR_FG, 0, opts->error_msg);
    if (cx >= 0)
      wmove(o.win, 2, cx);
    tui_overlay_refresh(&o);
    UiEvent event;
    if (!render_translate_key(wgetch(o.win), &event))
      continue;
    if (event.type == UI_EVENT_RESIZE)
      break;
    if (event.type == UI_EVENT_MOUSE) {
      bool wheel = event.mouse.button == UI_MOUSE_SCROLL_UP || event.mouse.button == UI_MOUSE_SCROLL_DOWN;
      if (!wheel && !tui_overlay_contains(&o, event.mouse.y, event.mouse.x))
        break; /* a click outside cancels */
      continue;
    }
    if (render_event_is_special(&event, UI_KEY_ESCAPE))
      break;
    if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (input.len > 0 || opts->allow_empty)
        result = str_dup(input.buf ? input.buf : "");
      break;
    }
    text_input_handle_key(&input, &event);
  }
  text_input_free(&input); /* Wiped when secret */
  curs_set(0);
  tui_overlay_close(&o);
  return result;
}
