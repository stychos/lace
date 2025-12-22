/*
 * lace - Database Viewer and Manager
 * TUI core implementation
 */

#include "tui_internal.h"
#include "../core/actions.h"
#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>


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

  /* Sync connection and table list */
  state->conn = app->conn;
  state->tables = app->tables;
  state->num_tables = app->num_tables;

  /* Sync workspaces */
  state->workspaces = app->workspaces;
  state->num_workspaces = app->num_workspaces;
  state->current_workspace = app->current_workspace;

  /* Sync from current workspace */
  Workspace *ws = app_current_workspace(app);
  if (ws) {
    state->current_table = ws->table_index;
    state->data = ws->data;
    state->schema = ws->schema;
    state->cursor_row = ws->cursor_row;
    state->cursor_col = ws->cursor_col;
    state->scroll_row = ws->scroll_row;
    state->scroll_col = ws->scroll_col;
    state->total_rows = ws->total_rows;
    state->loaded_offset = ws->loaded_offset;
    state->loaded_count = ws->loaded_count;
    state->col_widths = ws->col_widths;
    state->num_col_widths = ws->num_col_widths;
    state->filters_visible = ws->filters_visible;
    state->filters_focused = ws->filters_focused;
    state->filters_cursor_row = ws->filters_cursor_row;
    state->filters_cursor_col = ws->filters_cursor_col;
    state->filters_scroll = ws->filters_scroll;

    /* Sync sidebar state */
    state->sidebar_visible = ws->sidebar_visible;
    state->sidebar_focused = ws->sidebar_focused;
    state->sidebar_highlight = ws->sidebar_highlight;
    state->sidebar_scroll = ws->sidebar_scroll;
    memcpy(state->sidebar_filter, ws->sidebar_filter, sizeof(state->sidebar_filter));
    state->sidebar_filter_len = ws->sidebar_filter_len;
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
    state->col_widths = NULL;
    state->num_col_widths = 0;
    state->filters_visible = false;
    state->filters_focused = false;
    state->filters_cursor_row = 0;
    state->filters_cursor_col = 0;
    state->filters_scroll = 0;

    /* Default sidebar state */
    state->sidebar_visible = false;
    state->sidebar_focused = false;
    state->sidebar_highlight = 0;
    state->sidebar_scroll = 0;
    state->sidebar_filter[0] = '\0';
    state->sidebar_filter_len = 0;
  }

  state->page_size = app->page_size;
}

/* Sync current workspace from view cache - call before workspace switch */
void tui_sync_to_workspace(TuiState *state) {
  if (!state || !state->app)
    return;

  Workspace *ws = app_current_workspace(state->app);
  if (!ws)
    return;

  /* Save UI state back to workspace */
  ws->cursor_row = state->cursor_row;
  ws->cursor_col = state->cursor_col;
  ws->scroll_row = state->scroll_row;
  ws->scroll_col = state->scroll_col;
  ws->filters_visible = state->filters_visible;
  ws->filters_focused = state->filters_focused;
  ws->filters_cursor_row = state->filters_cursor_row;
  ws->filters_cursor_col = state->filters_cursor_col;
  ws->filters_scroll = state->filters_scroll;

  /* Save sidebar state back to workspace */
  ws->sidebar_visible = state->sidebar_visible;
  ws->sidebar_focused = state->sidebar_focused;
  ws->sidebar_highlight = state->sidebar_highlight;
  ws->sidebar_scroll = state->sidebar_scroll;
  memcpy(ws->sidebar_filter, state->sidebar_filter, sizeof(ws->sidebar_filter));
  ws->sidebar_filter_len = state->sidebar_filter_len;
}

bool tui_init(TuiState *state, AppState *app) {
  if (!state || !app)
    return false;

  memset(state, 0, sizeof(TuiState));
  state->app = app;

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
  state->header_visible = true;
  state->sidebar_visible = false;
  state->sidebar_focused = false;
  state->status_visible = true;

  return true;
}

void tui_cleanup(TuiState *state) {
  if (!state)
    return;

  tui_disconnect(state);

  /* Free status message */
  free(state->status_msg);

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

  state->app->conn = conn;

  /* Show sidebar on successful connection */
  if (!state->sidebar_visible) {
    state->sidebar_visible = true;
    tui_recreate_windows(state);
    tui_calculate_column_widths(state);
  }

  tui_set_status(state, "Connected to %s", state->app->conn->database);
  return tui_load_tables(state);
}

