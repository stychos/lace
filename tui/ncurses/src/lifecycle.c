/*
 * Lace
 * TUI lifecycle: per-tab UI state, init, cleanup, window layout
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
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


/* ============================================================================
 * UITabState Dynamic Array Management
 * ============================================================================
 */

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
        /* Free resources */
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

bool tui_init(TuiState *state, AppState *app) {
  if (!state || !app)
    return false;

  memset(state, 0, sizeof(TuiState));
  state->app = app;

  /* vm_table is created on-demand when tabs are accessed */

  /* Set locale for UTF-8 support */
  setlocale(LC_ALL, "");

  /* Initialize ncurses */
  initscr();
  /* ncurses waits ESCDELAY (default 1000 ms) after Esc for an escape
   * sequence, which made every Esc lag a second. Respect a user setting. */
  if (!getenv("ESCDELAY")) {
    set_escdelay(25);
  }
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  /* Ctrl+S and Ctrl+Q are keys here, not terminal flow control (with IXON
   * on, Ctrl+S froze the output and never reached us). endwin() restores
   * the modes initscr() saved. */
  struct termios tio;
  if (tcgetattr(STDIN_FILENO, &tio) == 0) {
    tio.c_iflag &= ~(tcflag_t)IXON;
    tcsetattr(STDIN_FILENO, TCSANOW, &tio);
    def_prog_mode(); /* Kept as the program mode across suspend/resume */
  }
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

  /* Clear the standard screen to ensure no garbage is visible */
  clear();
  refresh();

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

  /* Clean up add-row state if active (just free memory, don't do UI stuff) */
  if (state->adding_row) {
    if (state->new_row_values) {
      for (size_t i = 0; i < state->new_row_num_cols; i++) {
        db_value_free(&state->new_row_values[i]);
      }
      free(state->new_row_values);
      state->new_row_values = NULL;
    }
    free(state->new_row_placeholders);
    state->new_row_placeholders = NULL;
    free(state->new_row_auto_increment);
    state->new_row_auto_increment = NULL;
    free(state->new_row_edited);
    state->new_row_edited = NULL;
    free(state->new_row_edit_buffer);
    state->new_row_edit_buffer = NULL;
    state->adding_row = false;
  }

  tui_disconnect(state);

  /* Cleanup ViewModels */
  table_vm_destroy(state->vm_table);
  state->vm_table = NULL;

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

  /* Final cleanup of app state - tui_disconnect reinits app state for continued
   * use, but on quit we need to free those allocations */
  if (state->app) {
    app_state_cleanup(state->app);
  }
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
    wtimeout(state->main_win, 80); /* The main loop's tick */
  }
  /* A terminal below the minimum size gets no windows (they would not fit):
   * the loop then reads from stdscr, which must tick too, not spin */
  keypad(stdscr, TRUE);
  wtimeout(stdscr, 80);

  /* Update content dimensions */
  state->content_cols = main_width - 2;
}
