/*
 * Lace
 * TUI core implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/session.h"
#include "core/actions.h"
#include "../../liblace/include/util/mem.h"
#include "tui_internal.h"
#include "views/form_tab.h"
#include "ddl_edit.h"
#include "views/config_view.h"
#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* After the current tab changed: if it shows another connection's tables,
 * the sidebar position and filter belong to the old list, so reset them.
 * Every tab switch must call this (tab_restore does). */
void tui_bind_connection(TuiState *state) {
  char **tables = TUI_TABLES(state);
  size_t num_tables = TUI_NUM_TABLES(state);
  if (tables == state->sidebar_tables_key)
    return;
  state->sidebar_tables_key = tables;
  if (num_tables > 0) {
    /* Clamp sidebar_highlight to valid range */
    if (state->sidebar_highlight >= num_tables) {
      state->sidebar_highlight = 0;
    }
    /* Clear filter as it may not match new table names */
    state->sidebar_filter[0] = '\0';
    state->sidebar_filter_len = 0;
    state->sidebar_filter_active = false;
    state->sidebar_scroll = 0;
  }
}

/* Sync view cache from AppState - call after app state changes */
void tui_sync_from_app(TuiState *state) {
  if (!state || !state->app)
    return;

  AppState *app = state->app;

  /* Ensure UITabState capacity for current workspace/tab */
  size_t ws_idx = app->current_workspace;
  Workspace *ws = app_current_workspace(app);
  if (ws && ws->num_tabs > 0) {
    size_t tab_idx = ws->current_tab;
    tui_ensure_tab_ui_capacity(state, ws_idx, tab_idx);
  }

  /* Track layout changes for window recreation */
  bool old_sidebar_visible = state->sidebar_visible;

  tui_bind_connection(state);

  /* Sync visibility toggles from app level */
  state->header_visible = app->header_visible;
  state->status_visible = app->status_visible;

  /* Tab data fields (cursor, scroll, data, schema) are accessed directly via
   * TUI_TAB() and VmTable - no sync needed. Only sync TUI-specific UI state. */
  Tab *tab = app_current_tab(app);

  /* Filter panel visibility and focus live in the tab's UITabState */
  if (!tab) {
    /* No tab - clear UI state */
    state->filters_was_focused = false;
    state->filters_cursor_row = 0;
    state->filters_cursor_col = 0;
    state->filters_scroll = 0;
    state->sidebar_visible = false;
    state->sidebar_focused = false;
    state->sidebar_highlight = 0;
    state->sidebar_scroll = 0;
    state->sidebar_filter[0] = '\0';
    state->sidebar_filter_len = 0;
  }

  /* Bind ViewModels to current tab */
  Tab *current_tab = app_current_tab(app);
  if (current_tab) {
    /* Bind or create appropriate ViewModel based on tab type */
    if (current_tab->type == TAB_TYPE_TABLE) {
      if (!state->vm_table) {
        state->vm_table = table_vm_create(app, current_tab);
      } else {
        table_vm_bind(state->vm_table, current_tab);
      }
    }
  }

  /* Recreate windows if sidebar visibility changed */
  if (old_sidebar_visible != state->sidebar_visible) {
    tui_recreate_windows(state);
  }

  /* Check if tab needs refresh due to changes in another tab */
  Tab *refresh_tab = app_current_tab(app);
  if (refresh_tab && refresh_tab->needs_refresh &&
      refresh_tab->type == TAB_TYPE_TABLE && refresh_tab->table_name) {
    refresh_tab->needs_refresh = false;
    tui_refresh_table(state);
  }

  /* Widgets read directly from Tab fields - no legacy sync needed */
}

/* Sync current tab/workspace from view cache - call before tab/workspace switch
 */
void tui_sync_to_workspace(TuiState *state) {
  if (!state || !state->app)
    return;

  AppState *app = state->app;

  /* Cancel any pending background load before switching away */
  tui_cancel_background_load(state);

  /* Save visibility toggles to app level */
  app->header_visible = state->header_visible;
  app->status_visible = state->status_visible;
}

