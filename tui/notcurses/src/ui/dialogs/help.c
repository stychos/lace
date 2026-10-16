#include "ui/dialogs/help.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>

static bool help_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  ListWidget *l = o->data;
  ListAction a = ev->type == UI_EVENT_MOUSE ? list_mouse(l, ev, 0, o->rows) : list_key(l, ev);
  if (a == LIST_CANCEL) overlay_pop_this(app, o);
  return true;
}

static void help_draw(Overlay *o, NcApp *app) {
  list_draw(o->data, o->plane, &app->theme, 1, 1, o->rows, o->cols, NULL, NULL);
}

static void help_layout(Overlay *o, NcApp *app) {
  o->cols = app->cols - 8 < 76 ? app->cols - 8 : 76;
  o->rows = app->rows - 4;
}

static void help_free(void *data) {
  list_free(data);
  free(data);
}

void help_open(NcApp *app) {
  ListWidget *l = safe_calloc(1, sizeof *l);
  list_init(l, false);
  ListRow *rows = NULL;
  size_t n = 0, cap = 0;
  for (int c = 0; c < HOTKEY_CAT_COUNT; c++) {
    size_t first = n;
    for (int a = 0; a < HOTKEY_COUNT; a++) {
      if ((int)hotkey_get_category((HotkeyAction)a) != c) continue;
      if (!(hotkey_action_frontends((HotkeyAction)a) & CONFIG_FE_NOTCURSES)) continue;
      if (n + 2 > cap) { cap = cap ? cap * 2 : 64; rows = safe_realloc(rows, cap * sizeof *rows); }
      if (n == first)
        rows[n++] = (ListRow){str_dup(hotkey_category_name((HotkeyCategory)c)), NULL, NULL, -1, NULL, true};
      rows[n++] = (ListRow){str_dup(hotkey_action_name((HotkeyAction)a)), NULL,
                            hotkey_get_display(app->config, (HotkeyAction)a), a, NULL, false};
    }
  }
  list_set_rows(l, rows, n);
  Overlay *o = overlay_new("Keys");
  o->footer = "Esc close  \xe2\x86\x91\xe2\x86\x93 scroll";
  o->data = l;
  o->free_data = help_free;
  o->draw = help_draw;
  o->key = help_key;
  o->layout = help_layout;
  overlay_push(app, o);
}
