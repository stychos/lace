/*
 * Lace
 * TUI core implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../core/actions.h"
#include "tui_internal.h"
#include "../../config/session.h"
#include "views/config_view.h"
#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ============================================================================
 * UITabState Dynamic Array Management
 * ============================================================================
 */

#define INITIAL_TAB_UI_WS_CAPACITY 4
#define INITIAL_TAB_UI_TAB_CAPACITY 8

bool tui_ensure_tab_ui_capacity(TuiState *state, size_t ws_idx, size_t tab_idx) {
  if (!state)
    return false;

  /* Ensure workspace dimension capacity */
  if (ws_idx >= state->tab_ui_ws_capacity) {
    size_t new_ws_cap = state->tab_ui_ws_capacity == 0
                            ? INITIAL_TAB_UI_WS_CAPACITY
                            : state->tab_ui_ws_capacity * 2;
    while (new_ws_cap <= ws_idx)
      new_ws_cap *= 2;

    /* Use malloc+memcpy+free for atomic all-or-nothing allocation */
    UITabState **new_tab_ui = malloc(new_ws_cap * sizeof(UITabState *));
    if (!new_tab_ui)
      return false;

    size_t *new_capacity = malloc(new_ws_cap * sizeof(size_t));
    if (!new_capacity) {
      free(new_tab_ui);
      return false;
    }

    /* Copy existing data */
    if (state->tab_ui && state->tab_ui_ws_capacity > 0) {
      memcpy(new_tab_ui, state->tab_ui,
             state->tab_ui_ws_capacity * sizeof(UITabState *));
    }
    if (state->tab_ui_capacity && state->tab_ui_ws_capacity > 0) {
      memcpy(new_capacity, state->tab_ui_capacity,
             state->tab_ui_ws_capacity * sizeof(size_t));
    }

    /* Zero new entries */
    for (size_t i = state->tab_ui_ws_capacity; i < new_ws_cap; i++) {
      new_tab_ui[i] = NULL;
      new_capacity[i] = 0;
    }

    /* Free old and assign new atomically */
    free(state->tab_ui);
    free(state->tab_ui_capacity);
    state->tab_ui = new_tab_ui;
    state->tab_ui_capacity = new_capacity;
    state->tab_ui_ws_capacity = new_ws_cap;
  }

  /* Ensure tab dimension capacity for this workspace */
  if (!state->tab_ui[ws_idx] || tab_idx >= state->tab_ui_capacity[ws_idx]) {
    size_t old_cap = state->tab_ui_capacity[ws_idx];
    size_t new_tab_cap =
        old_cap == 0 ? INITIAL_TAB_UI_TAB_CAPACITY : old_cap * 2;
    while (new_tab_cap <= tab_idx)
      new_tab_cap *= 2;

    UITabState *new_tabs =
        realloc(state->tab_ui[ws_idx], new_tab_cap * sizeof(UITabState));
    if (!new_tabs)
      return false;

    /* Zero new entries */
    memset(&new_tabs[old_cap], 0, (new_tab_cap - old_cap) * sizeof(UITabState));

    state->tab_ui[ws_idx] = new_tabs;
    state->tab_ui_capacity[ws_idx] = new_tab_cap;
  }

  return true;
}

/* Free all UITabState arrays */
static void tui_free_tab_ui(TuiState *state) {
  if (!state || !state->tab_ui)
    return;

  for (size_t ws = 0; ws < state->tab_ui_ws_capacity; ws++) {
    if (state->tab_ui[ws]) {
      for (size_t tab = 0; tab < state->tab_ui_capacity[ws]; tab++) {
        free(state->tab_ui[ws][tab].query_result_edit_buf);
      }
      free(state->tab_ui[ws]);
    }
  }
  free(state->tab_ui);
  free(state->tab_ui_capacity);
  state->tab_ui = NULL;
  state->tab_ui_capacity = NULL;
  state->tab_ui_ws_capacity = 0;
}

/*
 * Sanitize string for single-line cell display.
 * Replaces newlines, tabs, and control characters with safe alternatives.
 * Returns a newly allocated string that must be freed.
 */
char *tui_sanitize_for_display(const char *str) {
  if (!str)
    return NULL;

  size_t len = strlen(str);
  char *result = malloc(len + 1);
  if (!result)
    return NULL;

  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    if (c == '\n' || c == '\r') {
      result[i] = ' '; /* Replace newlines with space */
    } else if (c == '\t') {
      result[i] = ' '; /* Replace tabs with space */
    } else if (c < 32 && c != 0) {
      result[i] = '?'; /* Replace other control chars */
    } else {
      result[i] = str[i];
    }
  }
  result[len] = '\0';
  return result;
}

