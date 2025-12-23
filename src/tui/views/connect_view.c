/*
 * lace - Database Viewer and Manager
 * Connection dialog view implementation
 */

#include "connect_view.h"
#include "../../db/connstr.h"
#include "../../util/str.h"
#include <ctype.h>
#include <form.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONNSTR_LEN 512

/* Input field structure */
typedef struct {
  WINDOW *win;
  char buffer[MAX_CONNSTR_LEN];
  size_t len;
  size_t cursor;
  size_t scroll;
  int width;
} InputField;

/* Dialog focus states */
typedef enum {
  FOCUS_INPUT,      /* Connection string input */
  FOCUS_MODE,       /* Mode selection (after successful connection test) */
  FOCUS_BUTTONS     /* Connect/Cancel buttons */
} DialogFocus;

static void input_init(InputField *input, int width) {
  memset(input, 0, sizeof(InputField));
  input->width = width < 3 ? 3 : width; /* Minimum 3 for borders + 1 char */
}

static void input_draw(InputField *input, WINDOW *win, int y, int x,
                       int *cursor_y, int *cursor_x) {
  /* Calculate visible portion */
  size_t visible_start = input->scroll;
  size_t visible_len = input->width;

  /* Clear the input area */
  mvwhline(win, y, x, ' ', input->width);

  /* Draw text */
  size_t draw_len =
      (visible_start <= input->len) ? input->len - visible_start : 0;
  if (draw_len > visible_len)
    draw_len = visible_len;

  if (draw_len > 0) {
    mvwaddnstr(win, y, x, input->buffer + visible_start, draw_len);
  }

  /* Draw underline below input */
  mvwhline(win, y + 1, x, ACS_HLINE, input->width);

  /* Return cursor position for later */
  *cursor_y = y;
  *cursor_x = x + (int)(input->cursor - input->scroll);
}

static void input_handle_key(InputField *input, int ch) {
  switch (ch) {
  case KEY_LEFT:
    if (input->cursor > 0) {
      input->cursor--;
      if (input->cursor < input->scroll) {
        input->scroll = input->cursor;
      }
    }
    break;

  case KEY_RIGHT:
    if (input->cursor < input->len) {
      input->cursor++;
      if (input->cursor >= input->scroll + input->width - 2) {
        if (input->cursor > (size_t)(input->width - 3)) {
          input->scroll = input->cursor - input->width + 3;
        } else {
          input->scroll = 0;
        }
      }
    }
    break;

  case KEY_HOME:
  case 1: /* Ctrl+A */
    input->cursor = 0;
    input->scroll = 0;
    break;

  case KEY_END:
  case 5: /* Ctrl+E */
    input->cursor = input->len;
    if (input->cursor >= input->scroll + input->width - 2) {
      if (input->cursor > (size_t)(input->width - 3)) {
        input->scroll = input->cursor - input->width + 3;
      } else {
        input->scroll = 0;
      }
    }
    break;

  case KEY_BACKSPACE:
  case 127:
  case 8:
    if (input->cursor > 0 && input->cursor <= input->len) {
      memmove(input->buffer + input->cursor - 1, input->buffer + input->cursor,
              input->len - input->cursor + 1);
      input->cursor--;
      input->len--;
      if (input->cursor < input->scroll) {
        input->scroll = input->cursor;
      }
    }
    break;

  case KEY_DC: /* Delete */
  case 4:      /* Ctrl+D */
    if (input->cursor < input->len) {
      memmove(input->buffer + input->cursor, input->buffer + input->cursor + 1,
              input->len - input->cursor);
      input->len--;
    }
    break;

  case 21: /* Ctrl+U - clear line */
    input->buffer[0] = '\0';
    input->len = 0;
    input->cursor = 0;
    input->scroll = 0;
    break;

  case 11: /* Ctrl+K - clear to end */
    input->buffer[input->cursor] = '\0';
    input->len = input->cursor;
    break;

  default:
    if (ch >= 32 && ch < 127 && input->len < MAX_CONNSTR_LEN - 1 &&
        input->cursor <= input->len) {
      /* Insert character */
      memmove(input->buffer + input->cursor + 1, input->buffer + input->cursor,
              input->len - input->cursor + 1);
      input->buffer[input->cursor] = (char)ch;
      input->cursor++;
      input->len++;
      if (input->cursor >= input->scroll + input->width - 2) {
        /* Guard against underflow: ensure cursor > width - 3 before subtraction
         */
        if (input->cursor > (size_t)(input->width - 3)) {
          input->scroll = input->cursor - input->width + 3;
        } else {
          input->scroll = 0;
        }
      }
    }
    break;
  }
}