void tui_refresh(TuiState *state) {
  tui_note_tab_switch(state);

  Tab *tab = TUI_TAB(state);
  /* The table sidebar has no window while a form is current: arriving at or
   * leaving a form by any route lays the screen out again */
  if (tab && tab->type == TAB_TYPE_FORM)
    state->sidebar_focused = false;
  if (state->main_win && (state->sidebar_win != NULL) != tui_sidebar_shown(state))
    tui_recreate_windows(state);

  tui_draw_header(state);
  tui_draw_tabs(state);
  tui_draw_sidebar(state);

  /* Dispatch drawing based on tab type */
  if (tab) {
    if (tab->type == TAB_TYPE_QUERY) {
      tui_draw_query(state);
    } else if (tab->type == TAB_TYPE_CONNECTION) {
      tui_draw_connection_tab(state);
    } else if (tab->type == TAB_TYPE_FORM) {
      form_tab_draw(state, tab);
    } else {
      tui_draw_table(state);
    }
  } else {
    tui_draw_table(state);
  }

  tui_draw_status(state);

  /* Ensure cursor is visible when editing or filtering */
  if (state->editing && state->main_win) {
    /* Inline editing - position cursor at stored location */
    wmove(state->main_win, state->edit_cursor_y, state->edit_cursor_x);
    wrefresh(state->main_win);
    curs_set(1);
  } else if (state->sidebar_filter_active && state->sidebar_focused) {
    curs_set(1);
    if (state->sidebar_win) {
      wmove(state->sidebar_win, 1, 2 + (int)state->sidebar_filter_len);
      wrefresh(state->sidebar_win);
    }
  } else {
    curs_set(0);
  }
}

void tui_set_status(TuiState *state, const char *fmt, ...) {
  if (!state)
    return;

  free(state->status_msg);

  va_list args;
  va_start(args, fmt);
  state->status_msg = str_vprintf(fmt, args);
  va_end(args);

  state->status_is_error = false;
}

void tui_set_error(TuiState *state, const char *fmt, ...) {
  if (!state)
    return;

  free(state->status_msg);

  va_list args;
  va_start(args, fmt);
  state->status_msg = str_vprintf(fmt, args);
  va_end(args);

  state->status_is_error = true;
}

/* ============================================================================
 * Hotkey Dispatch Tables
 * ============================================================================
 * Tables for dispatching hotkeys to handlers. Reduces if-else boilerplate.
 */

/* Simple action table - hotkeys that map directly to Action constructors */
typedef Action (*SimpleActionFn)(void);

typedef struct {
  HotkeyAction hotkey;
  SimpleActionFn action_fn;
} SimpleHotkeyEntry;

static const SimpleHotkeyEntry simple_hotkey_table[] = {
    /* Navigation */
    {HOTKEY_PAGE_UP, action_page_up},
    {HOTKEY_PAGE_DOWN, action_page_down},
    {HOTKEY_FIRST_COL, action_column_first},
    {HOTKEY_LAST_COL, action_column_last},
    {HOTKEY_FIRST_ROW, action_home},
    {HOTKEY_LAST_ROW, action_end},
    /* Editing */
    {HOTKEY_EDIT_INLINE, action_edit_start},
    {HOTKEY_EDIT_MODAL, action_edit_start_modal},
    {HOTKEY_SET_NULL, action_cell_set_null},
    {HOTKEY_SET_EMPTY, action_cell_set_empty},
    {HOTKEY_CELL_CUT, action_cell_cut},
    {HOTKEY_DELETE_ROW, action_row_delete},
    /* Tabs */
    {HOTKEY_NEXT_TAB, action_tab_next},
    {HOTKEY_PREV_TAB, action_tab_prev},
    {HOTKEY_NEXT_WORKSPACE, action_workspace_next},
    {HOTKEY_PREV_WORKSPACE, action_workspace_prev},
    /* UI Toggles */
    {HOTKEY_TOGGLE_HEADER, action_toggle_header},
    {HOTKEY_TOGGLE_STATUS, action_toggle_status},
};

/* Lookup simple hotkey action. Returns true if found, sets *action. */
static bool lookup_simple_hotkey(const Config *config, const UiEvent *event,
                                 Action *action) {
  for (size_t i = 0; i < ARRAY_LEN(simple_hotkey_table); i++) {
    if (hotkey_matches(config, event, simple_hotkey_table[i].hotkey)) {
      *action = simple_hotkey_table[i].action_fn();
      return true;
    }
  }
  return false;
}

