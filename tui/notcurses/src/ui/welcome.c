#include "app.h"
#include "ui/conn_picker.h"
#include "util/mem.h"
#include <notcurses/notcurses.h>
#include <string.h>

/* The welcome screen: shown while the current workspace has no tab. A
 * centered panel with the picker (input, saved connections, recent). */
typedef struct { ConnPicker picker; } Welcome;

static Welcome *state(NcApp *app) {
  if (!app->welcome) {
    Welcome *w = safe_calloc(1, sizeof *w);
    cp_init(app, &w->picker, true);
    app->welcome = w;
  }
  return app->welcome;
}

void welcome_free(NcApp *app) {
  Welcome *w = app->welcome;
  if (!w) return;
  cp_free(&w->picker);
  free(w);
  app->welcome = NULL;
}

static void block(NcApp *app, int *y, int *x, int *h, int *w) {
  *w = app->cols - 4 < 72 ? app->cols - 4 : 72;
  if (*w < 10) *w = 10;
  *h = app->rows - 4 < 22 ? app->rows - 4 : 22;
  if (*h < 6) *h = 6;
  *y = (app->rows - *h) / 2;
  if (*y < 1) *y = 1;
  *x = (app->cols - *w) / 2;
  if (*x < 0) *x = 0;
}

void welcome_draw(NcApp *app) {
  Welcome *w = state(app);
  int y, x, h, width;
  block(app, &y, &x, &h, &width);
  const Theme *t = &app->theme;
  nc_style(app->std, t, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
  for (int i = 0; i < h; i++) nc_put(app->std, y + i, x, width, "");
  nc_style(app->std, t, TC_ACCENT, TC_OVERLAY_BG, NCSTYLE_BOLD);
  nc_put(app->std, y, x + (width - 4) / 2, 4, "Lace");
  int cy = y + 2, cx = x + 3;
  cp_draw(app, &w->picker, app->std, y + 2, x, h - 3, width, &cy, &cx);
  nc_style(app->std, t, TC_DIM, TC_OVERLAY_BG, 0);
  nc_put(app->std, y + h - 1, x + 1, width - 2, "Ctrl+P palette \xc2\xb7 n new connection \xc2\xb7 ? keys");
  if (w->picker.input_focused) notcurses_cursor_enable(app->nc, cy, cx);
  else notcurses_cursor_disable(app->nc);
}

bool welcome_handle_key(NcApp *app, const UiEvent *ev) {
  if (ev->type != UI_EVENT_KEY) return false;
  Welcome *w = state(app);
  /* An empty input leaves "?", "{", "}", "," (preferences) and "q" to the global keys (help,
   * workspace switching, quit) and "n" opens the new-connection form; once
   * text is typed every key goes to the input */
  if (w->picker.input_focused && w->picker.in.len == 0 && !ev->key.is_special &&
      !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
    int k = ev->key.key;
    if (k == '?' || k == '{' || k == '}' || k == 'q' || k == ',') return false;
    if (k == 'n') {
      cp_perform(app, &w->picker, CP_NEW, NULL);
      return true;
    }
  }
  CpResult r = cp_key(app, &w->picker, ev, false);
  if (r.act != CP_NONE) cp_perform(app, &w->picker, r.act, NULL);
  return r.consumed;
}

/* A click on the input focuses it, on a row selects and opens it; the wheel scrolls the list */
bool welcome_handle_mouse(NcApp *app, const UiEvent *ev) {
  if (ev->type != UI_EVENT_MOUSE) return false;
  Welcome *w = state(app);
  int y, x, h, width;
  block(app, &y, &x, &h, &width);
  if (ev->mouse.button == UI_MOUSE_LEFT && ev->mouse.action == UI_MOUSE_PRESS && ev->mouse.y == y + 2 &&
      ev->mouse.x >= x && ev->mouse.x < x + width) {
    w->picker.input_focused = true;
    return true;
  }
  if (ev->mouse.x < x || ev->mouse.x >= x + width) return true;
  if (list_mouse(&w->picker.list, ev, y + 4, h - 5) == LIST_ACTIVATE) {
    w->picker.input_focused = false;
    cp_perform(app, &w->picker, CP_ROW, NULL);
  }
  return true;
}