static void draw_dialog(WINDOW *win, int height, int width, InputField *input,
                        const char *error_msg, int selected_button,
                        bool has_existing_tabs, DialogFocus focus) {
  werase(win);
  box(win, 0, 0);

  /* Title */
  wattron(win, A_BOLD);
  mvwprintw(win, 0, (width - 22) / 2, " Connect to Database ");
  wattroff(win, A_BOLD);

  int y = 2;

  /* Examples */
  wattron(win, COLOR_PAIR(COLOR_NULL));
  mvwprintw(win, y++, 2, "Examples:");
  mvwprintw(win, y++, 4, "./database.db");
  mvwprintw(win, y++, 4, "sqlite:///path/to/database.db");
  mvwprintw(win, y++, 4, "pg://user:pass@host/db");
  mvwprintw(win, y++, 4, "mysql://user:pass@host/db");
  wattroff(win, COLOR_PAIR(COLOR_NULL));

  y++;

  /* Input field - get cursor position */
  int cursor_y, cursor_x;
  input_draw(input, win, y, 2, &cursor_y, &cursor_x);
  y += 3;

  /* Error message */
  if (error_msg && *error_msg) {
    wattron(win, COLOR_PAIR(COLOR_ERROR));
    mvwprintw(win, y++, 2, "Error: %s", error_msg);
    wattroff(win, COLOR_PAIR(COLOR_ERROR));
  }

  /* Buttons - layout depends on whether we have existing tabs */
  int btn_y = height - 2;
  bool btn_focused = (focus == FOCUS_BUTTONS);

  if (has_existing_tabs) {
    /* Three buttons: [Connect] [New Workspace] [Cancel] */
    int total_width = 11 + 17 + 10 + 4; /* buttons + spacing */
    int start_x = (width - total_width) / 2;

    /* Connect (in tab) */
    if (selected_button == 0) {
      if (btn_focused) wattron(win, A_REVERSE);
      mvwprintw(win, btn_y, start_x, "[ Connect ]");
      if (btn_focused) wattroff(win, A_REVERSE);
    } else {
      mvwprintw(win, btn_y, start_x, "[ Connect ]");
    }

    /* New Workspace */
    if (selected_button == 1) {
      if (btn_focused) wattron(win, A_REVERSE);
      mvwprintw(win, btn_y, start_x + 13, "[ New Workspace ]");
      if (btn_focused) wattroff(win, A_REVERSE);
    } else {
      mvwprintw(win, btn_y, start_x + 13, "[ New Workspace ]");
    }

    /* Cancel */
    if (selected_button == 2) {
      if (btn_focused) wattron(win, A_REVERSE);
      mvwprintw(win, btn_y, start_x + 32, "[ Cancel ]");
      if (btn_focused) wattroff(win, A_REVERSE);
    } else {
      mvwprintw(win, btn_y, start_x + 32, "[ Cancel ]");
    }
  } else {
    /* Two buttons: [Connect] [Cancel] */
    int connect_x = width / 2 - 12;
    int cancel_x = width / 2 + 2;

    if (selected_button == 0) {
      if (btn_focused) wattron(win, A_REVERSE);
      mvwprintw(win, btn_y, connect_x, "[ Connect ]");
      if (btn_focused) wattroff(win, A_REVERSE);
      mvwprintw(win, btn_y, cancel_x, "[ Cancel ]");
    } else {
      mvwprintw(win, btn_y, connect_x, "[ Connect ]");
      if (btn_focused) wattron(win, A_REVERSE);
      mvwprintw(win, btn_y, cancel_x, "[ Cancel ]");
      if (btn_focused) wattroff(win, A_REVERSE);
    }
  }

  /* Position cursor in input field when focused */
  if (focus == FOCUS_INPUT) {
    wmove(win, cursor_y, cursor_x);
  }
  wrefresh(win);
}

