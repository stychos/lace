/*
 * Lace
 * The log and the view switcher: see panels.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "panels.h"
#include "form_tab.h"
#include "list_draw.h"
#include "overlay.h"
#include "../core/chrome.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/mem.h"
#include "../../../liblace/include/util/str.h"
#include <stdint.h>
#include <stdlib.h>

void tui_log_open(TuiState *state) {
  if (!state)
    return;
  ListWidget list;
  list_init(&list, false);
  size_t n = state->num_log;
  ListRow *rows = safe_calloc(n ? n : 1, sizeof *rows);
  for (size_t i = 0; i < n; i++)
    rows[i] = (ListRow){str_dup(state->log[i].line), NULL, NULL, state->log[i].error, NULL, false};
  list_set_rows(&list, rows, n);
  if (n)
    list.cursor = n - 1;
  int sr, sc;
  getmaxyx(stdscr, sr, sc);
  TuiOverlay o;
  char *copy = NULL;
  if (tui_overlay_open(&o, "Log", "Enter copy  Esc close  \xe2\x86\x91\xe2\x86\x93 scroll", sr - 6,
                       sc - 6 < 130 ? sc - 6 : 130, -1, -1)) {
    for (;;) {
      tui_overlay_frame(&o);
      if (!n)
        tui_overlay_put(&o, 0, 1, o.cols - 2, TC_DIM, 0, "Nothing yet");
      else
        tui_list_draw(&list, &o, 0, 0, o.rows, o.cols, NULL, NULL);
      tui_overlay_refresh(&o);
      UiEvent ev;
      if (!render_translate_key(wgetch(o.win), &ev))
        continue;
      if (ev.type == UI_EVENT_RESIZE)
        break;
      if (ev.type == UI_EVENT_KEY && hotkey_matches(state->app->config, &ev, HOTKEY_TOGGLE_LOG))
        break; /* the same key closes it */
      ListAction a;
      if (ev.type == UI_EVENT_MOUSE) {
        bool wheel = ev.mouse.button == UI_MOUSE_SCROLL_UP || ev.mouse.button == UI_MOUSE_SCROLL_DOWN;
        if (!wheel && !tui_overlay_contains(&o, ev.mouse.y, ev.mouse.x))
          break;
        a = tui_list_mouse(&list, &o, &ev, 0, o.rows);
      } else {
        a = list_key(&list, &ev);
      }
      if (a == LIST_CANCEL)
        break;
      if (a == LIST_ACTIVATE) {
        ListRow *r = list_selected(&list);
        if (r) {
          copy = str_dup(r->text);
          break;
        }
      }
    }
    tui_overlay_close(&o);
  }
  list_free(&list);
  tui_recreate_windows(state);
  if (copy) {
    tui_clipboard_copy(state, copy);
    tui_set_status(state, "Copied the log line");
    str_secure_free(copy);
  }
  tui_refresh(state);
}

/* "2 filters, 1 sort", "ran" */
static char *summary(const Tab *t) {
  if (t->type == TAB_TYPE_QUERY)
    return str_dup(t->query_results || t->query_exec_success || t->query_error ? "ran" : "");
  if (t->type != TAB_TYPE_TABLE)
    return str_dup("");
  size_t nf = 0;
  for (size_t i = 0; i < t->filters.num_filters; i++)
    nf += t->filters.filters[i].value[0] != '\0' || !filter_op_needs_value(t->filters.filters[i].op);
  char *f = nf ? str_printf("%zu filter%s", nf, nf == 1 ? "" : "s") : str_dup("");
  char *s = t->num_sort_entries ? str_printf("%s%zu sort%s", nf ? ", " : "", t->num_sort_entries,
                                             t->num_sort_entries == 1 ? "" : "s")
                                : str_dup("");
  char *out = str_printf("%s%s", f, s);
  free(f);
  free(s);
  return out;
}