/* Dialog handler table - hotkeys that open dialogs (no Action, direct call) */
typedef void (*DialogHandlerFn)(TuiState *);

typedef struct {
  HotkeyAction hotkey;
  DialogHandlerFn handler;
} DialogHotkeyEntry;

static const DialogHotkeyEntry dialog_hotkey_table[] = {
    {HOTKEY_GOTO_ROW, tui_show_goto_dialog},
    {HOTKEY_CONNECT_DIALOG, tui_show_connect_dialog},
    {HOTKEY_NEW_CONNECTION, tui_new_connection_tab},
    {HOTKEY_TOGGLE_HISTORY, tui_show_history_dialog},
    {HOTKEY_CONFIG, tui_show_config},
};

/* Lookup dialog hotkey handler. Returns true if found and executed. */
static bool lookup_dialog_hotkey(TuiState *state, const Config *config,
                                 const UiEvent *event) {
  for (size_t i = 0; i < ARRAY_LEN(dialog_hotkey_table); i++) {
    if (hotkey_matches(config, event, dialog_hotkey_table[i].hotkey)) {
      dialog_hotkey_table[i].handler(state);
      return true;
    }
  }
  return false;
}

/* ==========================================================================
 * Context-Aware Input Dispatch Tables
 *
 * These tables handle hotkeys that need to check focus state or produce
 * different actions based on context. Each handler returns an Action.
 * ========================================================================== */

/* Focus guard flags - which focus states allow the action */
typedef enum {
  FOCUS_ANY = 0,             /* Works in any focus state */
  FOCUS_TABLE = 1 << 0,      /* Must be focused on table (not sidebar/filters) */
  FOCUS_NOT_SIDEBAR = 1 << 1, /* Must not be in sidebar */
  FOCUS_NOT_FILTERS = 1 << 2, /* Must not be in filters */
  FOCUS_TABLE_ONLY = FOCUS_NOT_SIDEBAR | FOCUS_NOT_FILTERS,
} FocusGuard;

/* Context-aware action handler - returns Action based on state */
typedef Action (*ContextActionFn)(TuiState *state);

typedef struct {
  HotkeyAction hotkey;
  FocusGuard guard;
  ContextActionFn handler;
} ContextHotkeyEntry;

/* Forward declarations for context handlers */
static Action handle_move_up(TuiState *state);
static Action handle_move_down(TuiState *state);
static Action handle_move_left(TuiState *state);
static Action handle_move_right(TuiState *state);
static Action handle_toggle_sidebar(TuiState *state);
static Action handle_toggle_filters(TuiState *state);
static Action handle_filters_switch_focus(TuiState *state);

/* Wrappers for parameterless action constructors */
static Action handle_cell_copy(TuiState *state) {
  (void)state;
  return action_cell_copy();
}
static Action handle_cell_paste(TuiState *state) {
  (void)state;
  return action_cell_paste();
}
static Action handle_toggle_selection(TuiState *state) {
  (void)state;
  return action_row_toggle_select();
}

static const ContextHotkeyEntry context_hotkey_table[] = {
    /* Navigation - context-aware cursor movement */
    {HOTKEY_MOVE_UP, FOCUS_ANY, handle_move_up},
    {HOTKEY_MOVE_DOWN, FOCUS_ANY, handle_move_down},
    {HOTKEY_MOVE_LEFT, FOCUS_ANY, handle_move_left},
    {HOTKEY_MOVE_RIGHT, FOCUS_ANY, handle_move_right},
    /* Editing - requires table focus */
    {HOTKEY_CELL_COPY, FOCUS_TABLE_ONLY, handle_cell_copy},
    {HOTKEY_CELL_PASTE, FOCUS_TABLE_ONLY, handle_cell_paste},
    /* Selection - requires table focus */
    {HOTKEY_TOGGLE_SELECTION, FOCUS_TABLE_ONLY, handle_toggle_selection},
    /* UI Focus */
    {HOTKEY_TOGGLE_SIDEBAR, FOCUS_ANY, handle_toggle_sidebar},
    {HOTKEY_TOGGLE_FILTERS, FOCUS_ANY, handle_toggle_filters},
    {HOTKEY_FILTERS_SWITCH_FOCUS, FOCUS_ANY, handle_filters_switch_focus},
};