/* Case-insensitive substring search */
const char *tui_str_istr(const char *haystack, const char *needle) {
  if (!haystack || !needle)
    return NULL;
  if (!*needle)
    return haystack;

  for (; *haystack; haystack++) {
    const char *h = haystack;
    const char *n = needle;
    while (*h && *n &&
           tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
      h++;
      n++;
    }
    if (!*n)
      return haystack;
  }
  return NULL;
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

  /* Get connection from current tab */
  Connection *conn = app_current_tab_connection(app);

  /* Track if connection/tables changed for sidebar reset */
  char **old_tables = state->tables;
  size_t old_num_tables = state->num_tables;

  if (conn) {
    state->conn = conn->conn;
    state->tables = conn->tables;
    state->num_tables = conn->num_tables;
  } else {
    state->conn = NULL;
    state->tables = NULL;
    state->num_tables = 0;
  }

  /* If tables list changed (different connection), reset sidebar position */
  bool tables_changed =
      (state->tables != old_tables) || (state->num_tables != old_num_tables);
  if (tables_changed && state->num_tables > 0) {
    /* Clamp sidebar_highlight to valid range */
    if (state->sidebar_highlight >= state->num_tables) {
      state->sidebar_highlight = 0;
    }
    /* Clear filter as it may not match new table names */
    state->sidebar_filter[0] = '\0';
    state->sidebar_filter_len = 0;
    state->sidebar_filter_active = false;
    state->sidebar_scroll = 0;
  }

  /* Sync visibility toggles from app level */
  state->header_visible = app->header_visible;
  state->status_visible = app->status_visible;

  /* Sync from current tab for data state */
  Tab *tab = app_current_tab(app);
  if (tab) {
    state->current_table = tab->table_index;
    state->data = tab->data;
    state->schema = tab->schema;
    state->cursor_row = tab->cursor_row;
    state->cursor_col = tab->cursor_col;
    state->scroll_row = tab->scroll_row;
    state->scroll_col = tab->scroll_col;
    state->total_rows = tab->total_rows;
    state->loaded_offset = tab->loaded_offset;
    state->loaded_count = tab->loaded_count;
    state->row_count_approximate = tab->row_count_approximate;
    state->unfiltered_total_rows = tab->unfiltered_total_rows;
    state->col_widths = tab->col_widths;
    state->num_col_widths = tab->num_col_widths;

    /* UI state from UITabState (source of truth) */
    UITabState *ui = TUI_TAB_UI(state);
    if (ui) {
      state->filters_visible = ui->filters_visible;
      state->filters_focused = ui->filters_focused;
      state->filters_was_focused = ui->filters_was_focused;
      state->filters_cursor_row = ui->filters_cursor_row;
      state->filters_cursor_col = ui->filters_cursor_col;
      state->filters_scroll = ui->filters_scroll;
      state->sidebar_visible = ui->sidebar_visible;
      state->sidebar_focused = ui->sidebar_focused;
      state->sidebar_highlight = ui->sidebar_highlight;
      state->sidebar_scroll = ui->sidebar_scroll;
      state->sidebar_filter_len = ui->sidebar_filter_len;
      memcpy(state->sidebar_filter, ui->sidebar_filter,
             sizeof(state->sidebar_filter));
    } else {
      /* No UITabState - use defaults */
      state->filters_visible = false;
      state->filters_focused = false;
      state->filters_was_focused = false;
      state->filters_cursor_row = 0;
      state->filters_cursor_col = 0;
      state->filters_scroll = 0;
      state->sidebar_visible = true; /* Default: sidebar visible */
      state->sidebar_focused = false;
      state->sidebar_highlight = 0;
      state->sidebar_scroll = 0;
      state->sidebar_filter[0] = '\0';
      state->sidebar_filter_len = 0;
    }
  } else {
    state->current_table = 0;
    state->data = NULL;
    state->schema = NULL;
    state->cursor_row = 0;
    state->cursor_col = 0;
    state->scroll_row = 0;
    state->scroll_col = 0;
    state->total_rows = 0;
    state->loaded_offset = 0;
    state->loaded_count = 0;
    state->row_count_approximate = false;
    state->unfiltered_total_rows = 0;
    state->col_widths = NULL;
    state->num_col_widths = 0;
    state->filters_visible = false;
    state->filters_focused = false;
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

  /* Sync workspace cache fields for legacy code compatibility */
  state->workspaces = app->workspaces;
  state->num_workspaces = app->num_workspaces;
  state->current_workspace = app->current_workspace;

  state->page_size = app->page_size;

  /* Bind ViewModels to current tab */
  Tab *current_tab = app_current_tab(app);
  if (current_tab) {
    /* Bind sidebar to current connection */
    Connection *current_conn = app_get_tab_connection(app, current_tab);
    if (state->vm_sidebar && current_conn) {
      vm_sidebar_bind(state->vm_sidebar, current_conn);
    }

    /* Bind or create appropriate ViewModel based on tab type */
    if (current_tab->type == TAB_TYPE_TABLE) {
      if (!state->vm_table) {
        state->vm_table = vm_table_create(app, current_tab, NULL);
      } else {
        vm_table_bind(state->vm_table, current_tab);
      }
    } else if (current_tab->type == TAB_TYPE_QUERY) {
      if (!state->vm_query) {
        state->vm_query = vm_query_create(app, current_tab, NULL);
      } else {
        vm_query_bind(state->vm_query, current_tab);
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

  /* Save data state to current tab */
  Tab *tab = app_current_tab(app);
  if (!tab)
    return;

  /* Data pointers - sync in case they were updated */
  tab->data = state->data;
  tab->schema = state->schema;
  tab->col_widths = state->col_widths;
  tab->num_col_widths = state->num_col_widths;
  tab->total_rows = state->total_rows;
  tab->loaded_offset = state->loaded_offset;
  tab->loaded_count = state->loaded_count;
  tab->row_count_approximate = state->row_count_approximate;
  tab->unfiltered_total_rows = state->unfiltered_total_rows;

  /* Cursor and scroll */
  tab->cursor_row = state->cursor_row;
  tab->cursor_col = state->cursor_col;
  tab->scroll_row = state->scroll_row;
  tab->scroll_col = state->scroll_col;

  /* UI state to UITabState (source of truth) */
  UITabState *ui = TUI_TAB_UI(state);
  if (ui) {
    ui->filters_visible = state->filters_visible;
    ui->filters_focused = state->filters_focused;
    ui->filters_was_focused = state->filters_was_focused;
    ui->filters_cursor_row = state->filters_cursor_row;
    ui->filters_cursor_col = state->filters_cursor_col;
    ui->filters_scroll = state->filters_scroll;
    ui->sidebar_visible = state->sidebar_visible;
    ui->sidebar_focused = state->sidebar_focused;
    ui->sidebar_highlight = state->sidebar_highlight;
    ui->sidebar_scroll = state->sidebar_scroll;
    ui->sidebar_filter_len = state->sidebar_filter_len;
    memcpy(ui->sidebar_filter, state->sidebar_filter,
           sizeof(ui->sidebar_filter));
  }
}

/* ============================================================================
 * UICallbacks wrapper functions for core/actions dispatch
 * ============================================================================
 * These adapt TUI functions to the UICallbacks interface, allowing core
 * actions.c to call TUI functions without direct dependency.
 */

static void ui_move_cursor(void *ctx, int row_delta, int col_delta) {
  tui_move_cursor((TuiState *)ctx, row_delta, col_delta);
}

static void ui_page_up(void *ctx) { tui_page_up((TuiState *)ctx); }

static void ui_page_down(void *ctx) { tui_page_down((TuiState *)ctx); }

static void ui_home(void *ctx) { tui_home((TuiState *)ctx); }

static void ui_end(void *ctx) { tui_end((TuiState *)ctx); }

static void ui_column_first(void *ctx) { tui_column_first((TuiState *)ctx); }

static void ui_column_last(void *ctx) { tui_column_last((TuiState *)ctx); }

static void ui_start_edit(void *ctx) { tui_start_edit((TuiState *)ctx); }

static void ui_start_modal_edit(void *ctx) {
  tui_start_modal_edit((TuiState *)ctx);
}

static void ui_cancel_edit(void *ctx) { tui_cancel_edit((TuiState *)ctx); }

static void ui_set_cell_null(void *ctx) {
  tui_set_cell_direct((TuiState *)ctx, true);
}

static void ui_set_cell_empty(void *ctx) {
  tui_set_cell_direct((TuiState *)ctx, false);
}

static void ui_delete_row(void *ctx) { tui_delete_row((TuiState *)ctx); }

static void ui_recreate_layout(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  /* Sync layout-related state before recreating windows */
  AppState *app = state->app;
  if (app) {
    state->header_visible = app->header_visible;
    state->status_visible = app->status_visible;
  }
  UITabState *ui = TUI_TAB_UI(state);
  if (ui) {
    state->sidebar_visible = ui->sidebar_visible;
    state->sidebar_focused = ui->sidebar_focused;
  }
  tui_recreate_windows(state);
}

static void ui_recalculate_widths(void *ctx) {
  tui_calculate_column_widths((TuiState *)ctx);
}

static bool ui_load_more_rows(void *ctx) {
  return tui_load_more_rows((TuiState *)ctx);
}

static bool ui_load_prev_rows(void *ctx) {
  return tui_load_prev_rows((TuiState *)ctx);
}

static void ui_disconnect(void *ctx) { tui_disconnect((TuiState *)ctx); }

static size_t ui_get_sidebar_highlight_for_table(void *ctx, size_t table_idx) {
  return tui_get_sidebar_highlight_for_table((TuiState *)ctx, table_idx);
}

/* ============================================================================
 * UI State Callbacks - Sidebar
 * These read/write sidebar state from UITabState (source of truth)
 * Also sync to TuiState cache for legacy code compatibility
 * ============================================================================
 */

static bool ui_is_sidebar_visible(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  return ui ? ui->sidebar_visible : state->sidebar_visible;
}

static bool ui_is_sidebar_focused(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  return ui ? ui->sidebar_focused : state->sidebar_focused;
}

static void ui_set_sidebar_visible(void *ctx, bool visible) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  if (ui)
    ui->sidebar_visible = visible;
  /* Sync to TuiState cache for legacy code */
  state->sidebar_visible = visible;
}

static void ui_set_sidebar_focused(void *ctx, bool focused) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  if (ui)
    ui->sidebar_focused = focused;
  /* Sync to TuiState cache for legacy code */
  state->sidebar_focused = focused;
}

static size_t ui_get_sidebar_highlight(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  return ui ? ui->sidebar_highlight : state->sidebar_highlight;
}

static void ui_set_sidebar_highlight(void *ctx, size_t highlight) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  if (ui)
    ui->sidebar_highlight = highlight;
  /* Sync to TuiState cache for legacy code */
  state->sidebar_highlight = highlight;
}

static void ui_set_sidebar_scroll(void *ctx, size_t scroll) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  if (ui)
    ui->sidebar_scroll = scroll;
  /* Sync to TuiState cache for legacy code */
  state->sidebar_scroll = scroll;
}