ConnectResult connect_view_show(TuiState *state) {
  ConnectResult result = {NULL, CONNECT_MODE_CANCELLED};

  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

  /* Check if we have an existing workspace with tabs */
  bool has_existing_workspace = false;
  if (state && state->app) {
    Workspace *ws = app_current_workspace(state->app);
    if (ws && ws->num_tabs > 0) {
      has_existing_workspace = true;
    }
  }

  int height = 15; /* Simplified - no mode options */
  int width = 50;
  if (width > term_cols - 4)
    width = term_cols - 4;
  if (height > term_rows - 4)
    height = term_rows - 4;

  int starty = (term_rows - height) / 2;
  int startx = (term_cols - width) / 2;

  WINDOW *dialog = newwin(height, width, starty, startx);
  if (!dialog)
    return result;

  keypad(dialog, TRUE);
  curs_set(1); /* Show cursor */

  InputField input;
  input_init(&input, width - 6);

  char *error_msg = NULL;
  int selected_button = 0; /* 0 = Connect, 1 = New Workspace (if shown), last = Cancel */
  int num_buttons = has_existing_workspace ? 3 : 2;
  int cancel_button = num_buttons - 1;
  DialogFocus focus = FOCUS_INPUT;

  bool running = true;
  while (running) {
    /* Show/hide cursor based on focus */
    curs_set(focus == FOCUS_INPUT ? 1 : 0);

    draw_dialog(dialog, height, width, &input, error_msg, selected_button,
                has_existing_workspace, focus);

    int ch = wgetch(dialog);

    free(error_msg);
    error_msg = NULL;

    /* Tab cycles through focus areas */
    if (ch == '\t') {
      focus = (focus == FOCUS_INPUT) ? FOCUS_BUTTONS : FOCUS_INPUT;
      continue;
    }

    /* Escape cancels */
    if (ch == 27) {
      running = false;
      continue;
    }

    /* Handle input based on focus */
    switch (focus) {
    case FOCUS_INPUT:
      switch (ch) {
      case '\n':
      case KEY_ENTER:
        /* Enter in input field = Connect (in tab if available) */
        if (input.len > 0) {
          char *connstr_to_use = NULL;
          char *err = NULL;

          if (!strstr(input.buffer, "://")) {
            connstr_to_use = connstr_from_path(input.buffer, &err);
            if (!connstr_to_use) {
              error_msg = err ? err : str_dup("Invalid file path");
              break;
            }
          } else {
            connstr_to_use = str_dup(input.buffer);
          }

          DbConnection *conn = db_connect(connstr_to_use, &err);
          if (conn) {
            db_disconnect(conn);
            result.connstr = connstr_to_use;
            /* Default action: new tab if tabs exist, otherwise new workspace */
            result.mode = has_existing_workspace ? CONNECT_MODE_NEW_TAB
                                                 : CONNECT_MODE_NEW_WORKSPACE;
            running = false;
          } else {
            error_msg = err ? err : str_dup("Connection failed");
            free(connstr_to_use);
          }
        } else {
          error_msg = str_dup("Please enter a connection string or file path");
        }
        break;

      case KEY_DOWN:
        focus = FOCUS_BUTTONS;
        break;

      default:
        input_handle_key(&input, ch);
        break;
      }
      break;

    case FOCUS_MODE:
      /* Not used anymore - fall through to buttons */
      focus = FOCUS_BUTTONS;
      break;

    case FOCUS_BUTTONS:
      switch (ch) {
      case KEY_LEFT:
      case 'h':
        if (selected_button > 0)
          selected_button--;
        break;

      case KEY_RIGHT:
      case 'l':
        if (selected_button < num_buttons - 1)
          selected_button++;
        break;

      case KEY_UP:
        focus = FOCUS_INPUT;
        break;

      case '\n':
      case KEY_ENTER:
        if (selected_button == cancel_button) {
          /* Cancel */
          running = false;
        } else {
          /* Connect or New Workspace */
          if (input.len > 0) {
            char *connstr_to_use = NULL;
            char *err = NULL;

            if (!strstr(input.buffer, "://")) {
              connstr_to_use = connstr_from_path(input.buffer, &err);
              if (!connstr_to_use) {
                error_msg = err ? err : str_dup("Invalid file path");
                focus = FOCUS_INPUT;
                break;
              }
            } else {
              connstr_to_use = str_dup(input.buffer);
            }

            DbConnection *conn = db_connect(connstr_to_use, &err);
            if (conn) {
              db_disconnect(conn);
              result.connstr = connstr_to_use;
              /* Button 0 = Connect (new tab), Button 1 = New Workspace */
              if (has_existing_workspace && selected_button == 1) {
                result.mode = CONNECT_MODE_NEW_WORKSPACE;
              } else if (has_existing_workspace) {
                result.mode = CONNECT_MODE_NEW_TAB;
              } else {
                result.mode = CONNECT_MODE_NEW_WORKSPACE;
              }
              running = false;
            } else {
              error_msg = err ? err : str_dup("Connection failed");
              free(connstr_to_use);
              focus = FOCUS_INPUT;
            }
          } else {
            error_msg = str_dup("Please enter a connection string or file path");
            focus = FOCUS_INPUT;
          }
        }
        break;

      default:
        break;
      }
      break;
    }
  }

  curs_set(0); /* Hide cursor */
  delwin(dialog);
  free(error_msg);

  /* Redraw main screen */
  touchwin(stdscr);
  if (state) {
    tui_refresh(state);
  }

  return result;
}

char *connect_view_recent(TuiState *state) {
  /* TODO: Implement recent connections list */
  (void)state;
  return NULL;
}