/* A tab's row text: its strip label (a connection's own tab "(no views)") */
static char *row_text(TuiState *state, const Tab *t) {
  if (t->type == TAB_TYPE_FORM)
    return form_tab_label(t);
  if (t->type == TAB_TYPE_CONNECTION) {
    Connection *c = app_get_connection(state->app, t->connection_index);
    char *name = c ? chrome_connstr_short_name(c->connstr) : str_dup("connection");
    char *out = str_printf("%s (no views)", name);
    free(name);
    return out;
  }
  if (t->type == TAB_TYPE_QUERY) {
    char buf[256];
    return str_dup(chrome_query_title(t->query_text, buf, sizeof buf));
  }
  return t->preview ? str_printf("%s (preview)", t->table_name ? t->table_name : "?")
                    : str_dup(t->table_name ? t->table_name : "?");
}

void tui_view_switcher_open(TuiState *state) {
  Workspace *ws = state ? TUI_WORKSPACE(state) : NULL;
  if (!ws || !ws->num_tabs)
    return;
  ListWidget list;
  list_init(&list, false);
  ListRow *rows = safe_calloc(ws->num_tabs, sizeof *rows);
  size_t n = 0, cur = 0;
  for (size_t i = 0; i < ws->num_tabs; i++) {
    const Tab *t = &ws->tabs[i];
    if (!t->active)
      continue;
    if (i == ws->current_tab)
      cur = n;
    rows[n] = (ListRow){row_text(state, t), summary(t), NULL, 0, (void *)(intptr_t)i, false};
    n++;
  }
  list_set_rows(&list, rows, n);
  list.cursor = cur;
  int sr, sc;
  getmaxyx(stdscr, sr, sc);
  int h = (int)n < sr - 6 ? (int)n : sr - 6;
  if (h < 3)
    h = 3;
  TuiOverlay o;
  intptr_t pick = -1;
  bool close_it = false;
  if (tui_overlay_open(&o, "Views", "Enter switch  x close view  Esc cancel", h, sc - 8 < 70 ? sc - 8 : 70, -1,
                       -1)) {
    for (;;) {
      tui_overlay_frame(&o);
      tui_list_draw(&list, &o, 0, 0, o.rows, o.cols, NULL, NULL);
      tui_overlay_refresh(&o);
      UiEvent ev;
      if (!render_translate_key(wgetch(o.win), &ev))
        continue;
      if (ev.type == UI_EVENT_RESIZE)
        break;
      if (ev.type == UI_EVENT_KEY && !ev.key.is_special && !ev.key.mods && ev.key.key == 'x') {
        ListRow *r = list_selected(&list);
        if (r) {
          pick = (intptr_t)r->data;
          close_it = true;
        }
        break;
      }
      ListAction a;
      if (ev.type == UI_EVENT_MOUSE) {
        bool wheel = ev.mouse.button == UI_MOUSE_SCROLL_UP || ev.mouse.button == UI_MOUSE_SCROLL_DOWN;
        if (!wheel && !tui_overlay_contains(&o, ev.mouse.y, ev.mouse.x))
          break;
        a = tui_list_mouse(&list, &o, &ev, 0, o.rows);
      } else {
        a = list_key(&list, &ev);
      }
      if (a == LIST_CANCEL)
        break;
      if (a == LIST_ACTIVATE) {
        ListRow *r = list_selected(&list);
        if (r)
          pick = (intptr_t)r->data;
        break;
      }
    }
    tui_overlay_close(&o);
  }
  list_free(&list);
  tui_recreate_windows(state);
  tui_refresh(state);
  ws = TUI_WORKSPACE(state);
  if (pick >= 0 && ws && (size_t)pick < ws->num_tabs) {
    if ((size_t)pick != ws->current_tab)
      tab_switch(state, (size_t)pick);
    if (close_it)
      tab_close(state);
    tui_refresh(state);
  }
}
