/*
 * lace - Database Viewer and Manager
 * TUI core implementation
 */

#include "tui_internal.h"
#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/*
 * Translate keyboard input from non-Latin layouts to Latin equivalents
 * based on physical key position. This allows hotkeys to work regardless
 * of the active keyboard layout (e.g., Russian, Ukrainian, etc.)
 */
int tui_translate_key(int ch) {
  /* Russian layout (ЙЦУКЕН) -> QWERTY mapping */
  switch (ch) {
  /* Lowercase Cyrillic */
  case 0x0439:
    return 'q'; /* й */
  case 0x0446:
    return 'w'; /* ц */
  case 0x0443:
    return 'e'; /* у */
  case 0x043A:
    return 'r'; /* к */
  case 0x0435:
    return 't'; /* е */
  case 0x043D:
    return 'y'; /* н */
  case 0x0433:
    return 'u'; /* г */
  case 0x0448:
    return 'i'; /* ш */
  case 0x0449:
    return 'o'; /* щ */
  case 0x0437:
    return 'p'; /* з */
  case 0x0445:
    return '['; /* х */
  case 0x044A:
    return ']'; /* ъ */
  case 0x0444:
    return 'a'; /* ф */
  case 0x044B:
    return 's'; /* ы */
  case 0x0432:
    return 'd'; /* в */
  case 0x0430:
    return 'f'; /* а */
  case 0x043F:
    return 'g'; /* п */
  case 0x0440:
    return 'h'; /* р */
  case 0x043E:
    return 'j'; /* о */
  case 0x043B:
    return 'k'; /* л */
  case 0x0434:
    return 'l'; /* д */
  case 0x044F:
    return 'z'; /* я */
  case 0x0447:
    return 'x'; /* ч */
  case 0x0441:
    return 'c'; /* с */
  case 0x043C:
    return 'v'; /* м */
  case 0x0438:
    return 'b'; /* и */
  case 0x0442:
    return 'n'; /* т */
  case 0x044C:
    return 'm'; /* ь */
  case 0x0436:
    return ';'; /* ж */
  case 0x044D:
    return '\''; /* э */
  case 0x0451:
    return '`'; /* ё */

  /* Uppercase Cyrillic */
  case 0x0419:
    return 'Q'; /* Й */
  case 0x0426:
    return 'W'; /* Ц */
  case 0x0423:
    return 'E'; /* У */
  case 0x041A:
    return 'R'; /* К */
  case 0x0415:
    return 'T'; /* Е */
  case 0x041D:
    return 'Y'; /* Н */
  case 0x0413:
    return 'U'; /* Г */
  case 0x0428:
    return 'I'; /* Ш */
  case 0x0429:
    return 'O'; /* Щ */
  case 0x0417:
    return 'P'; /* З */
  case 0x0425:
    return '{'; /* Х */
  case 0x042A:
    return '}'; /* Ъ */
  case 0x0424:
    return 'A'; /* Ф */
  case 0x042B:
    return 'S'; /* Ы */
  case 0x0412:
    return 'D'; /* В */
  case 0x0410:
    return 'F'; /* А */
  case 0x041F:
    return 'G'; /* П */
  case 0x0420:
    return 'H'; /* Р */
  case 0x041E:
    return 'J'; /* О */
  case 0x041B:
    return 'K'; /* Л */
  case 0x0414:
    return 'L'; /* Д */
  case 0x042F:
    return 'Z'; /* Я */
  case 0x0427:
    return 'X'; /* Ч */
  case 0x0421:
    return 'C'; /* С */
  case 0x041C:
    return 'V'; /* М */
  case 0x0418:
    return 'B'; /* И */
  case 0x0422:
    return 'N'; /* Т */
  case 0x042C:
    return 'M'; /* Ь */
  case 0x0416:
    return ':'; /* Ж */
  case 0x042D:
    return '"'; /* Э */
  case 0x0401:
    return '~'; /* Ё */

  default:
    return ch;
  }
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

bool tui_init(TuiState *state) {
  if (!state)
    return false;

  memset(state, 0, sizeof(TuiState));

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
    init_pair(COLOR_HEADER, COLOR_WHITE, COLOR_BLUE);
    init_pair(COLOR_SELECTED, COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_STATUS, COLOR_WHITE, COLOR_BLUE);
    init_pair(COLOR_ERROR, COLOR_WHITE, COLOR_RED);
    init_pair(COLOR_BORDER, COLOR_CYAN, -1);
    init_pair(COLOR_TITLE, COLOR_YELLOW, -1);
    init_pair(COLOR_NULL, COLOR_MAGENTA, -1);
    init_pair(COLOR_NUMBER, COLOR_CYAN, -1);
    init_pair(COLOR_EDIT, COLOR_BLACK, COLOR_YELLOW);
  }

  /* Get terminal dimensions */
  getmaxyx(stdscr, state->term_rows, state->term_cols);

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
  scrollok(state->main_win, FALSE);
  keypad(state->main_win, TRUE);

  state->running = true;
  state->sidebar_visible = false;
  state->sidebar_focused = false;

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

  /* Resize header and status */
  wresize(state->header_win, 1, state->term_cols);
  wresize(state->status_win, 1, state->term_cols);
  mvwin(state->status_win, state->term_rows - 1, 0);

  /* Create tab bar */
  state->tab_win = newwin(TAB_BAR_HEIGHT, state->term_cols, 1, 0);

  /* Calculate main window dimensions */
  int main_start_y = 2; /* After header + tab bar */
  int main_height = state->term_rows - 3;
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
  if (!state || !connstr)
    return false;

  char *err = NULL;
  state->conn = db_connect(connstr, &err);

  if (!state->conn) {
    tui_set_error(state, "Connection failed: %s", err ? err : "Unknown error");
    free(err);
    return false;
  }

  /* Show sidebar on successful connection */
  if (!state->sidebar_visible) {
    state->sidebar_visible = true;
    tui_recreate_windows(state);
    tui_calculate_column_widths(state);
  }

  tui_set_status(state, "Connected to %s", state->conn->database);
  return tui_load_tables(state);
}