/* Check if focus state passes guard requirements */
static bool check_focus_guard(TuiState *state, FocusGuard guard) {
  if (guard == FOCUS_ANY)
    return true;
  if ((guard & FOCUS_NOT_SIDEBAR) && tui_sidebar_focused(state))
    return false;
  if ((guard & FOCUS_NOT_FILTERS) && tui_filters_focused(state))
    return false;
  return true;
}

/* Lookup context-aware hotkey. Returns true if found, sets *action. */
static bool lookup_context_hotkey(TuiState *state, const Config *config,
                                  const UiEvent *event, Action *action) {
  for (size_t i = 0; i < ARRAY_LEN(context_hotkey_table); i++) {
    const ContextHotkeyEntry *entry = &context_hotkey_table[i];
    if (hotkey_matches(config, event, entry->hotkey)) {
      if (check_focus_guard(state, entry->guard)) {
        *action = entry->handler(state);
        return true;
      }
    }
  }
  return false;
}

/* ==========================================================================
 * Stateful Input Handlers
 *
 * These handlers need full TuiState access and may have side effects beyond
 * returning an Action. They return true if handled (with optional action).
 * ========================================================================== */

typedef bool (*StatefulHandlerFn)(TuiState *state, Action *action);

typedef struct {
  HotkeyAction hotkey;
  FocusGuard guard;
  StatefulHandlerFn handler;
} StatefulHotkeyEntry;

/* Forward declarations for stateful handlers */
static bool handle_quit(TuiState *state, Action *action);
static bool handle_clear_selections(TuiState *state, Action *action);
static bool handle_row_add(TuiState *state, Action *action);
static bool handle_open_query(TuiState *state, Action *action);
static bool handle_close_tab(TuiState *state, Action *action);
static bool handle_refresh(TuiState *state, Action *action);
static bool handle_cycle_sort(TuiState *state, Action *action);
static bool handle_help(TuiState *state, Action *action);
static bool handle_undo(TuiState *state, Action *action);
static bool handle_follow_fk(TuiState *state, Action *action);
static bool handle_referenced_by(TuiState *state, Action *action);
static bool handle_table_menu(TuiState *state, Action *action);

static const StatefulHotkeyEntry stateful_hotkey_table[] = {
    /* Application */
    {HOTKEY_QUIT, FOCUS_ANY, handle_quit},
    /* Editing */
    {HOTKEY_ROW_ADD, FOCUS_TABLE_ONLY, handle_row_add},
    {HOTKEY_CLEAR_SELECTIONS, FOCUS_TABLE_ONLY, handle_clear_selections},
    /* Workspaces */
    {HOTKEY_OPEN_QUERY, FOCUS_ANY, handle_open_query},
    {HOTKEY_CLOSE_TAB, FOCUS_ANY, handle_close_tab},
    /* Table Operations */
    {HOTKEY_REFRESH, FOCUS_ANY, handle_refresh},
    {HOTKEY_UNDO, FOCUS_TABLE_ONLY, handle_undo},
    {HOTKEY_FOLLOW_FK, FOCUS_TABLE_ONLY, handle_follow_fk},
    {HOTKEY_REFERENCED_BY, FOCUS_TABLE_ONLY, handle_referenced_by},
    {HOTKEY_TABLE_MENU, FOCUS_TABLE_ONLY, handle_table_menu},
    {HOTKEY_CYCLE_SORT, FOCUS_ANY, handle_cycle_sort},
    /* Help */
    {HOTKEY_HELP, FOCUS_ANY, handle_help},
};

/* Lookup stateful hotkey. Returns true if found and handled. */
static bool lookup_stateful_hotkey(TuiState *state, const Config *config,
                                   const UiEvent *event, Action *action) {
  for (size_t i = 0; i < ARRAY_LEN(stateful_hotkey_table); i++) {
    const StatefulHotkeyEntry *entry = &stateful_hotkey_table[i];
    if (hotkey_matches(config, event, entry->hotkey)) {
      if (check_focus_guard(state, entry->guard)) {
        return entry->handler(state, action);
      }
    }
  }
  return false;
}

/* ==========================================================================
 * Context Handler Implementations
 * ========================================================================== */