static size_t ui_get_sidebar_last_position(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  return ui ? ui->sidebar_last_position : 0;
}

static void ui_set_sidebar_last_position(void *ctx, size_t position) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  if (ui)
    ui->sidebar_last_position = position;
}

/* ============================================================================
 * UI State Callbacks - Filters Panel
 * These read/write filters state from UITabState (source of truth)
 * Also sync to TuiState cache for legacy code compatibility
 * ============================================================================
 */

static bool ui_is_filters_visible(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  return ui ? ui->filters_visible : state->filters_visible;
}

static bool ui_is_filters_focused(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  return ui ? ui->filters_focused : state->filters_focused;
}

static void ui_set_filters_visible(void *ctx, bool visible) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  if (ui)
    ui->filters_visible = visible;
  /* Sync to TuiState cache for legacy code */
  state->filters_visible = visible;
}

static void ui_set_filters_focused(void *ctx, bool focused) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  if (ui)
    ui->filters_focused = focused;
  /* Sync to TuiState cache for legacy code */
  state->filters_focused = focused;
}

static void ui_set_filters_editing(void *ctx, bool editing) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  if (ui)
    ui->filters_editing = editing;
  /* Sync to TuiState cache for legacy code */
  state->filters_editing = editing;
}

static bool ui_get_filters_was_focused(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  return ui ? ui->filters_was_focused : state->filters_was_focused;
}

static void ui_set_filters_was_focused(void *ctx, bool was_focused) {
  TuiState *state = (TuiState *)ctx;
  UITabState *ui = TUI_TAB_UI(state);
  if (ui)
    ui->filters_was_focused = was_focused;
  /* Sync to TuiState cache for legacy code */
  state->filters_was_focused = was_focused;
}

