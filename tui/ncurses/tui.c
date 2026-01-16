/*
 * Lace
 * TUI core implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/session.h"
#include "core/actions.h"
#include "util/mem.h"
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

/* ============================================================================
 * UITabState Dynamic Array Management
 * ============================================================================
 */

#define INITIAL_TAB_UI_WS_CAPACITY 4
#define INITIAL_TAB_UI_TAB_CAPACITY 8

bool tui_ensure_tab_ui_capacity(TuiState *state, size_t ws_idx,
                                size_t tab_idx) {
  if (!state)
    return false;

  /* Ensure workspace dimension capacity */
  if (ws_idx >= state->tab_ui_ws_capacity) {
    size_t new_ws_cap;
    if (state->tab_ui_ws_capacity == 0) {
      new_ws_cap = INITIAL_TAB_UI_WS_CAPACITY;
    } else if (state->tab_ui_ws_capacity > SIZE_MAX / 2) {
      return false; /* Would overflow */
    } else {
      new_ws_cap = state->tab_ui_ws_capacity * 2;
    }
    while (new_ws_cap <= ws_idx) {
      if (new_ws_cap > SIZE_MAX / 2)
        return false; /* Would overflow */
      new_ws_cap *= 2;
    }

    /* Check for overflow in allocation size calculations */
    if (new_ws_cap > SIZE_MAX / sizeof(UITabState *) ||
        new_ws_cap > SIZE_MAX / sizeof(size_t))
      return false;

    /* Use safe_malloc+memcpy+free for atomic all-or-nothing allocation */
    UITabState **new_tab_ui = safe_malloc(new_ws_cap * sizeof(UITabState *));
    size_t *new_capacity = safe_malloc(new_ws_cap * sizeof(size_t));

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
    size_t new_tab_cap;
    if (old_cap == 0) {
      new_tab_cap = INITIAL_TAB_UI_TAB_CAPACITY;
    } else if (old_cap > SIZE_MAX / 2) {
      return false; /* Would overflow */
    } else {
      new_tab_cap = old_cap * 2;
    }
    while (new_tab_cap <= tab_idx) {
      if (new_tab_cap > SIZE_MAX / 2)
        return false; /* Would overflow */
      new_tab_cap *= 2;
    }

    /* Check for overflow in allocation size */
    if (new_tab_cap > SIZE_MAX / sizeof(UITabState))
      return false;

    UITabState *new_tabs =
        safe_reallocarray(state->tab_ui[ws_idx], new_tab_cap, sizeof(UITabState));

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
        /* Clean up widgets for this tab */
        tui_cleanup_tab_widgets(&state->tab_ui[ws][tab]);
        /* Free legacy resources */
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
  char *result = safe_malloc(len + 1);

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

  /* Tab data fields (cursor, scroll, data, schema) are accessed directly via
   * TUI_TAB() and VmTable - no sync needed. Only sync TUI-specific UI state. */
  Tab *tab = app_current_tab(app);

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
  } else if (tab) {
    /* No UITabState but have tab - use defaults */
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
  } else {
    /* No tab - clear UI state */
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

  state->page_size = app->page_size;

  /* Bind ViewModels to current tab */
  Tab *current_tab = app_current_tab(app);
  if (current_tab) {
    /* Bind or create appropriate ViewModel based on tab type */
    if (current_tab->type == TAB_TYPE_TABLE) {
      if (!state->vm_table) {
        state->vm_table = vm_table_create(app, current_tab, NULL);
      } else {
        vm_table_bind(state->vm_table, current_tab);
      }
    }
    /* VmQuery removed - use QueryWidget instead */
  }

  /* Initialize/sync widgets for current tab (same logic as tab_restore) */
  if (ui && current_tab) {
    if (current_tab->type == TAB_TYPE_TABLE && !ui->table_widget) {
      tui_init_table_tab_widgets(state, ui, current_tab);
    } else if (current_tab->type == TAB_TYPE_TABLE && ui->table_widget) {
      table_widget_sync_from_tab(ui->table_widget);
    } else if (current_tab->type == TAB_TYPE_QUERY && !ui->query_widget) {
      tui_init_query_tab_widgets(state, ui, current_tab);
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

  /* Tab data fields (cursor, scroll, data, schema) are modified directly by
   * VmTable and navigation functions - no sync needed. Only sync UI state. */

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

static void ui_cell_copy(void *ctx) { tui_cell_copy((TuiState *)ctx); }

static void ui_cell_paste(void *ctx) { tui_cell_paste((TuiState *)ctx); }

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
  return tui_sidebar_visible(state);
}

static bool ui_is_sidebar_focused(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  return tui_sidebar_focused(state);
}

static void ui_set_sidebar_visible(void *ctx, bool visible) {
  TuiState *state = (TuiState *)ctx;
  tui_set_sidebar_visible(state, visible);
}

static void ui_set_sidebar_focused(void *ctx, bool focused) {
  TuiState *state = (TuiState *)ctx;
  tui_set_sidebar_focused(state, focused);
}

static size_t ui_get_sidebar_highlight(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  return tui_sidebar_highlight(state);
}

static void ui_set_sidebar_highlight(void *ctx, size_t highlight) {
  TuiState *state = (TuiState *)ctx;
  tui_set_sidebar_highlight(state, highlight);
}

static void ui_set_sidebar_scroll(void *ctx, size_t scroll) {
  TuiState *state = (TuiState *)ctx;
  tui_set_sidebar_scroll(state, scroll);
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
  return tui_filters_visible(state);
}

static bool ui_is_filters_focused(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  return tui_filters_focused(state);
}

static void ui_set_filters_visible(void *ctx, bool visible) {
  TuiState *state = (TuiState *)ctx;
  tui_set_filters_visible(state, visible);
}

static void ui_set_filters_focused(void *ctx, bool focused) {
  TuiState *state = (TuiState *)ctx;
  tui_set_filters_focused(state, focused);
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
      .cell_copy = ui_cell_copy,
      .cell_paste = ui_cell_paste,
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
  /* vm_table and vm_query are created on-demand when tabs are accessed */
  /* SidebarWidget is used instead of VmSidebar */

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

  /* Create render context for backend abstraction (wraps existing ncurses
   * session) */
  state->render_ctx = render_context_wrap_ncurses();

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

  /* Register windows with render backend for abstraction layer */
  if (state->render_ctx) {
    render_set_region_handle(state->render_ctx, UI_REGION_MAIN, state->main_win);
    render_set_region_handle(state->render_ctx, UI_REGION_HEADER,
                             state->header_win);
    render_set_region_handle(state->render_ctx, UI_REGION_STATUS,
                             state->status_win);
    render_set_region_handle(state->render_ctx, UI_REGION_TABS, state->tab_win);
    /* sidebar_win is NULL initially */
  }

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
    /* Sync widget state to Tab before saving (ensures cursor/scroll persisted) */
    tab_save(state);

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

  /* Free render context */
  free(state->render_ctx);
  state->render_ctx = NULL;

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

  /* Register windows with render backend for abstraction layer */
  if (state->render_ctx) {
    render_set_region_handle(state->render_ctx, UI_REGION_MAIN, state->main_win);
    render_set_region_handle(state->render_ctx, UI_REGION_HEADER,
                             state->header_win);
    render_set_region_handle(state->render_ctx, UI_REGION_STATUS,
                             state->status_win);
    render_set_region_handle(state->render_ctx, UI_REGION_SIDEBAR,
                             state->sidebar_win);
    render_set_region_handle(state->render_ctx, UI_REGION_TABS, state->tab_win);
  }
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
    Tab *tab = workspace_create_table_tab(ws, conn_index, table_idx,
                                          state->tables[table_idx]);
    if (tab) {
      tab->table_index = table_idx;

      /* Ensure UITabState capacity before accessing */
      size_t ws_idx = state->app->current_workspace;
      size_t tab_idx_ui = ws->current_tab;
      tui_ensure_tab_ui_capacity(state, ws_idx, tab_idx_ui);

      /* Load table data (updates Tab directly) */
      tui_load_table_data(state, state->tables[table_idx]);

      /* Initialize UI state */
      UITabState *ui = TUI_TAB_UI(state);
      if (ui) {
        ui->sidebar_visible = true;
        ui->sidebar_focused = false;

        /* Initialize widgets for the new table tab */
        tui_init_table_tab_widgets(state, ui, tab);
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

      /* Connection tab has no table data - Tab fields stay NULL */

      /* Sync workspace cache */
      state->workspaces = state->app->workspaces;
      state->num_workspaces = state->app->num_workspaces;
      state->current_workspace = state->app->current_workspace;

      tui_recreate_windows(state);

      if (state->num_tables == 0) {
        tui_set_status(state, "Connected to %s - No tables found",
                       conn->database);
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

  /* Clear connection cache (memory was freed by app_state_cleanup) */
  state->conn = NULL;
  state->tables = NULL;
  state->num_tables = 0;

  /* Clear TUI-specific UI state */
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
  /* Sync TableWidget from Tab before drawing (Tab is modified by pagination) */
  Tab *tab = TUI_TAB(state);
  TableWidget *widget = TUI_TABLE_WIDGET(state);
  if (widget && tab && tab->type == TAB_TYPE_TABLE) {
    table_widget_sync_from_tab(widget);
  }

  tui_draw_header(state);
  tui_draw_tabs(state);
  tui_draw_sidebar(state);

  /* Dispatch drawing based on tab type */
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
    {HOTKEY_SHOW_SCHEMA, tui_show_schema},
    {HOTKEY_GOTO_ROW, tui_show_goto_dialog},
    {HOTKEY_CONNECT_DIALOG, tui_show_connect_dialog},
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
  if (tui_cursor_row(state) == 0 && state->filters_visible) {
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
  bool focusing = state->filters_visible && !state->filters_focused;
  size_t table_col = tui_cursor_col(state);
  Tab *tab = TUI_TAB(state);

  Action action = focusing ? action_filters_focus() : action_filters_toggle();

  /* Smart filter positioning when opening or focusing from table */
  bool closing = (!focusing && state->filters_visible);
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
  if (state->filters_visible) {
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

static bool handle_cycle_sort(TuiState *state, Action *action) {
  Tab *tab = TUI_TAB(state);
  if (tab && tab->type == TAB_TYPE_TABLE && tab->schema) {
    size_t col = tab->cursor_col;
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

    /* Handle add-row mode input */
    if (state->adding_row && tui_handle_add_row_input(state, &event)) {
      tui_refresh(state);
      continue;
    }

    /* Try FocusManager routing first (for widgets that implement handle_event).
     * This is the future path - as widgets implement handle_event, they'll
     * consume events here. For now, most fall through to legacy handlers below.
     */
    UITabState *ui_tab = tui_current_tab_ui(state);
    if (ui_tab && focus_manager_route_event(&ui_tab->focus_mgr, &event)) {
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