static Action handle_move_up(TuiState *state) {
  /* At first row with filters visible - focus filters */
  if (tui_cursor_row(state) == 0 && tui_filters_visible(state)) {
    Tab *tab = TUI_TAB(state);
    state->filters_cursor_row = tab ? tab->filters.num_filters - 1 : 0;
    if (state->filters_cursor_row == (size_t)-1)
      state->filters_cursor_row = 0;
    return action_filters_focus();
  }
  return action_cursor_move(-1, 0);
}

static Action handle_move_down(TuiState *state) {
  (void)state;
  return action_cursor_move(1, 0);
}

static Action handle_move_left(TuiState *state) {
  /* At leftmost column with sidebar visible - focus sidebar */
  if (tui_cursor_col(state) == 0 && state->sidebar_visible) {
    return action_sidebar_focus();
  }
  return action_cursor_move(0, -1);
}

static Action handle_move_right(TuiState *state) {
  (void)state;
  return action_cursor_move(0, 1);
}

static Action handle_toggle_sidebar(TuiState *state) {
  /* If sidebar visible but not focused, focus it; otherwise toggle */
  if (state->sidebar_visible && !state->sidebar_focused) {
    return action_sidebar_focus();
  }
  return action_sidebar_toggle();
}

static Action handle_toggle_filters(TuiState *state) {
  /* If filters visible but not focused, focus them; otherwise toggle */
  bool focusing = tui_filters_visible(state) && !tui_filters_focused(state);
  size_t table_col = tui_cursor_col(state);
  Tab *tab = TUI_TAB(state);

  Action action = focusing ? action_filters_focus() : action_filters_toggle();

  /* Smart filter positioning when opening or focusing from table */
  bool closing = (!focusing && tui_filters_visible(state));
  if (!closing && tab && tab->schema && table_col < tab->schema->num_columns) {
    if (tab->type == TAB_TYPE_TABLE) {
      TableFilters *f = &tab->filters;

      /* Check if column already exists in filters */
      size_t found_idx = SIZE_MAX;
      for (size_t i = 0; i < f->num_filters; i++) {
        if (f->filters[i].column_index == table_col) {
          found_idx = i;
          break;
        }
      }

      if (found_idx != SIZE_MAX) {
        /* Column exists - move cursor to that filter row */
        ColumnFilter *cf = &f->filters[found_idx];
        state->filters_cursor_row = found_idx;
        state->filters_cursor_col = filter_op_needs_value(cf->op) ? 2 : 0;
        if (found_idx < state->filters_scroll) {
          state->filters_scroll = found_idx;
        } else if (found_idx >= state->filters_scroll + MAX_VISIBLE_FILTERS) {
          state->filters_scroll = found_idx - MAX_VISIBLE_FILTERS + 1;
        }
      } else if (f->num_filters == 1) {
        /* Single filter - check if inactive, update its column */
        ColumnFilter *cf = &f->filters[0];
        bool is_raw = (cf->column_index == SIZE_MAX);
        bool is_inactive =
            (cf->value[0] == '\0' && (is_raw || filter_op_needs_value(cf->op)));
        if (is_inactive) {
          cf->column_index = table_col;
          state->filters_cursor_row = 0;
          state->filters_cursor_col = 2;
        } else {
          filters_add(f, table_col, FILTER_OP_EQ, "");
          state->filters_cursor_row = f->num_filters - 1;
          state->filters_cursor_col = 2;
        }
      } else if (f->num_filters == 0) {
        filters_add(f, table_col, FILTER_OP_EQ, "");
        state->filters_cursor_row = 0;
        state->filters_cursor_col = 2;
      } else {
        filters_add(f, table_col, FILTER_OP_EQ, "");
        state->filters_cursor_row = f->num_filters - 1;
        state->filters_cursor_col = 2;
        if (state->filters_cursor_row >= state->filters_scroll + MAX_VISIBLE_FILTERS) {
          state->filters_scroll = state->filters_cursor_row - MAX_VISIBLE_FILTERS + 1;
        }
      }
    }
  }
  return action;
}

static Action handle_filters_switch_focus(TuiState *state) {
  if (tui_filters_visible(state)) {
    return action_filters_focus();
  }
  return (Action){0};
}