/* Build UICallbacks structure for dispatch */
static UICallbacks tui_make_callbacks(TuiState *state) {
  return (UICallbacks){
      .ctx = state,
      .move_cursor = ui_move_cursor,
      .page_up = ui_page_up,
      .page_down = ui_page_down,
      .home = ui_home,
      .end = ui_end,
      .column_first = ui_column_first,
      .column_last = ui_column_last,
      .start_edit = ui_start_edit,
      .start_modal_edit = ui_start_modal_edit,
      .cancel_edit = ui_cancel_edit,
      .set_cell_null = ui_set_cell_null,
      .set_cell_empty = ui_set_cell_empty,
      .delete_row = ui_delete_row,
      .recreate_layout = ui_recreate_layout,
      .recalculate_widths = ui_recalculate_widths,
      .load_more_rows = ui_load_more_rows,
      .load_prev_rows = ui_load_prev_rows,
      .disconnect = ui_disconnect,
      /* UI State - Sidebar */
      .is_sidebar_visible = ui_is_sidebar_visible,
      .is_sidebar_focused = ui_is_sidebar_focused,
      .set_sidebar_visible = ui_set_sidebar_visible,
      .set_sidebar_focused = ui_set_sidebar_focused,
      .get_sidebar_highlight = ui_get_sidebar_highlight,
      .set_sidebar_highlight = ui_set_sidebar_highlight,
      .set_sidebar_scroll = ui_set_sidebar_scroll,
      .get_sidebar_last_position = ui_get_sidebar_last_position,
      .set_sidebar_last_position = ui_set_sidebar_last_position,
      .get_sidebar_highlight_for_table = ui_get_sidebar_highlight_for_table,
      /* UI State - Filters Panel */
      .is_filters_visible = ui_is_filters_visible,
      .is_filters_focused = ui_is_filters_focused,
      .set_filters_visible = ui_set_filters_visible,
      .set_filters_focused = ui_set_filters_focused,
      .set_filters_editing = ui_set_filters_editing,
      .get_filters_was_focused = ui_get_filters_was_focused,
      .set_filters_was_focused = ui_set_filters_was_focused,
  };
}

bool tui_init(TuiState *state, AppState *app) {
  if (!state || !app)
    return false;

  memset(state, 0, sizeof(TuiState));
  state->app = app;

  /* Initialize ViewModels */
  state->vm_app = vm_app_create(app, NULL);
  if (state->vm_app) {
    state->vm_sidebar = vm_app_sidebar_vm(state->vm_app);
  }
  /* vm_table and vm_query are created on-demand when tabs are accessed */

  /* Set locale for UTF-8 support */
  setlocale(LC_ALL, "");

  /* Initialize ncurses */
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0); /* Hide cursor */

  /* Define Ctrl+Home and Ctrl+End key sequences */
  define_key("\033[1;5H", KEY_F(61)); /* Ctrl+Home - xterm */
  define_key("\033[7^", KEY_F(61));   /* Ctrl+Home - rxvt */
  define_key("\033[1;5F", KEY_F(62)); /* Ctrl+End - xterm */
  define_key("\033[8^", KEY_F(62));   /* Ctrl+End - rxvt */

  /* Enable mouse support (including scroll wheel) */
  mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON4_PRESSED |
                BUTTON5_PRESSED,
            NULL);
  mouseinterval(300); /* Double-click interval in ms */

  /* Setup colors */
  if (has_colors()) {
    start_color();
    use_default_colors();

    /* Define color pairs */
    init_pair(COLOR_HEADER, COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_SELECTED, COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_STATUS, COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_ERROR, COLOR_WHITE, COLOR_RED);
    init_pair(COLOR_BORDER, COLOR_CYAN, -1);
    init_pair(COLOR_TITLE, COLOR_YELLOW, -1);
    init_pair(COLOR_NULL, COLOR_MAGENTA, -1);
    init_pair(COLOR_NUMBER, COLOR_CYAN, -1);
    init_pair(COLOR_EDIT, COLOR_BLACK, COLOR_YELLOW);
    init_pair(COLOR_ERROR_TEXT, COLOR_RED, -1);
    init_pair(COLOR_PK, COLOR_YELLOW, -1);
  }

  /* Get terminal dimensions */
  getmaxyx(stdscr, state->term_rows, state->term_cols);

  /* Clamp to minimum dimensions to prevent negative calculations */
  if (state->term_rows < MIN_TERM_ROWS)
    state->term_rows = MIN_TERM_ROWS;
  if (state->term_cols < MIN_TERM_COLS)
    state->term_cols = MIN_TERM_COLS;

  /* Create windows */
  state->header_win = newwin(1, state->term_cols, 0, 0);
  state->status_win = newwin(1, state->term_cols, state->term_rows - 1, 0);

  /* Calculate main window dimensions (leave room for header, tab bar, status)
   */
  state->content_rows = state->term_rows - 4;

  /* Tab bar */
  state->tab_win = newwin(TAB_BAR_HEIGHT, state->term_cols, 1, 0);

  /* Main window (after header and tab bar) */
  state->main_win = newwin(state->content_rows, state->term_cols, 2, 0);

  /* Validate all windows were created successfully */
  if (!state->header_win || !state->status_win || !state->tab_win ||
      !state->main_win) {
    endwin();
    return false;
  }

  scrollok(state->main_win, FALSE);
  keypad(state->main_win, TRUE);

  state->running = true;
  state->app->running = true;
  state->header_visible = true;
  state->sidebar_visible = false;
  state->sidebar_focused = false;
  state->status_visible = true;

  return true;
}

