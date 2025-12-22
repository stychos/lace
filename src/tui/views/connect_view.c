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
                        const char *error_msg, int selected) {
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
  mvwprintw(win, y++, 4, "mariadb://user:pass@host/db");
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

  /* Buttons */
  int btn_y = height - 2;
  int connect_x = width / 2 - 14;
  int cancel_x = width / 2 + 4;

  if (selected == 0) {
    wattron(win, A_REVERSE);
    mvwprintw(win, btn_y, connect_x, "[ Connect ]");
    wattroff(win, A_REVERSE);
    mvwprintw(win, btn_y, cancel_x, "[ Cancel ]");
  } else {
    mvwprintw(win, btn_y, connect_x, "[ Connect ]");
    wattron(win, A_REVERSE);
    mvwprintw(win, btn_y, cancel_x, "[ Cancel ]");
    wattroff(win, A_REVERSE);
  }

  /* Position cursor in input field */
  wmove(win, cursor_y, cursor_x);
  wrefresh(win);
}

char *connect_view_show(TuiState *state) {
  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

  int height = 15;
  int width = 50;
  if (width > term_cols - 4)
    width = term_cols - 4;
  if (height > term_rows - 4)
    height = term_rows - 4;

  int starty = (term_rows - height) / 2;
  int startx = (term_cols - width) / 2;

  WINDOW *dialog = newwin(height, width, starty, startx);
  if (!dialog)
    return NULL;

  keypad(dialog, TRUE);
  curs_set(1); /* Show cursor */

  InputField input;
  input_init(&input, width - 6);

  char *error_msg = NULL;
  char *result = NULL;
  int selected = 0; /* 0 = Connect, 1 = Cancel */

  bool running = true;
  while (running) {
    draw_dialog(dialog, height, width, &input, error_msg, selected);

    int ch = wgetch(dialog);

    free(error_msg);
    error_msg = NULL;

    switch (ch) {
    case '\t':
      selected = 1 - selected;
      break;

    case 27: /* Escape */
      running = false;
      break;

    case '\n':
    case KEY_ENTER:
      if (selected == 1) {
        /* Cancel button selected */
        running = false;
        break;
      }
      if (input.len > 0) {
        /* Determine the connection string to use */
        char *connstr_to_use = NULL;
        char *err = NULL;

        if (!strstr(input.buffer, "://")) {
          /* No scheme - try to detect SQLite file */
          connstr_to_use = connstr_from_path(input.buffer, &err);
          if (!connstr_to_use) {
            error_msg = err ? err : str_dup("Invalid file path");
            break;
          }
        } else {
          connstr_to_use = str_dup(input.buffer);
        }

        /* Try to connect */
        DbConnection *conn = db_connect(connstr_to_use, &err);
        if (conn) {
          db_disconnect(conn);
          result = connstr_to_use;
          running = false;
        } else {
          error_msg = err ? err : str_dup("Connection failed");
          free(connstr_to_use);
        }
      } else {
        error_msg = str_dup("Please enter a connection string or file path");
      }
      break;

    default:
      input_handle_key(&input, ch);
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
