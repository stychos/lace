/*
 * Lace
 * Focus: arriving at a tab, and the Tab / Shift+Tab focus ring
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/session.h"
#include "core/actions.h"
#include "../../liblace/include/util/mem.h"
#include "tui_internal.h"
#include "views/config_view.h"
#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


/* Arriving at a query tab (tab keys, mouse, closing a tab, workspace switch,
 * session restore) must not put keystrokes into its editor, where '[' and
 * ']' are text: leave it unfocused until Enter or Tab. */
void tui_note_tab_switch(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  uint64_t uid = tab ? tab->uid : 0;
  if (uid == state->shown_tab_uid)
    return;
  state->shown_tab_uid = uid;
  UITabState *ui = TUI_TAB_UI(state);
  if (tab && tab->type == TAB_TYPE_QUERY && ui && !ui->query_result_editing) {
    ui->query_unfocused = true;
  }
}

/* Focus stops of the main screen in spatial order (Tab / Shift+Tab) */
typedef enum {
  FOCUS_STOP_SIDEBAR,
  FOCUS_STOP_FILTERS,
  FOCUS_STOP_TABLE,
  FOCUS_STOP_EDITOR,
  FOCUS_STOP_RESULTS,
} FocusStop;

bool tui_focus_cycle(TuiState *state, int dir) {
  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || dir == 0)
    return false;

  FocusStop stops[5];
  size_t n = 0;
  if (state->sidebar_visible)
    stops[n++] = FOCUS_STOP_SIDEBAR;
  if (tab->type == TAB_TYPE_TABLE) {
    if (tui_filters_visible(state))
      stops[n++] = FOCUS_STOP_FILTERS;
    stops[n++] = FOCUS_STOP_TABLE;
  } else if (tab->type == TAB_TYPE_QUERY && ui) {
    stops[n++] = FOCUS_STOP_EDITOR;
    if (tab->query_results)
      stops[n++] = FOCUS_STOP_RESULTS;
  }
  if (n == 0)
    return false;

  /* Where focus is now. An unfocused query tab sits just before its editor:
   * Tab enters the editor, Shift+Tab the panel before it. */
  FocusStop cur;
  bool between = false;
  if (state->sidebar_focused) {
    cur = FOCUS_STOP_SIDEBAR;
  } else if (tab->type == TAB_TYPE_TABLE) {
    cur = tui_filters_focused(state) ? FOCUS_STOP_FILTERS : FOCUS_STOP_TABLE;
  } else if (tab->type == TAB_TYPE_QUERY && ui) {
    if (ui->query_unfocused) {
      cur = FOCUS_STOP_EDITOR;
      between = true;
    } else {
      cur = ui->query_focus_results && tab->query_results ? FOCUS_STOP_RESULTS
                                                          : FOCUS_STOP_EDITOR;
    }
  } else {
    return false; /* Connection tab: the sidebar is the only panel */
  }

  size_t idx = 0;
  while (idx < n && stops[idx] != cur)
    idx++;
  if (idx == n)
    idx = 0;
  size_t target_idx;
  if (between && dir > 0)
    target_idx = idx; /* Enter the editor itself */
  else
    target_idx = (idx + n + (dir > 0 ? 1 : n - 1)) % n;
  FocusStop target = stops[target_idx];
  if (!between && target == cur)
    return false;

  /* Leave the current panel */
  if (state->sidebar_focused && target != FOCUS_STOP_SIDEBAR) {
    state->sidebar_last_position = tui_sidebar_highlight(state);
    state->sidebar_filter_active = false;
    tui_set_sidebar_focused(state, false);
  }
  if (tab->type == TAB_TYPE_TABLE && target != FOCUS_STOP_FILTERS) {
    state->filters_editing = false;
    tui_set_filters_focused(state, false);
  }

  switch (target) {
  case FOCUS_STOP_SIDEBAR:
    state->filters_was_focused = false;
    tui_set_sidebar_focused(state, true);
    state->sidebar_highlight = state->sidebar_last_position;
    break;
  case FOCUS_STOP_FILTERS: {
    TableFilters *f = &tab->filters;
    if (f->num_filters == 0)
      filters_add(f, 0, FILTER_OP_EQ, "");
    state->filters_editing = false;
    if (dir > 0) {
      state->filters_cursor_row = 0;
      state->filters_cursor_col = 0;
    } else {
      /* Entering backwards lands on the last field, like a form */
      state->filters_cursor_row = f->num_filters - 1;
      state->filters_cursor_col =
          f->filters[f->num_filters - 1].op == FILTER_OP_BETWEEN ? 4 : 3;
    }
    tui_set_filters_focused(state, true);
    break;
  }
  case FOCUS_STOP_TABLE:
    break;
  case FOCUS_STOP_EDITOR:
  case FOCUS_STOP_RESULTS:
    if (ui) {
      ui->query_unfocused = false;
      ui->query_focus_results = target == FOCUS_STOP_RESULTS;
    }
    break;
  }
  return true;
}

unsigned tui_focus_context(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab)
    return 0;
  if (tab->type == TAB_TYPE_FORM)
    return HOTKEY_CTX_CONNECT;
  if (state->sidebar_focused || tab->type == TAB_TYPE_CONNECTION)
    return HOTKEY_CTX_SIDEBAR;
  if (tab->type == TAB_TYPE_TABLE) {
    if (tui_filters_visible(state) && tui_filters_focused(state))
      return HOTKEY_CTX_FILTERS;
    return tab->subview != SUBVIEW_DATA ? HOTKEY_CTX_LIST : HOTKEY_CTX_GRID;
  }
  if (tab->type == TAB_TYPE_QUERY && ui) {
    if (ui->query_unfocused)
      return HOTKEY_CTX_UNFOCUSED;
    return ui->query_focus_results && tab->query_results ? HOTKEY_CTX_RESULTS : HOTKEY_CTX_EDITOR;
  }
  return 0;
}