void tui_disconnect(TuiState *state) {
  if (!state || !state->app)
    return;

  /* Cleanup is handled by app_state_cleanup() */
  app_state_cleanup(state->app);
  app_state_init(state->app);

  /* Hide sidebar when disconnected */
  if (state->sidebar_visible) {
    state->sidebar_visible = false;
    state->sidebar_focused = false;
    tui_recreate_windows(state);
  }

  /* Clear cached state */
  state->conn = NULL;
  state->tables = NULL;
  state->num_tables = 0;
  state->data = NULL;
  state->schema = NULL;
}

bool tui_load_tables(TuiState *state) {
  if (!state || !state->app || !state->app->conn)
    return false;

  AppState *app = state->app;

  /* Free old tables */
  if (app->tables) {
    for (size_t i = 0; i < app->num_tables; i++) {
      free(app->tables[i]);
    }
    free(app->tables);
    app->tables = NULL;
    app->num_tables = 0;
  }

  /* Sync conn for async operation */
  state->conn = app->conn;

  /* Use async operation with progress dialog */
  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_LIST_TABLES;
  op.conn = app->conn;

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

  app->tables = (char **)op.result;
  app->num_tables = op.result_count;
  async_free(&op);

  /* Sync tables to TUI state so workspace_create can access them */
  state->tables = app->tables;
  state->num_tables = app->num_tables;

  state->sidebar_highlight = 0;

  if (app->num_tables == 0) {
    tui_set_status(state, "No tables found");
    state->sidebar_focused = true;
  } else {
    /* Auto-load first table */
    workspace_create(state, 0);
    state->sidebar_focused = false;
  }

  return true;
}

