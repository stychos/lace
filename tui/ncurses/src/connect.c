/*
 * Lace ncurses frontend
 * Connection dialog
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connect.h"
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Dialog dimensions */
#define CONNECT_DIALOG_WIDTH 60
#define CONNECT_DIALOG_HEIGHT 12

/* Field positions */
#define FIELD_CONNSTR 0
#define FIELD_PASSWORD 1
#define FIELD_COUNT 2

/* ==========================================================================
 * Helper Functions
 * ========================================================================== */

static WINDOW *create_dialog_win(TuiState *tui, const char *title) {
  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

  int height = CONNECT_DIALOG_HEIGHT;
  int width = CONNECT_DIALOG_WIDTH;

  /* Center dialog */
  int y = (term_rows - height) / 2;
  int x = (term_cols - width) / 2;

  /* Clamp to screen */
  if (y < 0) y = 0;
  if (x < 0) x = 0;
  if (y + height > term_rows) height = term_rows - y;
  if (x + width > term_cols) width = term_cols - x;

  WINDOW *win = newwin(height, width, y, x);
  if (!win) return NULL;

  keypad(win, TRUE);

  /* Draw border */
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  box(win, 0, 0);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));

  /* Draw title */
  if (title) {
    int title_len = (int)strlen(title);
    int title_x = (width - title_len - 2) / 2;
    if (title_x < 1) title_x = 1;

    wattron(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    mvwprintw(win, 0, title_x, " %s ", title);
    wattroff(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
  }

  (void)tui;
  return win;
}

static void destroy_dialog_win(WINDOW *win, TuiState *tui) {
  if (!win) return;
  werase(win);
  wrefresh(win);
  delwin(win);
  tui->app->needs_redraw = true;
}

/* ==========================================================================
 * Connection Dialog
 * ========================================================================== */

bool connect_dialog(TuiState *tui) {
  return connect_quick_dialog(tui);
}

bool connect_quick_dialog(TuiState *tui) {
  if (!tui || !tui->app) return false;

  WINDOW *win = create_dialog_win(tui, "Connect to Database");
  if (!win) return false;

  /* Fields */
  char connstr[512] = "";
  char password[128] = "";
  size_t connstr_len = 0;
  size_t password_len = 0;
  int current_field = FIELD_CONNSTR;

  int label_x = 2;
  int input_x = 14;
  int input_width = CONNECT_DIALOG_WIDTH - input_x - 3;

  bool running = true;
  bool connected = false;

  curs_set(1);

  while (running) {
    /* Draw labels */
    mvwprintw(win, 2, label_x, "Connection:");
    mvwprintw(win, 4, label_x, "Password:");

    /* Draw connection string field */
    if (current_field == FIELD_CONNSTR) {
      wattron(win, A_UNDERLINE | A_BOLD);
    } else {
      wattron(win, A_UNDERLINE);
    }
    mvwprintw(win, 2, input_x, "%-*.*s", input_width, input_width, connstr);
    wattroff(win, A_UNDERLINE | A_BOLD);

    /* Draw password field (masked) */
    if (current_field == FIELD_PASSWORD) {
      wattron(win, A_UNDERLINE | A_BOLD);
    } else {
      wattron(win, A_UNDERLINE);
    }
    char masked[128];
    memset(masked, '*', password_len);
    masked[password_len] = '\0';
    mvwprintw(win, 4, input_x, "%-*.*s", input_width, input_width, masked);
    wattroff(win, A_UNDERLINE | A_BOLD);

    /* Draw buttons */
    mvwprintw(win, 7, 10, "[ Connect ]");
    mvwprintw(win, 7, 30, "[ Cancel ]");

    /* Draw help text */
    wattron(win, A_DIM);
    mvwprintw(win, 9, 2, "Example: sqlite:///path/to/db.sqlite");
    mvwprintw(win, 10, 2, "         postgres://user@host/db");
    wattroff(win, A_DIM);

    /* Position cursor */
    size_t *current_len = (current_field == FIELD_CONNSTR) ?
                           &connstr_len : &password_len;
    int cursor_x = input_x + (int)*current_len;
    if (cursor_x >= input_x + input_width) {
      cursor_x = input_x + input_width - 1;
    }
    wmove(win, 2 + current_field * 2, cursor_x);
    wrefresh(win);

    int ch = wgetch(win);
    char *current_buf = (current_field == FIELD_CONNSTR) ? connstr : password;
    size_t max_len = (current_field == FIELD_CONNSTR) ?
                     sizeof(connstr) - 1 : sizeof(password) - 1;

    switch (ch) {
    case '\n':
    case KEY_ENTER:
      if (connstr_len > 0) {
        /* Try to connect */
        int conn_idx = app_connect(tui->app, connstr,
                                   password_len > 0 ? password : NULL);
        if (conn_idx >= 0) {
          connected = true;
          running = false;
        } else {
          /* Show error */
          wattron(win, COLOR_PAIR(COLOR_ERROR));
          mvwprintw(win, 6, 2, "Connection failed!            ");
          wattroff(win, COLOR_PAIR(COLOR_ERROR));
        }
      }
      break;

    case 27: /* Escape */
      running = false;
      break;

    case '\t':
    case KEY_DOWN:
      current_field = (current_field + 1) % FIELD_COUNT;
      break;

    case KEY_UP:
      current_field = (current_field + FIELD_COUNT - 1) % FIELD_COUNT;
      break;

    case KEY_BACKSPACE:
    case 127:
    case 8:
      if (*current_len > 0) {
        current_buf[--(*current_len)] = '\0';
      }
      break;

    default:
      /* Printable characters */
      if (ch >= 32 && ch < 127 && *current_len < max_len) {
        current_buf[(*current_len)++] = (char)ch;
        current_buf[*current_len] = '\0';
      }
      break;
    }
  }

  curs_set(0);

  /* Clear password from memory */
  memset(password, 0, sizeof(password));

  destroy_dialog_win(win, tui);
  return connected;
}
