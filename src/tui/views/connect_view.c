/*
 * lace - Database Viewer and Manager
 * Connection dialog view implementation
 */

#include "connect_view.h"
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
  input->width = width;
}

static void input_draw(InputField *input, WINDOW *win, int y, int x) {
  /* Calculate visible portion */
  size_t visible_start = input->scroll;
  size_t visible_len = input->width - 2;

  /* Draw field background */
  wattron(win, A_REVERSE);
  mvwhline(win, y, x, ' ', input->width);

  /* Draw text */
  size_t draw_len = input->len - visible_start;
  if (draw_len > visible_len)
    draw_len = visible_len;

  mvwaddnstr(win, y, x + 1, input->buffer + visible_start, draw_len);

  /* Position cursor */
  size_t cursor_pos = input->cursor - input->scroll;
  wmove(win, y, x + 1 + cursor_pos);

  wattroff(win, A_REVERSE);
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
        input->scroll = input->cursor - input->width + 3;
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
      input->scroll = input->cursor - input->width + 3;
    }
    break;

  case KEY_BACKSPACE:
  case 127:
  case 8:
    if (input->cursor > 0) {
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
    if (ch >= 32 && ch < 127 && input->len < MAX_CONNSTR_LEN - 1) {
      /* Insert character */
      memmove(input->buffer + input->cursor + 1, input->buffer + input->cursor,
              input->len - input->cursor + 1);
      input->buffer[input->cursor] = (char)ch;
      input->cursor++;
      input->len++;
      if (input->cursor >= input->scroll + input->width - 2) {
        input->scroll = input->cursor - input->width + 3;
      }
    }
    break;
  }
}

static void draw_dialog(WINDOW *win, int height, int width, InputField *input,
                        int selected_driver, const char *error_msg) {
  werase(win);
  box(win, 0, 0);

  /* Title */
  wattron(win, A_BOLD);
  mvwprintw(win, 0, (width - 14) / 2, " Connect to Database ");
  wattroff(win, A_BOLD);

  int y = 2;

  /* Instructions */
  mvwprintw(win, y++, 2, "Enter connection string or select a driver:");
  y++;

  /* Driver options */
  const char *drivers[] = {"SQLite", "PostgreSQL", "MySQL"};
  const char *examples[] = {"sqlite:///path/to/database.db",
                            "postgres://user:pass@host:5432/db",
                            "mysql://user:pass@host:3306/db"};

  for (int i = 0; i < 3; i++) {
    if (i == selected_driver) {
      wattron(win, A_REVERSE);
    }
    mvwprintw(win, y, 4, "[%c] %s", i == selected_driver ? '*' : ' ',
              drivers[i]);
    if (i == selected_driver) {
      wattroff(win, A_REVERSE);
    }
    y++;
  }

  y++;

  /* Example */
  wattron(win, COLOR_PAIR(COLOR_NULL));
  mvwprintw(win, y++, 2, "Example: %s", examples[selected_driver]);
  wattroff(win, COLOR_PAIR(COLOR_NULL));

  y++;

  /* Connection string label */
  mvwprintw(win, y++, 2, "Connection string:");

  /* Input field */
  input_draw(input, win, y, 2);
  y += 2;

  /* Error message */
  if (error_msg && *error_msg) {
    wattron(win, COLOR_PAIR(COLOR_ERROR));
    mvwprintw(win, y++, 2, "Error: %s", error_msg);
    wattroff(win, COLOR_PAIR(COLOR_ERROR));
  }

  /* Buttons */
  y = height - 3;
  mvwprintw(win, y, 2, "[Enter] Connect   [Tab] Switch driver   [Esc] Cancel");

  wrefresh(win);
}

char *connect_view_show(TuiState *state) {
  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

  int height = 18;
  int width = 70;
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

  int selected_driver = 0;
  char *error_msg = NULL;
  char *result = NULL;

  bool running = true;
  while (running) {
    draw_dialog(dialog, height, width, &input, selected_driver, error_msg);

    int ch = wgetch(dialog);

    free(error_msg);
    error_msg = NULL;

    switch (ch) {
    case 27: /* Escape */
      running = false;
      break;

    case '\n':
    case KEY_ENTER:
      if (input.len > 0) {
        /* Try to connect */
        char *err = NULL;
        DbConnection *conn = db_connect(input.buffer, &err);
        if (conn) {
          db_disconnect(conn);
          result = str_dup(input.buffer);
          running = false;
        } else {
          error_msg = err ? err : str_dup("Connection failed");
        }
      } else {
        error_msg = str_dup("Please enter a connection string");
      }
      break;

    case '\t':
    case KEY_DOWN:
      selected_driver = (selected_driver + 1) % 3;
      break;

    case KEY_BTAB:
    case KEY_UP:
      selected_driver = (selected_driver + 2) % 3;
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