void tui_refresh(TuiState *state) {
  tui_draw_header(state);
  tui_draw_tabs(state);
  tui_draw_sidebar(state);

  /* Dispatch drawing based on workspace type */
  Workspace *ws = TUI_WORKSPACE(state);
  if (ws) {
    if (ws->type == WORKSPACE_TYPE_QUERY) {
      tui_draw_query(state);
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

  while (state->running) {
    /* Get input from appropriate window */
    WINDOW *input_win = state->sidebar_focused && state->sidebar_win
                            ? state->sidebar_win
                            : state->main_win;
    int ch = wgetch(input_win);

    /* Handle timeout - update animations and background operations */
    if (ch == ERR) {
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
    if (ch == KEY_MOUSE) {
      if (tui_handle_mouse_event(state)) {
        tui_refresh(state);
      }
      continue;
    }

    /* Handle edit mode input (no key translation - user is typing) */
    if (state->editing && tui_handle_edit_input(state, ch)) {
      tui_refresh(state);
      continue;
    }

    /* Handle query tab input (no key translation - user is typing SQL) */
    if (state->num_workspaces > 0 && !state->sidebar_focused) {
      Workspace *ws = &state->workspaces[state->current_workspace];
      if (ws->type == WORKSPACE_TYPE_QUERY) {
        if (tui_handle_query_input(state, ch)) {
          tui_refresh(state);
          continue;
        }
      }
    }

    /* Handle sidebar filter input (no translation - user is typing filter text)
     */
    if (state->sidebar_focused && state->sidebar_filter_active) {
      if (tui_handle_sidebar_input(state, ch)) {
        tui_refresh(state);
        continue;
      }
    }

    /* Handle sidebar navigation if focused */
    if (state->sidebar_focused && tui_handle_sidebar_input(state, ch)) {
      tui_refresh(state);
      continue;
    }

    /* Handle filters panel input if visible */
    if (state->filters_visible && tui_handle_filters_input(state, ch)) {
      tui_refresh(state);
      continue;
    }

    /* Dispatch actions based on key input */
    Action action = {0};
    bool handled = true;

    switch (ch) {
    /* ========== Application ========== */
    case 'q':
    case 'Q':
    case 24:        /* Ctrl+X */
    case KEY_F(10):
      /* Quit with confirmation if connected */
      if (!state->conn || tui_show_confirm_dialog(state, "Quit application?")) {
        action = action_quit_force();
      }
      break;

    /* ========== Navigation ========== */
    case KEY_UP:
    case 'k':
      /* At first row with filters visible - focus filters */
      if (state->cursor_row == 0 && state->filters_visible) {
        action = action_filters_focus();
        state->filters_cursor_row =
            state->workspaces[state->current_workspace].filters.num_filters - 1;
        if (state->filters_cursor_row == (size_t)-1)
          state->filters_cursor_row = 0;
      } else {
        action = action_cursor_move(-1, 0);
      }
      break;

    case KEY_DOWN:
    case 'j':
      action = action_cursor_move(1, 0);
      break;

    case KEY_LEFT:
    case 'h':
      /* At leftmost column with sidebar visible - focus sidebar */
      if (state->cursor_col == 0 && state->sidebar_visible) {
        action = action_sidebar_focus();
      } else {
        action = action_cursor_move(0, -1);
      }
      break;

    case KEY_RIGHT:
    case 'l':
      action = action_cursor_move(0, 1);
      break;

    case KEY_PPAGE:
      action = action_page_up();
      break;

    case KEY_NPAGE:
      action = action_page_down();
      break;

    case KEY_HOME:
      action = action_column_first();
      break;

    case KEY_END:
      action = action_column_last();
      break;

    case KEY_F(61): /* Ctrl+Home */
    case 'a':
      action = action_home();
      break;

    case KEY_F(62): /* Ctrl+End */
    case 'z':
      action = action_end();
      break;

    /* ========== Editing ========== */
    case '\n':
    case KEY_ENTER:
      action = action_edit_start();
      break;

    case 'e':
    case KEY_F(4):
      action = action_edit_start_modal();
      break;

    case 14: /* Ctrl+N */
    case 'n':
      action = action_cell_set_null();
      break;

    case 4: /* Ctrl+D */
    case 'd':
      action = action_cell_set_empty();
      break;

    case 'x':
    case KEY_DC:
      action = action_row_delete();
      break;

    /* ========== Workspaces ========== */
    case 'p':
    case 'P':
      action = action_workspace_create_query();
      break;

    case ']':
    case KEY_F(6):
      action = action_workspace_next();
      break;

    case '[':
    case KEY_F(7):
      action = action_workspace_prev();
      break;

    case '-':
    case '_':
      /* Close with confirmation for query tabs with content */
      if (state->num_workspaces > 0) {
        Workspace *ws = &state->workspaces[state->current_workspace];
        if (ws->type == WORKSPACE_TYPE_QUERY &&
            ((ws->query_text && ws->query_len > 0) || ws->query_results)) {
          if (!tui_show_confirm_dialog(
                  state, "Close query tab with unsaved content?")) {
            handled = false;
            break;
          }
        }
        action = action_workspace_close();
      }
      break;

    /* ========== Sidebar ========== */
    case 't':
    case 'T':
    case KEY_F(9):
      action = action_sidebar_toggle();
      break;

    /* ========== Filters ========== */
    case '/':
    case 'f':
    case 'F':
      /* If filters visible but not focused, focus them; otherwise toggle */
      if (state->filters_visible && !state->filters_focused) {
        action = action_filters_focus();
      } else {
        action = action_filters_toggle();
      }
      break;

    case 23: /* Ctrl+W */
      if (state->filters_visible) {
        action = action_filters_focus();
      }
      break;

    /* ========== UI Toggles ========== */
    case 'm':
    case 'M':
      action = action_toggle_header();
      break;

    case 'b':
    case 'B':
      action = action_toggle_status();
      break;

    /* ========== Dialogs (handled directly by TUI) ========== */
    case 's':
    case 'S':
    case KEY_F(3):
      tui_show_schema(state);
      break;

    case 'g':
    case 'G':
    case 7: /* Ctrl+G */
    case KEY_F(5):
      tui_show_goto_dialog(state);
      break;

    case 'c':
    case 'C':
    case KEY_F(2):
      tui_show_connect_dialog(state);
      break;

    case '?':
    case KEY_F(1):
      tui_show_help(state);
      break;

    /* ========== Terminal Events ========== */
    case KEY_RESIZE:
      tui_recreate_windows(state);
      tui_calculate_column_widths(state);
      break;

    default:
      handled = false;
      break;
    }

    /* Dispatch action if one was created */
    if (handled && action.type != ACTION_NONE) {
      app_dispatch((struct TuiState *)state, &action);
    }

    tui_refresh(state);
  }
}