/* ==========================================================================
 * Stateful Handler Implementations
 * ========================================================================== */

static bool handle_quit(TuiState *state, Action *action) {
  bool needs_confirm = state->app && state->app->config &&
                       state->app->config->general.quit_confirmation;
  if (!needs_confirm || tui_show_confirm_dialog(state, "Quit application?")) {
    *action = action_quit_force();
    return true;
  }
  return false;
}

static bool handle_clear_selections(TuiState *state, Action *action) {
  Tab *tab = TUI_TAB(state);
  if (tab && tab->num_selected > 0) {
    *action = action_rows_clear_select();
    return true;
  }
  return false;
}

static bool handle_row_add(TuiState *state, Action *action) {
  Tab *tab = TUI_TAB(state);
  if (tab && tab->type == TAB_TYPE_TABLE && tab->data) {
    if (tui_start_add_row(state)) {
      tui_refresh(state);
    }
    return true;
  }
  (void)action;
  return false;
}

static bool handle_open_query(TuiState *state, Action *action) {
  workspace_create_query(state);
  (void)action;
  return true;
}

static bool handle_close_tab(TuiState *state, Action *action) {
  Tab *tab = TUI_TAB(state);
  if (tab) {
    if (tab->type == TAB_TYPE_QUERY &&
        ((tab->query_text && tab->query_len > 0) || tab->query_results)) {
      if (!tui_show_confirm_dialog(state,
                                   "Close query tab with unsaved content?")) {
        return false;
      }
    }
    tab_close(state);
    return true;
  }
  (void)action;
  return false;
}

static bool handle_refresh(TuiState *state, Action *action) {
  Tab *tab = TUI_TAB(state);
  if (tab && tab->type == TAB_TYPE_TABLE) {
    tui_refresh_table(state);
    return true;
  }
  (void)action;
  return false;
}

static bool handle_undo(TuiState *state, Action *action) {
  Tab *tab = TUI_TAB(state);
  (void)action;
  if (!tab || tab->type != TAB_TYPE_TABLE) return false; /* query results take it themselves */
  tui_undo(state);
  return true;
}

static bool handle_follow_fk(TuiState *state, Action *action) {
  (void)action;
  tui_follow_fk(state);
  return true;
}

static bool handle_referenced_by(TuiState *state, Action *action) {
  (void)action;
  tui_referenced_by(state);
  return true;
}

static bool handle_table_menu(TuiState *state, Action *action) {
  (void)action;
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE)
    return false;
  tui_table_menu(state);
  return true;
}

static bool handle_cycle_sort(TuiState *state, Action *action) {
  Tab *tab = TUI_TAB(state);
  if (tab && tab->type == TAB_TYPE_TABLE && tab->schema && tab->data) {
    /* The cursor is on a result column; sort entries index the schema */
    size_t col = tab->cursor_col < tab->data->num_columns
                     ? db_schema_column_index(tab->schema,
                                              tab->data->columns[tab->cursor_col].name)
                     : SIZE_MAX;
    if (col < tab->schema->num_columns) {
      /* Find if column is already in sort list */
      size_t existing_idx = SIZE_MAX;
      for (size_t i = 0; i < tab->num_sort_entries; i++) {
        if (tab->sort_entries[i].column == col) {
          existing_idx = i;
          break;
        }
      }

      if (existing_idx == SIZE_MAX) {
        /* Column not in list - add with ASC if room */
        if (tab->num_sort_entries < MAX_SORT_COLUMNS) {
          tab->sort_entries[tab->num_sort_entries].column = col;
          tab->sort_entries[tab->num_sort_entries].direction = SORT_ASC;
          tab->num_sort_entries++;
        }
      } else if (tab->sort_entries[existing_idx].direction == SORT_ASC) {
        /* Was ascending -> descending */
        tab->sort_entries[existing_idx].direction = SORT_DESC;
      } else {
        /* Was descending -> remove from list */
        for (size_t i = existing_idx; i < tab->num_sort_entries - 1; i++) {
          tab->sort_entries[i] = tab->sort_entries[i + 1];
        }
        tab->num_sort_entries--;
      }
      tui_refresh_table(state);
      return true;
    }
  }
  (void)action;
  return false;
}