void tui_disconnect(TuiState *state) {
  if (!state)
    return;

  /* Clean up all workspaces first (they own data/schema/col_widths) */
  for (size_t i = 0; i < MAX_WORKSPACES; i++) {
    Workspace *ws = &state->workspaces[i];
    if (ws->active) {
      free(ws->table_name);
      db_result_free(ws->data);
      db_schema_free(ws->schema);
      free(ws->col_widths);
      memset(ws, 0, sizeof(Workspace));
    }
  }
  state->num_workspaces = 0;
  state->current_workspace = 0;

  /* Clear convenience pointers (data was freed with workspaces) */
  state->data = NULL;
  state->schema = NULL;
  state->col_widths = NULL;
  state->num_col_widths = 0;

  if (state->tables) {
    for (size_t i = 0; i < state->num_tables; i++) {
      free(state->tables[i]);
    }
    free(state->tables);
    state->tables = NULL;
    state->num_tables = 0;
  }

  if (state->conn) {
    db_disconnect(state->conn);
    state->conn = NULL;
  }
}

bool tui_load_tables(TuiState *state) {
  if (!state || !state->conn)
    return false;

  /* Free old tables */
  if (state->tables) {
    for (size_t i = 0; i < state->num_tables; i++) {
      free(state->tables[i]);
    }
    free(state->tables);
  }

  char *err = NULL;
  state->tables = db_list_tables(state->conn, &state->num_tables, &err);

  if (!state->tables) {
    tui_set_error(state, "Failed to list tables: %s",
                  err ? err : "Unknown error");
    free(err);
    return false;
  }

  state->current_table = 0;
  state->sidebar_highlight = 0;

  if (state->num_tables == 0) {
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
  tui_draw_table(state);
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

    /* Handle timeout - update animations */
    if (ch == ERR) {
      tui_update_sidebar_scroll_animation(state);
      tui_draw_sidebar(state);
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

    /* Handle sidebar filter input (no translation - user is typing filter text)
     */
    if (state->sidebar_focused && state->sidebar_filter_active) {
      if (tui_handle_sidebar_input(state, ch)) {
        tui_refresh(state);
        continue;
      }
    }

    /* Translate non-Latin keyboard layouts for navigation hotkeys */
    ch = tui_translate_key(ch);

    /* Handle sidebar navigation if focused */
    if (state->sidebar_focused && tui_handle_sidebar_input(state, ch)) {
      tui_refresh(state);
      continue;
    }

    switch (ch) {
    case 'q':
    case 'Q':
    case 17:        /* Ctrl+Q - universal quit */
    case KEY_F(10): /* F10 - universal quit */
      state->running = false;
      break;

    case '\n':
    case KEY_ENTER:
      /* Start editing current cell */
      if (!state->sidebar_focused && state->data && state->data->num_rows > 0) {
        tui_start_edit(state);
      }
      break;

    case 'e':      /* 'e' - always open modal editor */
    case KEY_F(4): /* F4 - universal modal edit */
      if (!state->sidebar_focused && state->data && state->data->num_rows > 0) {
        tui_start_modal_edit(state);
      }
      break;

    case 14: /* Ctrl+N - set selected cell to NULL */
    case 'n':
      if (!state->sidebar_focused && state->data && state->data->num_rows > 0) {
        tui_set_cell_direct(state, true);
      }
      break;

    case 4: /* Ctrl+D - set selected cell to empty string */
    case 'd':
      if (!state->sidebar_focused && state->data && state->data->num_rows > 0) {
        tui_set_cell_direct(state, false);
      }
      break;

    case 'x':    /* Delete row */
    case KEY_DC: /* Delete key */
      if (!state->sidebar_focused && state->data && state->data->num_rows > 0) {
        tui_delete_row(state);
      }
      break;

    case KEY_UP:
    case 'k':
      tui_move_cursor(state, -1, 0);
      break;

    case KEY_DOWN:
    case 'j':
      tui_move_cursor(state, 1, 0);
      break;

    case KEY_LEFT:
    case 'h':
      /* If at leftmost column and sidebar is visible, focus sidebar */
      if (state->cursor_col == 0 && state->sidebar_visible) {
        state->sidebar_focused = true;
        state->sidebar_highlight =
            tui_get_sidebar_highlight_for_table(state, state->current_table);
      } else {
        tui_move_cursor(state, 0, -1);
      }
      break;

    case KEY_RIGHT:
    case 'l':
      tui_move_cursor(state, 0, 1);
      break;

    case KEY_PPAGE:
      tui_page_up(state);
      break;

    case KEY_NPAGE:
      tui_page_down(state);
      break;

    case KEY_HOME:
      /* Move to first column */
      state->cursor_col = 0;
      state->scroll_col = 0;
      break;

    case KEY_END:
      /* Move to last column */
      if (state->schema) {
        state->cursor_col =
            state->schema->num_columns > 0 ? state->schema->num_columns - 1 : 0;
      }
      break;

    case KEY_F(61): /* Ctrl+Home */
    case 'g':
    case 'a':
      tui_home(state);
      break;

    case KEY_F(62): /* Ctrl+End */
    case 'G':
    case 'z':
      tui_end(state);
      break;

    case ']':
    case KEY_F(6): /* F6 - next tab */
      if (state->num_workspaces > 1) {
        size_t next = (state->current_workspace + 1) % state->num_workspaces;
        workspace_switch(state, next);
      }
      break;

    case '[':
    case KEY_F(7): /* F7 - previous tab */
      if (state->num_workspaces > 1) {
        size_t prev = state->current_workspace > 0
                          ? state->current_workspace - 1
                          : state->num_workspaces - 1;
        workspace_switch(state, prev);
      }
      break;

    case '-':
    case '_':
      /* Close current tab */
      if (state->num_workspaces > 0) {
        workspace_close(state);
      }
      break;

    case 's':
    case 'S':
    case KEY_F(3): /* F3 - universal schema */
      tui_show_schema(state);
      break;

    case '/':
    case 7:        /* Ctrl+G - universal go to line */
    case KEY_F(5): /* F5 - universal go to row */
      tui_show_goto_dialog(state);
      break;

    case 'c':
    case 'C':
    case KEY_F(2): /* F2 - universal connect */
      tui_show_connect_dialog(state);
      break;

    case 't':
    case 'T':
    case KEY_F(9): /* F9 - universal toggle sidebar */
      /* Toggle sidebar */
      if (state->sidebar_visible) {
        /* Hide sidebar */
        state->sidebar_visible = false;
        state->sidebar_focused = false;
      } else {
        /* Show and focus sidebar */
        state->sidebar_visible = true;
        state->sidebar_focused = true;
        state->sidebar_highlight =
            tui_get_sidebar_highlight_for_table(state, state->current_table);
        state->sidebar_scroll = 0;
      }
      tui_recreate_windows(state);
      tui_calculate_column_widths(state);
      break;

    case '?':
    case KEY_F(1):
      tui_show_help(state);
      break;

    case KEY_RESIZE:
      /* Handle terminal resize */
      getmaxyx(stdscr, state->term_rows, state->term_cols);
      wresize(state->header_win, 1, state->term_cols);
      wresize(state->status_win, 1, state->term_cols);
      mvwin(state->status_win, state->term_rows - 1, 0);
      state->content_rows = state->term_rows - 4;
      tui_recreate_windows(state);
      tui_calculate_column_widths(state);
      break;

    default:
      break;
    }

    tui_refresh(state);
  }
}
