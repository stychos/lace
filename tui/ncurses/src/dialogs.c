/*
 * Lace ncurses frontend
 * Modal dialogs
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "dialogs.h"
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Dialog window dimensions */
#define DIALOG_MIN_WIDTH 30
#define DIALOG_MAX_WIDTH 70
#define DIALOG_PADDING 2

/* ==========================================================================
 * Dialog Helpers
 * ========================================================================== */

static WINDOW *create_dialog_win(TuiState *tui, int height, int width,
                                 const char *title) {
  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

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

static void destroy_dialog_win(WINDOW *win) {
  if (!win) return;
  werase(win);
  wrefresh(win);
  delwin(win);
}

/* ==========================================================================
 * Confirmation Dialog
 * ========================================================================== */

bool dialog_confirm(TuiState *tui, const char *title, const char *message) {
  int msg_len = message ? (int)strlen(message) : 0;
  int width = msg_len + DIALOG_PADDING * 2 + 2;
  if (width < DIALOG_MIN_WIDTH) width = DIALOG_MIN_WIDTH;
  if (width > DIALOG_MAX_WIDTH) width = DIALOG_MAX_WIDTH;

  int height = 7;

  WINDOW *win = create_dialog_win(tui, height, width, title);
  if (!win) return false;

  /* Draw message */
  if (message) {
    mvwprintw(win, 2, DIALOG_PADDING, "%.*s", width - DIALOG_PADDING * 2, message);
  }

  bool selected = false; /* false = No, true = Yes */
  bool running = true;
  bool result = false;

  while (running) {
    /* Draw buttons */
    int btn_y = 4;
    int yes_x = width / 3 - 2;
    int no_x = 2 * width / 3 - 2;

    if (selected) {
      wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
      mvwprintw(win, btn_y, yes_x, "[ Yes ]");
      wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
      mvwprintw(win, btn_y, no_x, "[ No ]");
    } else {
      mvwprintw(win, btn_y, yes_x, "[ Yes ]");
      wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
      mvwprintw(win, btn_y, no_x, "[ No ]");
      wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    }

    wrefresh(win);

    int ch = wgetch(win);
    switch (ch) {
    case '\n':
    case KEY_ENTER:
      result = selected;
      running = false;
      break;

    case 27: /* Escape */
      result = false;
      running = false;
      break;

    case 'y':
    case 'Y':
      result = true;
      running = false;
      break;

    case 'n':
    case 'N':
      result = false;
      running = false;
      break;

    case '\t':
    case KEY_LEFT:
    case KEY_RIGHT:
    case 'h':
    case 'l':
      selected = !selected;
      break;
    }
  }

  destroy_dialog_win(win);
  tui->app->needs_redraw = true;
  return result;
}

/* ==========================================================================
 * Goto Row Dialog
 * ========================================================================== */

bool dialog_goto_row(TuiState *tui, size_t max_row, size_t *result) {
  int width = 40;
  int height = 7;

  WINDOW *win = create_dialog_win(tui, height, width, "Go to Row");
  if (!win) return false;

  char prompt[64];
  snprintf(prompt, sizeof(prompt), "Row number (1-%zu):", max_row + 1);
  mvwprintw(win, 2, DIALOG_PADDING, "%s", prompt);

  /* Input field */
  char input[32] = "";
  size_t input_len = 0;
  int input_x = DIALOG_PADDING;
  int input_y = 3;
  int input_width = width - DIALOG_PADDING * 2;

  bool running = true;
  bool confirmed = false;

  curs_set(1);

  while (running) {
    /* Draw input field */
    wattron(win, A_UNDERLINE);
    mvwprintw(win, input_y, input_x, "%-*s", input_width, input);
    wattroff(win, A_UNDERLINE);
    wmove(win, input_y, input_x + (int)input_len);
    wrefresh(win);

    int ch = wgetch(win);
    switch (ch) {
    case '\n':
    case KEY_ENTER:
      if (input_len > 0) {
        size_t row = (size_t)strtoul(input, NULL, 10);
        if (row >= 1 && row <= max_row + 1) {
          *result = row - 1; /* Convert to 0-based */
          confirmed = true;
        }
      }
      running = false;
      break;

    case 27: /* Escape */
      running = false;
      break;

    case KEY_BACKSPACE:
    case 127:
    case 8:
      if (input_len > 0) {
        input[--input_len] = '\0';
      }
      break;

    default:
      if (isdigit(ch) && input_len < sizeof(input) - 1) {
        input[input_len++] = (char)ch;
        input[input_len] = '\0';
      }
      break;
    }
  }

  curs_set(0);
  destroy_dialog_win(win);
  tui->app->needs_redraw = true;
  return confirmed;
}

/* ==========================================================================
 * Input Dialog
 * ========================================================================== */

bool dialog_input(TuiState *tui, const char *title, const char *prompt,
                  const char *initial, char **result) {
  int width = 50;
  int height = 7;

  WINDOW *win = create_dialog_win(tui, height, width, title);
  if (!win) return false;

  if (prompt) {
    mvwprintw(win, 2, DIALOG_PADDING, "%s", prompt);
  }

  /* Input field */
  char input[256] = "";
  size_t input_len = 0;
  if (initial) {
    strncpy(input, initial, sizeof(input) - 1);
    input_len = strlen(input);
  }

  int input_x = DIALOG_PADDING;
  int input_y = 3;
  int input_width = width - DIALOG_PADDING * 2;

  bool running = true;
  bool confirmed = false;

  curs_set(1);

  while (running) {
    /* Draw input field */
    wattron(win, A_UNDERLINE);
    mvwprintw(win, input_y, input_x, "%-*s", input_width, input);
    wattroff(win, A_UNDERLINE);
    wmove(win, input_y, input_x + (int)input_len);
    wrefresh(win);

    int ch = wgetch(win);
    switch (ch) {
    case '\n':
    case KEY_ENTER:
      *result = strdup(input);
      confirmed = true;
      running = false;
      break;

    case 27: /* Escape */
      running = false;
      break;

    case KEY_BACKSPACE:
    case 127:
    case 8:
      if (input_len > 0) {
        input[--input_len] = '\0';
      }
      break;

    default:
      if (ch >= 32 && ch < 127 && input_len < sizeof(input) - 1) {
        input[input_len++] = (char)ch;
        input[input_len] = '\0';
      }
      break;
    }
  }

  curs_set(0);
  destroy_dialog_win(win);
  tui->app->needs_redraw = true;
  return confirmed;
}

/* ==========================================================================
 * Schema Dialog
 * ========================================================================== */

void dialog_schema(TuiState *tui) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->schema) {
    dialog_message(tui, "Schema", "No table loaded");
    return;
  }

  LaceSchema *schema = tab->schema;
  int num_cols = (int)schema->num_columns;

  /* Calculate dialog size */
  int width = 60;
  int height = num_cols + 6;
  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);
  if (height > term_rows - 4) height = term_rows - 4;

  char title[128];
  snprintf(title, sizeof(title), "Schema: %s", tab->table_name ? tab->table_name : "unknown");

  WINDOW *win = create_dialog_win(tui, height, width, title);
  if (!win) return;

  /* Draw header */
  wattron(win, A_BOLD);
  mvwprintw(win, 2, 2, "%-20s %-12s %-6s %-6s", "Column", "Type", "NULL", "PK");
  wattroff(win, A_BOLD);

  /* Draw separator */
  mvwhline(win, 3, 2, ACS_HLINE, width - 4);

  /* Draw columns */
  int max_visible = height - 6;
  for (int i = 0; i < num_cols && i < max_visible; i++) {
    LaceColumn *col = &schema->columns[i];

    const char *type_str = "?";
    switch (col->type) {
    case LACE_TYPE_INT: type_str = "INTEGER"; break;
    case LACE_TYPE_FLOAT: type_str = "REAL"; break;
    case LACE_TYPE_TEXT: type_str = "TEXT"; break;
    case LACE_TYPE_BLOB: type_str = "BLOB"; break;
    case LACE_TYPE_BOOL: type_str = "BOOL"; break;
    case LACE_TYPE_DATE: type_str = "DATE"; break;
    case LACE_TYPE_TIMESTAMP: type_str = "TIMESTAMP"; break;
    case LACE_TYPE_NULL: type_str = "NULL"; break;
    default: type_str = "?"; break;
    }

    mvwprintw(win, 4 + i, 2, "%-20.20s %-12s %-6s %-6s",
              col->name ? col->name : "?",
              type_str,
              col->nullable ? "YES" : "NO",
              col->primary_key ? "YES" : "");
  }

  if (num_cols > max_visible) {
    mvwprintw(win, height - 2, 2, "... and %d more columns", num_cols - max_visible);
  }

  wrefresh(win);

  /* Wait for key */
  wgetch(win);

  destroy_dialog_win(win);
  tui->app->needs_redraw = true;
}