static bool handle_help(TuiState *state, Action *action) {
  config_view_show_tab(state, CONFIG_TAB_HOTKEYS);
  tui_refresh(state);
  (void)action;
  return true;
}

void tui_run(TuiState *state) {
  if (!state)
    return;

  tui_refresh(state);

  /* Set timeout for animation (80ms) */
  if (state->main_win)
    wtimeout(state->main_win, 80);
  wtimeout(stdscr, 80);
  if (state->sidebar_win) {
    wtimeout(state->sidebar_win, 80);
  }

  while (state->running && state->app->running) {
    /* Get input from appropriate window */
    WINDOW *input_win = state->sidebar_focused && state->sidebar_win
                            ? state->sidebar_win
                            : state->main_win;
    if (!input_win)
      input_win = stdscr; /* Terminal too small for any window */
    int ch = wgetch(input_win);

    /* Alt+key arrives as Esc and the key in one write: a key already waiting
     * right after a bare Esc carries Alt (ESCDELAY has let a sequence
     * through by now; a second Esc stays an Esc) */
    bool alt = false;
    if (ch == 27) {
      wtimeout(input_win, 0);
      int next = wgetch(input_win);
      wtimeout(input_win, 80);
      if (next != ERR && next != 27 && next != KEY_MOUSE && next != KEY_RESIZE) {
        alt = true;
        ch = next;
      }
    }

    /* Translate ncurses key to platform-independent UiEvent */
    UiEvent event;
    bool has_event = render_translate_key(ch, &event);
    if (has_event && alt && event.type == UI_EVENT_KEY) {
      event.key.mods |= UI_MOD_ALT;
      if (!event.key.is_special && event.key.key >= 'a' && event.key.key <= 'z')
        event.key.key -= 'a' - 'A'; /* a letter with Alt has no case, as with Ctrl */
    }

    /* Handle timeout - update animations and background operations */
    if (!has_event || event.type == UI_EVENT_NONE) {
      /* Poll background pagination */
      BgPollResult bg_result = tui_poll_background_load(state);

      /* The query tab's background row count */
      Tab *count_tab = TUI_TAB(state);
      if (count_tab && count_tab->query_count_op) {
        query_poll_count(state);
        if (!count_tab->query_count_op) {
          tui_refresh(state);
        }
      }

      /* Check if speculative prefetch should start (only when not already loading) */
      if (bg_result == BG_POLL_NONE) {
        tui_check_speculative_prefetch(state);
      }

      tui_update_sidebar_scroll_animation(state);

      /* Only do full refresh when data was actually merged */
      if (bg_result == BG_POLL_MERGED) {
        tui_refresh(state);
      } else if (!state->editing) {
        /* Just update sidebar animation, don't redraw everything */
        /* Skip when editing to avoid moving cursor away from edit cell */
        tui_draw_sidebar(state);
      }
      continue;
    }

    /* Clear status message on any keypress */
    if (state->status_msg) {
      free(state->status_msg);
      state->status_msg = NULL;
      state->status_is_error = false;
    }

    /* Handle mouse events first - they should work regardless of mode */
    if (event.type == UI_EVENT_MOUSE) {
      if (tui_handle_mouse_event(state, &event)) {
        tui_refresh(state);
      }
      continue;
    }

    /* Handle resize events */
    if (event.type == UI_EVENT_RESIZE) {
      tui_recreate_windows(state);
      tui_calculate_column_widths(state);
      tui_refresh(state);
      continue;
    }

    /* A connection form takes its keys first (its own Tab ring too); the
     * keys it leaves go to the global hotkeys below */
    Tab *form_tab = TUI_TAB(state);
    if (form_tab && form_tab->type == TAB_TYPE_FORM) {
      if (form_tab_handle_key(state, form_tab, &event)) {
        tui_refresh(state);
        continue;
      }
    }

    /* Handle edit mode input */
    if (state->editing && tui_handle_edit_input(state, &event)) {
      tui_refresh(state);
      continue;
    }

    /* Handle add-row mode input */
    if (state->adding_row && tui_handle_add_row_input(state, &event)) {
      tui_refresh(state);
      continue;
    }

    /* Tab / Shift+Tab move focus between panels. Exceptions keep their own
     * meaning: Tab inserts a tab in a focused query editor, the filters panel
     * walks its fields first, and an inline result edit consumes keys. */
    if (render_event_is_special(&event, UI_KEY_TAB) ||
        render_event_is_special(&event, UI_KEY_BTAB)) {
      bool is_tab = render_event_is_special(&event, UI_KEY_TAB);
      Tab *ft = TUI_TAB(state);
      UITabState *fui = TUI_TAB_UI(state);
      bool in_main = ft && !state->sidebar_focused;
      bool editor_tab = in_main && is_tab && ft->type == TAB_TYPE_QUERY &&
                        fui && !fui->query_unfocused &&
                        !fui->query_focus_results;
      bool result_edit = in_main && ft->type == TAB_TYPE_QUERY && fui &&
                         fui->query_result_editing;
      bool filters_walk = in_main && ft->type == TAB_TYPE_TABLE &&
                          tui_filters_visible(state) &&
                          tui_filters_focused(state);
      if (!editor_tab && !result_edit && !filters_walk) {
        tui_focus_cycle(state, is_tab ? 1 : -1);
        tui_refresh(state);
        continue;
      }
    }

    /* Handle query tab input */
    Tab *query_tab = TUI_TAB(state);
    if (query_tab && !state->sidebar_focused) {
      if (query_tab->type == TAB_TYPE_QUERY) {
        if (tui_handle_query_input(state, &event)) {
          tui_refresh(state);
          continue;
        }
      }
    }

    /* Handle sidebar input (filter text or navigation) */
    if (state->sidebar_focused && tui_handle_sidebar_input(state, &event)) {
      tui_refresh(state);
      continue;
    }

    /* A table tab's sub-views: the switch keys, and a list's own keys */
    if (tui_handle_subview_input(state, &event)) {
      tui_refresh(state);
      continue;
    }

    /* The table's actions (table_menu) on a table tab, before the general
     * keys: its default m is toggle_header's too, which keeps it elsewhere */
    {
      Tab *mt = TUI_TAB(state);
      if (mt && mt->type == TAB_TYPE_TABLE && !tui_sidebar_focused(state) && !tui_filters_focused(state) &&
          hotkey_matches(state->app->config, &event, HOTKEY_TABLE_MENU)) {
        tui_table_menu(state);
        tui_refresh(state);
        continue;
      }
    }

    /* Handle filters panel input if visible */
    if (tui_filters_visible(state) && tui_handle_filters_input(state, &event)) {
      tui_refresh(state);
      continue;
    }

    /* Dispatch actions based on key input using platform-independent UiEvent.
     * All key checks use render_event_* helpers for portability. */
    Action action = {0};
    bool handled = true;
    (void)render_event_get_char(&event); /* May be used for debugging */
    (void)render_event_get_fkey(&event); /* May be used for debugging */

    /* Dispatch via lookup tables - order matters for priority */
    if (lookup_simple_hotkey(state->app->config, &event, &action)) {
      /* Simple hotkey matched - action already set */
    } else if (lookup_dialog_hotkey(state, state->app->config, &event)) {
      /* Dialog hotkey handled directly */
      tui_refresh(state);
      continue;
    } else if (lookup_context_hotkey(state, state->app->config, &event,
                                     &action)) {
      /* Context-aware hotkey matched - action set by handler */
    } else if (lookup_stateful_hotkey(state, state->app->config, &event,
                                      &action)) {
      /* Stateful hotkey handled - may or may not set action */
    } else {
      handled = false;
    }

    /* Dispatch action if one was created */
    if (handled && action.type != ACTION_NONE) {
      /* Sync TuiState to workspace before dispatch so core sees current state
       */
      tui_sync_to_workspace(state);
      UICallbacks callbacks = tui_make_callbacks(state);
      ChangeFlags changes = app_dispatch(state->app, &action, &callbacks);

      /* Sync core changes from workspace to TuiState */
      if (changes & (CHANGED_SIDEBAR | CHANGED_FILTERS | CHANGED_FOCUS |
                     CHANGED_WORKSPACE | CHANGED_CONNECTION | CHANGED_TABLES |
                     CHANGED_LAYOUT)) {
        tui_sync_from_app(state);
      }
      /* Tab is now the source of truth - draw code reads from Tab directly */
    }

    tui_refresh(state);
  }
}