void tui_cleanup(TuiState *state) {
  if (!state)
    return;

  /* Save session before cleanup (only if restore_session is enabled) */
  if (state->app && state->app->config &&
      state->app->config->general.restore_session) {
    char *session_err = NULL;
    if (!session_save(state, &session_err)) {
      /* Log error but don't block quit */
      if (session_err)
        free(session_err);
    }
  }

  tui_disconnect(state);

  /* Cleanup ViewModels */
  vm_table_destroy(state->vm_table);
  state->vm_table = NULL;
  vm_query_destroy(state->vm_query);
  state->vm_query = NULL;
  /* vm_sidebar is owned by vm_app, don't destroy separately */
  state->vm_sidebar = NULL;
  vm_app_destroy(state->vm_app);
  state->vm_app = NULL;

  /* Free UITabState dynamic arrays */
  tui_free_tab_ui(state);

  /* Free status message */
  free(state->status_msg);

  /* Free internal clipboard buffer */
  free(state->clipboard_buffer);

  /* Delete windows */
  if (state->main_win)
    delwin(state->main_win);
  if (state->status_win)
    delwin(state->status_win);
  if (state->header_win)
    delwin(state->header_win);
  if (state->sidebar_win)
    delwin(state->sidebar_win);
  if (state->tab_win)
    delwin(state->tab_win);

  /* End ncurses */
  endwin();
}

/* Recreate windows after resize or sidebar toggle */
void tui_recreate_windows(TuiState *state) {
  if (!state)
    return;

  /* Delete old windows (except header and status) */
  if (state->main_win) {
    delwin(state->main_win);
    state->main_win = NULL;
  }
  if (state->sidebar_win) {
    delwin(state->sidebar_win);
    state->sidebar_win = NULL;
  }
  if (state->tab_win) {
    delwin(state->tab_win);
    state->tab_win = NULL;
  }

  /* Get current terminal size */
  getmaxyx(stdscr, state->term_rows, state->term_cols);

  /* Clamp to minimum dimensions to prevent negative calculations */
  if (state->term_rows < MIN_TERM_ROWS)
    state->term_rows = MIN_TERM_ROWS;
  if (state->term_cols < MIN_TERM_COLS)
    state->term_cols = MIN_TERM_COLS;

  /* Calculate how many rows are used by header/status */
  int top_rows = (state->header_visible ? 1 : 0) + TAB_BAR_HEIGHT;
  int bottom_rows = state->status_visible ? 1 : 0;

  /* Resize header and status */
  if (state->header_visible) {
    wresize(state->header_win, 1, state->term_cols);
    mvwin(state->header_win, 0, 0);
  }
  if (state->status_visible) {
    wresize(state->status_win, 1, state->term_cols);
    mvwin(state->status_win, state->term_rows - 1, 0);
  }

  /* Create tab bar */
  int tab_y = state->header_visible ? 1 : 0;
  state->tab_win = newwin(TAB_BAR_HEIGHT, state->term_cols, tab_y, 0);

  /* Calculate main window dimensions */
  int main_start_y = top_rows;
  int main_height = state->term_rows - top_rows - bottom_rows;
  int main_start_x = 0;
  int main_width = state->term_cols;

  state->content_rows = main_height - 3;

  if (state->sidebar_visible) {
    /* Create sidebar window (starts after tab bar) */
    state->sidebar_win = newwin(main_height, SIDEBAR_WIDTH, main_start_y, 0);
    if (state->sidebar_win) {
      keypad(state->sidebar_win, TRUE);
      wtimeout(state->sidebar_win, 80); /* For scroll animation */
    }
    main_start_x = SIDEBAR_WIDTH;
    main_width = state->term_cols - SIDEBAR_WIDTH;
  } else {
    state->sidebar_win = NULL;
  }

  /* Create main window */
  state->main_win = newwin(main_height, main_width, main_start_y, main_start_x);
  if (state->main_win) {
    scrollok(state->main_win, FALSE);
    keypad(state->main_win, TRUE);
  }

  /* Update content dimensions */
  state->content_cols = main_width - 2;
}

bool tui_connect(TuiState *state, const char *connstr) {
  if (!state || !state->app || !connstr)
    return false;

  /* Use async connection with progress dialog */
  DbConnection *conn = tui_connect_with_progress(state, connstr);

  if (!conn) {
    /* Error already set by tui_connect_with_progress */
    return false;
  }

  /* Create a new connection in the app state hierarchy */
  Connection *app_conn = app_add_connection(state->app, conn, connstr);
  if (!app_conn) {
    db_disconnect(conn);
    tui_set_error(state, "Failed to create connection");
    return false;
  }

  /* Sync to TUI state cache */
  state->conn = conn;

  /* Load tables first */
  if (!tui_load_tables(state)) {
    return false;
  }

  /* Create a workspace if none exists */
  Workspace *ws = app_current_workspace(state->app);
  if (!ws) {
    ws = app_create_workspace(state->app);
    if (!ws) {
      tui_set_error(state, "Failed to create workspace");
      return false;
    }
  }

  /* Find connection index */
  size_t conn_index = app_find_connection_index(state->app, conn);

  /* Check if we should auto-open the first table */
  bool auto_open = state->app->config &&
                   state->app->config->general.auto_open_first_table &&
                   state->num_tables > 0;

  if (auto_open) {
    /* Create a table tab with the first table */
    size_t table_idx = 0;
    Tab *tab = workspace_create_table_tab(ws, conn_index, table_idx, state->tables[table_idx]);
    if (tab) {
      tab->table_index = table_idx;

      /* Ensure UITabState capacity before accessing */
      size_t ws_idx = state->app->current_workspace;
      size_t tab_idx_ui = ws->current_tab;
      tui_ensure_tab_ui_capacity(state, ws_idx, tab_idx_ui);

      /* Load table data */
      state->current_table = table_idx;
      tui_load_table_data(state, state->tables[table_idx]);

      /* Save to tab */
      tab->data = state->data;
      tab->schema = state->schema;
      tab->col_widths = state->col_widths;
      tab->num_col_widths = state->num_col_widths;
      tab->total_rows = state->total_rows;
      tab->loaded_offset = state->loaded_offset;
      tab->loaded_count = state->loaded_count;

      /* Initialize UI state */
      UITabState *ui = TUI_TAB_UI(state);
      if (ui) {
        ui->sidebar_visible = true;
        ui->sidebar_focused = false;
      }
      state->sidebar_visible = true;
      state->sidebar_focused = false;

      /* Sync workspace cache */
      state->workspaces = state->app->workspaces;
      state->num_workspaces = state->app->num_workspaces;
      state->current_workspace = state->app->current_workspace;

      tui_recreate_windows(state);
      tui_set_status(state, "Connected to %s - %s", conn->database,
                     state->tables[table_idx]);
      return true;
    }
  } else {
    /* Create a connection tab (don't auto-load any table) */
    Tab *tab = workspace_create_connection_tab(ws, conn_index, connstr);
    if (tab) {
      /* Ensure UITabState capacity before accessing */
      size_t ws_idx = state->app->current_workspace;
      size_t tab_idx = ws->current_tab;
      tui_ensure_tab_ui_capacity(state, ws_idx, tab_idx);

      /* Initialize UITabState for new tab - show sidebar for table selection */
      UITabState *ui = TUI_TAB_UI(state);
      if (ui) {
        ui->sidebar_visible = true;
        ui->sidebar_focused = true; /* Focus sidebar to select a table */
        ui->sidebar_highlight = 0;
        ui->sidebar_scroll = 0;
        ui->filters_visible = false;
        ui->filters_focused = false;
      }

      /* Apply UITabState to TUI cache */
      state->sidebar_visible = true;
      state->sidebar_focused = true;
      state->sidebar_highlight = 0;
      state->sidebar_scroll = 0;

      /* Clear data pointers - connection tab has no table data */
      state->data = NULL;
      state->schema = NULL;
      state->col_widths = NULL;
      state->num_col_widths = 0;

      /* Sync workspace cache */
      state->workspaces = state->app->workspaces;
      state->num_workspaces = state->app->num_workspaces;
      state->current_workspace = state->app->current_workspace;

      tui_recreate_windows(state);

      if (state->num_tables == 0) {
        tui_set_status(state, "Connected to %s - No tables found", conn->database);
      } else {
        tui_set_status(state, "Connected to %s - Select a table from sidebar",
                       conn->database);
      }
      return true;
    }
  }

  tui_set_error(state, "Failed to create tab");
  return false;
}