/* ==========================================================================
 * Help Dialog
 * ========================================================================== */

void dialog_help(TuiState *tui) {
  int width = 55;
  int height = 24;

  WINDOW *win = create_dialog_win(tui, height, width, "Help - Keyboard Shortcuts");
  if (!win) return;

  int y = 2;
  wattron(win, A_BOLD);
  mvwprintw(win, y++, 2, "Navigation");
  wattroff(win, A_BOLD);
  mvwprintw(win, y++, 2, "  h/j/k/l, Arrows  Move cursor");
  mvwprintw(win, y++, 2, "  PgUp/PgDn        Page up/down");
  mvwprintw(win, y++, 2, "  g, Home          Go to first row");
  mvwprintw(win, y++, 2, "  G, End           Go to last row");
  mvwprintw(win, y++, 2, "  /                Go to row number");

  y++;
  wattron(win, A_BOLD);
  mvwprintw(win, y++, 2, "Editing");
  wattroff(win, A_BOLD);
  mvwprintw(win, y++, 2, "  Enter            Edit cell");
  mvwprintw(win, y++, 2, "  Ctrl+N           Set NULL");
  mvwprintw(win, y++, 2, "  Ctrl+D           Set empty");
  mvwprintw(win, y++, 2, "  x, Delete        Delete row");

  y++;
  wattron(win, A_BOLD);
  mvwprintw(win, y++, 2, "Tabs & Sidebar");
  wattroff(win, A_BOLD);
  mvwprintw(win, y++, 2, "  t                Toggle sidebar");
  mvwprintw(win, y++, 2, "  [, ]             Prev/next tab");
  mvwprintw(win, y++, 2, "  -                Close tab");
  mvwprintw(win, y++, 2, "  Tab              Switch focus");

  y++;
  wattron(win, A_BOLD);
  mvwprintw(win, y++, 2, "Other");
  wattroff(win, A_BOLD);
  mvwprintw(win, y++, 2, "  s                Show schema");
  mvwprintw(win, y++, 2, "  r, F5            Refresh data");
  mvwprintw(win, y++, 2, "  q, Ctrl+X        Quit");

  mvwprintw(win, height - 2, 2, "Press any key to close...");

  wrefresh(win);
  wgetch(win);

  destroy_dialog_win(win);
  tui->app->needs_redraw = true;
}

/* ==========================================================================
 * Message Dialog
 * ========================================================================== */

void dialog_message(TuiState *tui, const char *title, const char *message) {
  int msg_len = message ? (int)strlen(message) : 0;
  int width = msg_len + DIALOG_PADDING * 2 + 2;
  if (width < DIALOG_MIN_WIDTH) width = DIALOG_MIN_WIDTH;
  if (width > DIALOG_MAX_WIDTH) width = DIALOG_MAX_WIDTH;

  int height = 5;

  WINDOW *win = create_dialog_win(tui, height, width, title);
  if (!win) return;

  if (message) {
    mvwprintw(win, 2, DIALOG_PADDING, "%.*s", width - DIALOG_PADDING * 2, message);
  }

  wrefresh(win);
  wgetch(win);

  destroy_dialog_win(win);
  tui->app->needs_redraw = true;
}
