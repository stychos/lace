/*
 * Lace
 * Preferences: see prefs_dialog.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "prefs_dialog.h"
#include "config_view.h"
#include "list_draw.h"
#include "overlay.h"
#include "theme.h"
#include "../core/prefs_model.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/mem.h"
#include "../../../liblace/include/util/str.h"
#include <stdlib.h>

#define SECTION "ncurses"
#define NAME_COLS 25

static int rows_of(int *idx, int cap) {
  int n = 0;
  for (int i = 0; i < prefs_model_count() && n < cap; i++)
    if (prefs_model_for(i, CONFIG_FE_NCURSES))
      idx[n++] = i;
  return n;
}

static void fill(ListWidget *l, Config *c) {
  int idx[64];
  int n = rows_of(idx, 64);
  ListRow *rows = safe_calloc((size_t)(n ? n : 1), sizeof *rows);
  for (int r = 0; r < n; r++) {
    char *v = prefs_model_value(c, idx[r], SECTION);
    rows[r].text = str_printf("%-*s%s", NAME_COLS, prefs_model_name(idx[r]), v ? v : "");
    free(v);
    rows[r].kind = idx[r];
  }
  list_set_rows(l, rows, (size_t)n);
}

bool tui_prefs_change(TuiState *state, int i, int dir) {
  Config *cfg = state->app->config;
  config_section(cfg, SECTION); /* the frontend's own section exists from now on */
  prefs_model_change(cfg, i, dir, SECTION);
  if (prefs_model_is_scheme(i)) { /* the colours follow at once */
    tui_theme_init(cfg->general.color_scheme, state->terminal_dark);
    clearok(curscr, TRUE);
  }
  state->header_visible = state->app->header_visible = cfg->general.show_header;
  state->status_visible = state->app->status_visible = cfg->general.show_status_bar;
  tui_recreate_windows(state);
  char *err = NULL;
  if (!config_save(cfg, &err)) {
    tui_set_error(state, "config: %s", err ? err : "cannot save");
    free(err);
    return false;
  }
  return true;
}

/* A row whose kind is PREF_OPEN opens the hotkeys editor, which saves its
 * own changes; true when the overlay should close for it */
static bool change(TuiState *state, ListWidget *l, int i, int dir) {
  if (prefs_model_kind(i) == PREF_OPEN)
    return true;
  tui_prefs_change(state, i, dir);
  size_t cur = l->cursor;
  fill(l, state->app->config);
  l->cursor = cur;
  return false;
}

void tui_prefs_open(TuiState *state) {
  if (!state)
    return;
  ListWidget list;
  list_init(&list, false);
  fill(&list, state->app->config);
  int sr, sc;
  getmaxyx(stdscr, sr, sc);
  int idx[64];
  int n = rows_of(idx, 64);
  int rows = n < sr - 6 ? n : sr - 6;
  if (rows < 3)
    rows = 3;
  bool open_hotkeys = false;
  TuiOverlay o;
  if (tui_overlay_open(&o, "Preferences", "Space change  \xe2\x86\x90\xe2\x86\x92 cycle  Esc close", rows,
                       sc - 8 < 56 ? sc - 8 : 56, -1, -1)) {
    for (;;) {
      tui_overlay_frame(&o);
      tui_list_draw(&list, &o, 0, 0, o.rows, o.cols, NULL, NULL);
      tui_overlay_refresh(&o);
      UiEvent ev;
      if (!render_translate_key(wgetch(o.win), &ev))
        continue;
      if (ev.type == UI_EVENT_RESIZE)
        break;
      ListRow *r = list_selected(&list);
      if (ev.type == UI_EVENT_KEY && r) {
        bool space = !ev.key.is_special && ev.key.key == ' ' && !ev.key.mods;
        bool left = render_event_is_special(&ev, UI_KEY_LEFT);
        bool right = render_event_is_special(&ev, UI_KEY_RIGHT);
        if (space || left || right || render_event_is_special(&ev, UI_KEY_ENTER)) {
          if (change(state, &list, r->kind, left ? -1 : 1)) {
            open_hotkeys = true;
            break;
          }
          continue;
        }
      }
      ListAction a;
      if (ev.type == UI_EVENT_MOUSE) {
        if (!tui_overlay_contains(&o, ev.mouse.y, ev.mouse.x)) {
          if (ev.mouse.button != UI_MOUSE_SCROLL_UP && ev.mouse.button != UI_MOUSE_SCROLL_DOWN)
            break;
          continue;
        }
        a = tui_list_mouse(&list, &o, &ev, 0, o.rows);
        if (a == LIST_ACTIVATE && (r = list_selected(&list)) && change(state, &list, r->kind, 1)) {
          open_hotkeys = true;
          break;
        }
        continue;
      }
      a = list_key(&list, &ev);
      if (a == LIST_CANCEL)
        break;
    }
    tui_overlay_close(&o);
  }
  list_free(&list);
  tui_recreate_windows(state);
  tui_refresh(state);
  if (open_hotkeys) {
    config_view_show_tab(state, CONFIG_TAB_HOTKEYS);
    tui_refresh(state);
  }
}