void tui_disconnect(TuiState *state) {
  if (!state || !state->app)
    return;

  /* Cancel all background operations before cleanup to prevent use-after-free
   */
  /* Iterate through all workspaces/tabs and cancel their bg ops */
  AppState *app = state->app;
  for (size_t w = 0; w < app->num_workspaces; w++) {
    Workspace *ws = &app->workspaces[w];
    for (size_t t = 0; t < ws->num_tabs; t++) {
      Tab *tab = &ws->tabs[t];
      if (tab->bg_load_op) {
        AsyncOperation *op = (AsyncOperation *)tab->bg_load_op;
        async_cancel(op);
        async_wait(op, 500); /* Wait for cancellation */
        /* Poll until done */
        while (async_poll(op) == ASYNC_STATE_RUNNING) {
          struct timespec ts = {0, 10000000L}; /* 10ms */
          nanosleep(&ts, NULL);
        }
        if (op->result) {
          db_result_free((ResultSet *)op->result);
          op->result = NULL;
        }
        async_free(op);
        free(op);
        tab->bg_load_op = NULL;
      }
    }
  }
  state->bg_loading_active = false;

  /* Cleanup is handled by app_state_cleanup() */
  app_state_cleanup(state->app);
  app_state_init(state->app);

  /* Hide sidebar when disconnected */
  if (state->sidebar_visible) {
    state->sidebar_visible = false;
    state->sidebar_focused = false;
    tui_recreate_windows(state);
  }

  /* Clear all cached state pointers (memory was freed by app_state_cleanup) */
  state->conn = NULL;
  state->tables = NULL;
  state->num_tables = 0;
  state->data = NULL;
  state->schema = NULL;
  state->col_widths = NULL;
  state->num_col_widths = 0;
  state->current_table = 0;
  state->cursor_row = 0;
  state->cursor_col = 0;
  state->scroll_row = 0;
  state->scroll_col = 0;
  state->total_rows = 0;
  state->loaded_offset = 0;
  state->loaded_count = 0;
  state->filters_visible = false;
  state->filters_focused = false;
  state->sidebar_highlight = 0;
  state->sidebar_scroll = 0;
  state->sidebar_filter[0] = '\0';
  state->sidebar_filter_len = 0;
  state->sidebar_filter_active = false;

  /* Clear workspace cache (points to app->workspaces which was zeroed) */
  state->workspaces = state->app->workspaces;
  state->num_workspaces = 0;
  state->current_workspace = 0;

  /* Clear edit state */
  state->editing = false;
  free(state->edit_buffer);
  state->edit_buffer = NULL;
  state->edit_pos = 0;
}

bool tui_load_tables(TuiState *state) {
  if (!state || !state->app)
    return false;

  /* Get connection - prefer from current tab, fall back to state->conn */
  Connection *conn_obj = TUI_TAB_CONNECTION(state);
  DbConnection *db_conn = NULL;

  if (conn_obj && conn_obj->conn) {
    db_conn = conn_obj->conn;
  } else if (state->conn) {
    /* No tab yet - find connection in pool by DbConnection pointer */
    db_conn = state->conn;
    for (size_t i = 0; i < state->app->num_connections; i++) {
      if (state->app->connections[i].conn == db_conn) {
        conn_obj = &state->app->connections[i];
        break;
      }
    }
  }

  if (!conn_obj || !db_conn)
    return false;

  /* Free old tables */
  if (conn_obj->tables) {
    for (size_t i = 0; i < conn_obj->num_tables; i++) {
      free(conn_obj->tables[i]);
    }
    free(conn_obj->tables);
    conn_obj->tables = NULL;
    conn_obj->num_tables = 0;
  }

  /* Sync conn for async operation */
  state->conn = db_conn;

  /* Use async operation with progress dialog */
  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_LIST_TABLES;
  op.conn = db_conn;

  if (!async_start(&op)) {
    tui_set_error(state, "Failed to start operation");
    return false;
  }

  bool completed = tui_show_processing_dialog(state, &op, "Loading tables...");

  if (!completed || op.state == ASYNC_STATE_CANCELLED) {
    tui_set_status(state, "Operation cancelled");
    async_free(&op);
    return false;
  }

  if (op.state == ASYNC_STATE_ERROR) {
    tui_set_error(state, "Failed to list tables: %s",
                  op.error ? op.error : "Unknown error");
    async_free(&op);
    return false;
  }

  conn_obj->tables = (char **)op.result;
  conn_obj->num_tables = op.result_count;
  async_free(&op);

  /* Sync tables to TUI state */
  state->tables = conn_obj->tables;
  state->num_tables = conn_obj->num_tables;

  return true;
}

void tui_refresh(TuiState *state) {
  tui_draw_header(state);
  tui_draw_tabs(state);
  tui_draw_sidebar(state);

  /* Dispatch drawing based on tab type */
  Tab *tab = TUI_TAB(state);
  if (tab) {
    if (tab->type == TAB_TYPE_QUERY) {
      tui_draw_query(state);
    } else if (tab->type == TAB_TYPE_CONNECTION) {
      tui_draw_connection_tab(state);
    } else {
      tui_draw_table(state);
    }
  } else {
    tui_draw_table(state);
  }

  tui_draw_status(state);

  /* Ensure cursor is only visible when filter is active */
  if (state->sidebar_filter_active && state->sidebar_focused) {
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

void tui_run(TuiState *state) {
  if (!state)
    return;

  tui_refresh(state);

  /* Set timeout for animation (80ms) */
  wtimeout(state->main_win, 80);
  if (state->sidebar_win) {
    wtimeout(state->sidebar_win, 80);
  }

  while (state->running && state->app->running) {
    /* Get input from appropriate window */
    WINDOW *input_win = state->sidebar_focused && state->sidebar_win
                            ? state->sidebar_win
                            : state->main_win;
    int ch = wgetch(input_win);

    /* Translate ncurses key to platform-independent UiEvent */
    UiEvent event;
    bool has_event = render_translate_key(ch, &event);

    /* Handle timeout - update animations and background operations */
    if (!has_event || event.type == UI_EVENT_NONE) {
      /* Poll background pagination */
      bool bg_activity = tui_poll_background_load(state);

      /* Check if speculative prefetch should start */
      if (!bg_activity) {
        tui_check_speculative_prefetch(state);
      }

      tui_update_sidebar_scroll_animation(state);

      /* Redraw if background activity completed */
      if (bg_activity) {
        tui_refresh(state);
      } else {
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

    /* Handle edit mode input */
    if (state->editing && tui_handle_edit_input(state, &event)) {
      tui_refresh(state);
      continue;
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

    /* Handle filters panel input if visible */
    if (state->filters_visible && tui_handle_filters_input(state, &event)) {
      tui_refresh(state);
      continue;
    }

    /* Dispatch actions based on key input using platform-independent UiEvent.
     * All key checks use render_event_* helpers for portability. */
    Action action = {0};
    bool handled = true;
    (void)render_event_get_char(&event); /* May be used for debugging */
    (void)render_event_get_fkey(&event); /* May be used for debugging */

    /* ========== Application ========== */
    if (hotkey_matches(state->app->config, &event, HOTKEY_QUIT)) {
      /* Quit with confirmation only if configured */
      bool needs_confirm = false;
      if (state->app && state->app->config &&
          state->app->config->general.quit_confirmation) {
        needs_confirm = true;
      }
      if (!needs_confirm ||
          tui_show_confirm_dialog(state, "Quit application?")) {
        action = action_quit_force();
      }
    }
    /* ========== Navigation ========== */
    else if (hotkey_matches(state->app->config, &event, HOTKEY_MOVE_UP)) {
      /* At first row with filters visible - focus filters */
      if (state->cursor_row == 0 && state->filters_visible) {
        action = action_filters_focus();
        Tab *filters_tab = TUI_TAB(state);
        state->filters_cursor_row =
            filters_tab ? filters_tab->filters.num_filters - 1 : 0;
        if (state->filters_cursor_row == (size_t)-1)
          state->filters_cursor_row = 0;
      } else {
        action = action_cursor_move(-1, 0);
      }
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_MOVE_DOWN)) {
      action = action_cursor_move(1, 0);
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_MOVE_LEFT)) {
      /* At leftmost column with sidebar visible - focus sidebar */
      if (state->cursor_col == 0 && state->sidebar_visible) {
        action = action_sidebar_focus();
      } else {
        action = action_cursor_move(0, -1);
      }
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_MOVE_RIGHT)) {
      action = action_cursor_move(0, 1);
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_PAGE_UP)) {
      action = action_page_up();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_PAGE_DOWN)) {
      action = action_page_down();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_FIRST_COL)) {
      action = action_column_first();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_LAST_COL)) {
      action = action_column_last();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_FIRST_ROW)) {
      action = action_home();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_LAST_ROW)) {
      action = action_end();
    }
    /* ========== Editing ========== */
    else if (hotkey_matches(state->app->config, &event, HOTKEY_EDIT_INLINE)) {
      action = action_edit_start();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_EDIT_MODAL)) {
      action = action_edit_start_modal();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_SET_NULL)) {
      action = action_cell_set_null();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_SET_EMPTY)) {
      action = action_cell_set_empty();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_DELETE_ROW)) {
      action = action_row_delete();
    }
    /* ========== Row Selection ========== */
    else if (hotkey_matches(state->app->config, &event, HOTKEY_TOGGLE_SELECTION) &&
             !state->sidebar_focused && !state->filters_focused) {
      action = action_row_toggle_select();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_CLEAR_SELECTIONS) &&
               !state->sidebar_focused && !state->filters_focused) {
      Tab *tab = TUI_TAB(state);
      if (tab && tab->num_selected > 0) {
        action = action_rows_clear_select();
      }
    }
    /* ========== Workspaces ========== */
    else if (hotkey_matches(state->app->config, &event, HOTKEY_OPEN_QUERY)) {
      workspace_create_query(state);
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_NEXT_TAB)) {
      action = action_tab_next();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_PREV_TAB)) {
      action = action_tab_prev();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_NEXT_WORKSPACE)) {
      action = action_workspace_next();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_PREV_WORKSPACE)) {
      action = action_workspace_prev();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_CLOSE_TAB)) {
      /* Close with confirmation for query tabs with content */
      Tab *close_tab = TUI_TAB(state);
      if (close_tab) {
        if (close_tab->type == TAB_TYPE_QUERY &&
            ((close_tab->query_text && close_tab->query_len > 0) ||
             close_tab->query_results)) {
          if (!tui_show_confirm_dialog(state,
                                       "Close query tab with unsaved content?")) {
            handled = false;
          } else {
            tab_close(state);
          }
        } else {
          tab_close(state);
        }
      }
    }
    /* ========== Sidebar ========== */
    else if (hotkey_matches(state->app->config, &event, HOTKEY_TOGGLE_SIDEBAR)) {
      /* If sidebar visible but not focused, focus it; otherwise toggle */
      if (state->sidebar_visible && !state->sidebar_focused) {
        action = action_sidebar_focus();
      } else {
        action = action_sidebar_toggle();
      }
    }
    /* ========== Filters ========== */
    else if (hotkey_matches(state->app->config, &event, HOTKEY_TOGGLE_FILTERS)) {
      /* If filters visible but not focused, focus them; otherwise toggle */
      if (state->filters_visible && !state->filters_focused) {
        action = action_filters_focus();
      } else {
        action = action_filters_toggle();
      }
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_FILTERS_SWITCH_FOCUS)) {
      if (state->filters_visible) {
        action = action_filters_focus();
      }
    }
    /* ========== UI Toggles ========== */
    else if (hotkey_matches(state->app->config, &event, HOTKEY_TOGGLE_HEADER)) {
      action = action_toggle_header();
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_TOGGLE_STATUS)) {
      action = action_toggle_status();
    }
    /* ========== Table Operations ========== */
    else if (hotkey_matches(state->app->config, &event, HOTKEY_REFRESH)) {
      /* Refresh table (only for table tabs, not query) */
      Tab *refresh_tab = TUI_TAB(state);
      if (refresh_tab && refresh_tab->type == TAB_TYPE_TABLE) {
        tui_refresh_table(state);
      }
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_CYCLE_SORT)) {
      /* Cycle sort on current column: not in list -> asc -> desc -> remove */
      Tab *sort_tab = TUI_TAB(state);
      if (sort_tab && sort_tab->type == TAB_TYPE_TABLE && sort_tab->schema) {
        size_t col = state->cursor_col;
        if (col < sort_tab->schema->num_columns) {
          /* Find if column is already in sort list */
          size_t existing_idx = SIZE_MAX;
          for (size_t i = 0; i < sort_tab->num_sort_entries; i++) {
            if (sort_tab->sort_entries[i].column == col) {
              existing_idx = i;
              break;
            }
          }

          if (existing_idx == SIZE_MAX) {
            /* Column not in list - add with ASC if room */
            if (sort_tab->num_sort_entries < MAX_SORT_COLUMNS) {
              sort_tab->sort_entries[sort_tab->num_sort_entries].column = col;
              sort_tab->sort_entries[sort_tab->num_sort_entries].direction = SORT_ASC;
              sort_tab->num_sort_entries++;
            }
          } else if (sort_tab->sort_entries[existing_idx].direction == SORT_ASC) {
            /* Was ascending -> descending */
            sort_tab->sort_entries[existing_idx].direction = SORT_DESC;
          } else {
            /* Was descending -> remove from list */
            for (size_t i = existing_idx; i < sort_tab->num_sort_entries - 1; i++) {
              sort_tab->sort_entries[i] = sort_tab->sort_entries[i + 1];
            }
            sort_tab->num_sort_entries--;
          }
          /* Reload table with new sort order */
          tui_refresh_table(state);
        }
      }
    }
    /* ========== Dialogs (handled directly by TUI) ========== */
    else if (hotkey_matches(state->app->config, &event, HOTKEY_SHOW_SCHEMA)) {
      tui_show_schema(state);
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_GOTO_ROW)) {
      tui_show_goto_dialog(state);
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_CONNECT_DIALOG)) {
      tui_show_connect_dialog(state);
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_TOGGLE_HISTORY)) {
      tui_show_history_dialog(state);
      tui_refresh(state);
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_HELP)) {
      /* Help opens config dialog on hotkeys tab */
      config_view_show_tab(state, CONFIG_TAB_HOTKEYS);
      tui_refresh(state);
    } else if (hotkey_matches(state->app->config, &event, HOTKEY_CONFIG)) {
      tui_show_config(state);
    }
    /* ========== Unhandled ========== */
    else {
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

      /* Sync Tab with TuiState cursor/scroll after action
       * This handles both:
       * - Callback-based actions that modified TuiState (sync to Tab)
       * - Core actions that modified Tab (sync to TuiState)
       * We use Tab as authority for core actions, TuiState for callbacks.
       * Navigation callbacks now update Tab directly, so Tab is always current.
       */
      if (changes & (CHANGED_CURSOR | CHANGED_SCROLL)) {
        Tab *tab = app_current_tab(state->app);
        if (tab) {
          /* Sync TuiState to Tab (for callback-based changes) */
          tab->cursor_row = state->cursor_row;
          tab->cursor_col = state->cursor_col;
          tab->scroll_row = state->scroll_row;
          tab->scroll_col = state->scroll_col;
        }
      }
    }

    tui_refresh(state);
  }
}
