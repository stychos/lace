/*
 * Lace ncurses frontend
 * TUI implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "tui.h"
#include "dialogs.h"
#include "connect.h"
#include "filters.h"
#include "session.h"
#include <locale.h>
#include <stdlib.h>
#include <string.h>

/* Sidebar width */
#define SIDEBAR_WIDTH 24
#define MIN_CONTENT_WIDTH 40

/* Page size for data loading (must match app.c) */
#define PAGE_SIZE 500

/* ==========================================================================
 * TUI Lifecycle
 * ========================================================================== */

TuiState *tui_init(AppState *app) {
  if (!app) {
    return NULL;
  }

  TuiState *tui = calloc(1, sizeof(TuiState));
  if (!tui) {
    return NULL;
  }

  tui->app = app;
  tui->sidebar_width = SIDEBAR_WIDTH;

  /* Initialize ncurses */
  setlocale(LC_ALL, "");
  initscr();
  raw();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  mouseinterval(0);

  /* Initialize colors if supported */
  if (has_colors()) {
    start_color();
    use_default_colors();

    init_pair(COLOR_HEADER, COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_SELECTED, COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_STATUS, COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_ERROR, COLOR_WHITE, COLOR_RED);
    init_pair(COLOR_BORDER, COLOR_CYAN, -1);
    init_pair(COLOR_TITLE, COLOR_YELLOW, -1);
    init_pair(COLOR_NULL, COLOR_MAGENTA, -1);
    init_pair(COLOR_NUMBER, COLOR_CYAN, -1);
    init_pair(COLOR_SIDEBAR, COLOR_WHITE, COLOR_BLUE);
    init_pair(COLOR_EDIT, COLOR_BLACK, COLOR_YELLOW);
  }

  /* Enable mouse support */
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);

  /* Get terminal dimensions */
  getmaxyx(stdscr, tui->term_rows, tui->term_cols);

  /* Calculate content dimensions */
  if (app->sidebar_visible) {
    tui->content_width = tui->term_cols - tui->sidebar_width;
  } else {
    tui->content_width = tui->term_cols;
  }
  tui->content_height = tui->term_rows - 3; /* Tab bar + status bar + border */

  /* Create windows */
  tui->tab_win = newwin(1, tui->term_cols, 0, 0);
  tui->status_win = newwin(1, tui->term_cols, tui->term_rows - 1, 0);

  if (app->sidebar_visible) {
    tui->sidebar_win = newwin(tui->term_rows - 2, tui->sidebar_width, 1, 0);
    tui->main_win = newwin(tui->term_rows - 2, tui->content_width,
                           1, tui->sidebar_width);
  } else {
    tui->sidebar_win = NULL;
    tui->main_win = newwin(tui->term_rows - 2, tui->content_width, 1, 0);
  }

  keypad(tui->main_win, TRUE);
  if (tui->sidebar_win) {
    keypad(tui->sidebar_win, TRUE);
  }

  /* Start with sidebar focused if no tabs are open */
  if (app->num_tabs == 0 && app->sidebar_visible) {
    tui->in_sidebar = true;
  }

  return tui;
}

void tui_cleanup(TuiState *tui) {
  if (!tui) {
    return;
  }

  /* Delete windows */
  if (tui->tab_win) delwin(tui->tab_win);
  if (tui->status_win) delwin(tui->status_win);
  if (tui->sidebar_win) delwin(tui->sidebar_win);
  if (tui->main_win) delwin(tui->main_win);

  /* End ncurses */
  endwin();

  free(tui);
}

/* ==========================================================================
 * Drawing Functions
 * ========================================================================== */

void tui_draw_tabs(TuiState *tui) {
  if (!tui || !tui->tab_win) return;

  werase(tui->tab_win);
  wattron(tui->tab_win, COLOR_PAIR(COLOR_HEADER));

  /* Fill background */
  for (int i = 0; i < tui->term_cols; i++) {
    mvwaddch(tui->tab_win, 0, i, ' ');
  }

  /* Draw tabs */
  int x = 1;
  for (size_t i = 0; i < tui->app->num_tabs && x < tui->term_cols - 10; i++) {
    Tab *tab = &tui->app->tabs[i];
    const char *title = tab->title ? tab->title : "?";

    if (i == tui->app->active_tab) {
      wattroff(tui->tab_win, COLOR_PAIR(COLOR_HEADER));
      wattron(tui->tab_win, A_REVERSE | A_BOLD);
    }

    mvwprintw(tui->tab_win, 0, x, " %s ", title);
    x += (int)strlen(title) + 3;

    if (i == tui->app->active_tab) {
      wattroff(tui->tab_win, A_REVERSE | A_BOLD);
      wattron(tui->tab_win, COLOR_PAIR(COLOR_HEADER));
    }
  }

  /* Show tab count if many tabs */
  if (tui->app->num_tabs > 0) {
    mvwprintw(tui->tab_win, 0, tui->term_cols - 8, "[%zu/%zu]",
              tui->app->active_tab + 1, tui->app->num_tabs);
  }

  wattroff(tui->tab_win, COLOR_PAIR(COLOR_HEADER));
  wrefresh(tui->tab_win);
}

void tui_draw_status(TuiState *tui) {
  if (!tui || !tui->status_win) return;

  werase(tui->status_win);

  if (tui->app->status_is_error) {
    wattron(tui->status_win, COLOR_PAIR(COLOR_ERROR));
  } else {
    wattron(tui->status_win, COLOR_PAIR(COLOR_STATUS));
  }

  /* Fill background */
  for (int i = 0; i < tui->term_cols; i++) {
    mvwaddch(tui->status_win, 0, i, ' ');
  }

  /* Draw status message */
  if (tui->app->status_message) {
    mvwprintw(tui->status_win, 0, 1, "%s", tui->app->status_message);
  }

  /* Draw position info for table tabs */
  Tab *tab = app_current_tab(tui->app);
  if (tab && tab->type == TAB_TYPE_TABLE && tab->data) {
    char pos[64];
    size_t current = tab->data_offset + tab->cursor_row + 1;
    snprintf(pos, sizeof(pos), "Row %zu/%zu", current, tab->total_rows);
    mvwprintw(tui->status_win, 0, tui->term_cols - (int)strlen(pos) - 2, "%s", pos);
  }

  /* Draw help hint */
  mvwprintw(tui->status_win, 0, tui->term_cols / 2 - 10, "q:Quit t:Sidebar ?:Help");

  wattroff(tui->status_win, COLOR_PAIR(COLOR_STATUS));
  wattroff(tui->status_win, COLOR_PAIR(COLOR_ERROR));
  wrefresh(tui->status_win);
}

void tui_draw_sidebar(TuiState *tui) {
  if (!tui || !tui->sidebar_win || !tui->app->sidebar_visible) return;

  werase(tui->sidebar_win);

  Connection *conn = app_current_connection(tui->app);
  int height = tui->term_rows - 2;

  /* Draw border */
  wattron(tui->sidebar_win, COLOR_PAIR(COLOR_BORDER));
  for (int y = 0; y < height; y++) {
    mvwaddch(tui->sidebar_win, y, tui->sidebar_width - 1, ACS_VLINE);
  }
  wattroff(tui->sidebar_win, COLOR_PAIR(COLOR_BORDER));

  if (!conn) {
    wattron(tui->sidebar_win, COLOR_PAIR(COLOR_NULL));
    mvwprintw(tui->sidebar_win, 1, 1, "No connection");
    wattroff(tui->sidebar_win, COLOR_PAIR(COLOR_NULL));
    wrefresh(tui->sidebar_win);
    return;
  }

  /* Draw header */
  wattron(tui->sidebar_win, A_BOLD);
  mvwprintw(tui->sidebar_win, 0, 1, "Tables (%zu)", conn->num_tables);
  wattroff(tui->sidebar_win, A_BOLD);

  /* Draw table list */
  int y = 2;
  for (size_t i = tui->app->sidebar_scroll;
       i < conn->num_tables && y < height - 1; i++) {
    const char *name = conn->tables[i];
    if (!name) continue;

    /* Apply filter if set */
    if (tui->app->sidebar_filter && tui->app->sidebar_filter[0]) {
      if (!strstr(name, tui->app->sidebar_filter)) {
        continue;
      }
    }

    bool selected = (i == tui->app->sidebar_selected);

    if (selected && tui->in_sidebar) {
      wattron(tui->sidebar_win, COLOR_PAIR(COLOR_SELECTED));
    }

    /* Truncate long names */
    char display[32];
    int max_len = tui->sidebar_width - 3;
    if ((int)strlen(name) > max_len) {
      snprintf(display, sizeof(display), "%.*s..", max_len - 2, name);
    } else {
      snprintf(display, sizeof(display), "%s", name);
    }

    mvwprintw(tui->sidebar_win, y, 1, "%-*s", max_len, display);

    if (selected && tui->in_sidebar) {
      wattroff(tui->sidebar_win, COLOR_PAIR(COLOR_SELECTED));
    }

    y++;
  }

  wrefresh(tui->sidebar_win);
}

void tui_draw_content(TuiState *tui) {
  if (!tui || !tui->main_win) return;

  werase(tui->main_win);

  Tab *tab = app_current_tab(tui->app);
  if (!tab) {
    /* No tab open */
    wattron(tui->main_win, COLOR_PAIR(COLOR_NULL));
    mvwprintw(tui->main_win, tui->content_height / 2,
              (tui->content_width - 20) / 2,
              "No table open");
    mvwprintw(tui->main_win, tui->content_height / 2 + 1,
              (tui->content_width - 30) / 2,
              "Select a table from sidebar");
    wattroff(tui->main_win, COLOR_PAIR(COLOR_NULL));
    wrefresh(tui->main_win);
    return;
  }

  if (tab->type == TAB_TYPE_TABLE) {
    /* Draw table data */
    if (!tab->data || tab->data->num_rows == 0) {
      wattron(tui->main_win, COLOR_PAIR(COLOR_NULL));
      mvwprintw(tui->main_win, tui->content_height / 2,
                (tui->content_width - 15) / 2,
                "No data");
      wattroff(tui->main_win, COLOR_PAIR(COLOR_NULL));
      wrefresh(tui->main_win);
      return;
    }

    LaceResult *data = tab->data;

    /* Calculate column widths */
    int col_widths[64];
    size_t visible_cols = data->num_columns < 64 ? data->num_columns : 64;

    for (size_t i = 0; i < visible_cols; i++) {
      int width = 8; /* Minimum width */
      if (data->columns && data->columns[i].name) {
        int name_len = (int)strlen(data->columns[i].name);
        if (name_len > width) width = name_len;
      }
      if (width > 20) width = 20; /* Maximum width */
      col_widths[i] = width + 2;
    }

    /* Draw header */
    wattron(tui->main_win, COLOR_PAIR(COLOR_HEADER));
    int x = 0;
    for (size_t i = tab->scroll_col; i < visible_cols && x < tui->content_width; i++) {
      const char *name = (data->columns && data->columns[i].name)
                             ? data->columns[i].name : "?";
      mvwprintw(tui->main_win, 0, x, " %-*.*s",
                col_widths[i] - 1, col_widths[i] - 1, name);
      x += col_widths[i];
    }
    /* Fill rest of header */
    for (; x < tui->content_width; x++) {
      mvwaddch(tui->main_win, 0, x, ' ');
    }
    wattroff(tui->main_win, COLOR_PAIR(COLOR_HEADER));

    /* Draw rows */
    int y = 1;
    for (size_t row = tab->scroll_row;
         row < data->num_rows && y < tui->content_height; row++) {
      bool is_cursor = (row == tab->cursor_row && !tui->in_sidebar);

      x = 0;
      for (size_t col = tab->scroll_col;
           col < visible_cols && x < tui->content_width; col++) {

        bool is_cell_selected = is_cursor && (col == tab->cursor_col);

        if (is_cell_selected) {
          wattron(tui->main_win, COLOR_PAIR(COLOR_SELECTED));
        }

        /* Get cell value */
        LaceValue *cell = NULL;
        if (data->rows && row < data->num_rows &&
            data->rows[row].cells && col < data->rows[row].num_cells) {
          cell = &data->rows[row].cells[col];
        }

        char *val_str = cell ? lace_value_to_string(cell) : NULL;
        const char *display = val_str ? val_str : "NULL";

        /* Apply NULL color */
        if (!val_str || (cell && cell->is_null)) {
          if (!is_cell_selected) {
            wattron(tui->main_win, COLOR_PAIR(COLOR_NULL));
          }
        } else if (cell && (cell->type == LACE_TYPE_INT || cell->type == LACE_TYPE_FLOAT)) {
          if (!is_cell_selected) {
            wattron(tui->main_win, COLOR_PAIR(COLOR_NUMBER));
          }
        }

        mvwprintw(tui->main_win, y, x, " %-*.*s",
                  col_widths[col] - 1, col_widths[col] - 1, display);

        wattroff(tui->main_win, COLOR_PAIR(COLOR_NULL));
        wattroff(tui->main_win, COLOR_PAIR(COLOR_NUMBER));
        if (is_cell_selected) {
          wattroff(tui->main_win, COLOR_PAIR(COLOR_SELECTED));
        }

        free(val_str);
        x += col_widths[col];
      }
      y++;
    }
  } else if (tab->type == TAB_TYPE_QUERY) {
    /* Draw query editor placeholder */
    mvwprintw(tui->main_win, 1, 1, "Query Editor (not implemented)");
  }

  wrefresh(tui->main_win);
}

void tui_draw(TuiState *tui) {
  if (!tui) return;

  tui_draw_tabs(tui);
  if (tui->app->sidebar_visible) {
    tui_draw_sidebar(tui);
  }
  tui_draw_content(tui);
  tui_draw_status(tui);

  tui->app->needs_redraw = false;
}

/* ==========================================================================
 * Input Handling
 * ========================================================================== */

bool tui_handle_input(TuiState *tui, int ch) {
  if (!tui || !tui->app) return false;

  Tab *tab = app_current_tab(tui->app);

  /* Global keys */
  switch (ch) {
  case 'q':
  case 'Q':
    return false; /* Quit */

  case 't':
  case 'T':
    /* Toggle sidebar */
    tui->app->sidebar_visible = !tui->app->sidebar_visible;
    /* Recreate windows */
    if (tui->sidebar_win) {
      delwin(tui->sidebar_win);
      tui->sidebar_win = NULL;
    }
    if (tui->main_win) {
      delwin(tui->main_win);
    }

    if (tui->app->sidebar_visible) {
      tui->content_width = tui->term_cols - tui->sidebar_width;
      tui->sidebar_win = newwin(tui->term_rows - 2, tui->sidebar_width, 1, 0);
      tui->main_win = newwin(tui->term_rows - 2, tui->content_width,
                             1, tui->sidebar_width);
      keypad(tui->sidebar_win, TRUE);
    } else {
      tui->content_width = tui->term_cols;
      tui->main_win = newwin(tui->term_rows - 2, tui->content_width, 1, 0);
    }
    keypad(tui->main_win, TRUE);
    tui->app->needs_redraw = true;
    return true;

  case '\t':
    /* Switch focus between sidebar and content */
    if (tui->app->sidebar_visible) {
      tui->in_sidebar = !tui->in_sidebar;
      tui->app->needs_redraw = true;
    }
    return true;

  case '[':
  case KEY_BTAB:
    /* Previous tab */
    if (tui->app->num_tabs > 0) {
      size_t new_tab = tui->app->active_tab > 0
                           ? tui->app->active_tab - 1
                           : tui->app->num_tabs - 1;
      app_switch_tab(tui->app, new_tab);
    }
    return true;

  case ']':
    /* Next tab */
    if (tui->app->num_tabs > 0) {
      size_t new_tab = (tui->app->active_tab + 1) % tui->app->num_tabs;
      app_switch_tab(tui->app, new_tab);
    }
    return true;

  case '-':
    /* Close current tab */
    if (tui->app->num_tabs > 0) {
      app_close_tab(tui->app, tui->app->active_tab);
    }
    return true;

  case 'r':
  case 'R':
  case KEY_F(5):
    /* Refresh data */
    app_refresh_data(tui->app);
    return true;

  case KEY_RESIZE:
    /* Handle terminal resize */
    getmaxyx(stdscr, tui->term_rows, tui->term_cols);
    /* TODO: Recreate windows with new sizes */
    tui->app->needs_redraw = true;
    return true;

  case '?':
  case KEY_F(1):
    /* Help dialog */
    dialog_help(tui);
    return true;

  case 's':
  case KEY_F(3):
    /* Schema dialog */
    if (tab && tab->type == TAB_TYPE_TABLE && tab->schema) {
      dialog_schema(tui);
    }
    return true;

  case 'c':
  case KEY_F(2):
    /* Connect dialog */
    connect_dialog(tui);
    return true;

  case 'p':
    /* Open query tab */
    if (tui->app->active_connection >= 0) {
      app_open_query_tab(tui->app, tui->app->active_connection);
    }
    return true;

  case KEY_MOUSE:
    /* Handle mouse events */
    {
      MEVENT event;
      if (getmouse(&event) == OK) {
        /* Check if click is in sidebar */
        if (tui->app->sidebar_visible && event.x < tui->sidebar_width) {
          if (event.bstate & BUTTON1_CLICKED) {
            tui->in_sidebar = true;
            /* Calculate which table was clicked */
            int row = event.y - 3; /* Adjust for header */
            if (row >= 0 && (size_t)row < app_current_connection(tui->app)->num_tables) {
              tui->app->sidebar_selected = tui->app->sidebar_scroll + (size_t)row;
              tui->app->needs_redraw = true;
            }
          } else if (event.bstate & BUTTON1_DOUBLE_CLICKED) {
            /* Double-click opens table */
            tui->in_sidebar = true;
            int row = event.y - 3;
            Connection *conn = app_current_connection(tui->app);
            if (conn && row >= 0 && (size_t)row < conn->num_tables) {
              size_t idx = tui->app->sidebar_scroll + (size_t)row;
              if (idx < conn->num_tables) {
                app_open_table(tui->app, tui->app->active_connection, conn->tables[idx]);
                tui->in_sidebar = false;
              }
            }
          }
        } else {
          /* Click in main content */
          if (event.bstate & BUTTON1_CLICKED) {
            tui->in_sidebar = false;
            tui->app->needs_redraw = true;
          }
        }

        /* Scroll wheel */
        if (event.bstate & BUTTON4_PRESSED) {
          /* Scroll up */
          if (tab && tab->type == TAB_TYPE_TABLE && tab->cursor_row > 0) {
            tab->cursor_row--;
            if (tab->cursor_row < tab->scroll_row) {
              tab->scroll_row = tab->cursor_row;
            }
            tui->app->needs_redraw = true;
          }
        } else if (event.bstate & BUTTON5_PRESSED) {
          /* Scroll down */
          if (tab && tab->type == TAB_TYPE_TABLE && tab->data &&
              tab->cursor_row < tab->data->num_rows - 1) {
            tab->cursor_row++;
            if (tab->cursor_row >= tab->scroll_row + (size_t)(tui->content_height - 1)) {
              tab->scroll_row++;
            }
            tui->app->needs_redraw = true;
          }
        }
      }
    }
    return true;
  }

  /* Sidebar navigation */
  if (tui->in_sidebar && tui->app->sidebar_visible) {
    Connection *conn = app_current_connection(tui->app);
    if (!conn) return true;

    switch (ch) {
    case 'j':
    case KEY_DOWN:
      if (tui->app->sidebar_selected < conn->num_tables - 1) {
        tui->app->sidebar_selected++;
        tui->app->needs_redraw = true;
      }
      return true;

    case 'k':
    case KEY_UP:
      if (tui->app->sidebar_selected > 0) {
        tui->app->sidebar_selected--;
        tui->app->needs_redraw = true;
      }
      return true;

    case '\n':
    case KEY_ENTER:
      /* Open selected table */
      if (tui->app->sidebar_selected < conn->num_tables) {
        const char *table = conn->tables[tui->app->sidebar_selected];
        if (table) {
          app_open_table(tui->app, tui->app->active_connection, table);
          tui->in_sidebar = false;
        }
      }
      return true;
    }
  }

  /* Table navigation */
  if (!tui->in_sidebar && tab && tab->type == TAB_TYPE_TABLE && tab->data) {
    switch (ch) {
    case 'j':
    case KEY_DOWN:
      if (tab->cursor_row < tab->data->num_rows - 1) {
        tab->cursor_row++;
        /* Scroll if needed */
        if (tab->cursor_row >= tab->scroll_row + (size_t)(tui->content_height - 1)) {
          tab->scroll_row++;
        }
        tui->app->needs_redraw = true;
      } else if (tab->data_offset + tab->data->num_rows < tab->total_rows) {
        /* Load next page */
        app_load_more(tui->app, true);
      }
      return true;

    case 'k':
    case KEY_UP:
      if (tab->cursor_row > 0) {
        tab->cursor_row--;
        /* Scroll if needed */
        if (tab->cursor_row < tab->scroll_row) {
          tab->scroll_row = tab->cursor_row;
        }
        tui->app->needs_redraw = true;
      } else if (tab->data_offset > 0) {
        /* Load previous page */
        app_load_more(tui->app, false);
        if (tab->data) {
          tab->cursor_row = tab->data->num_rows > 0 ? tab->data->num_rows - 1 : 0;
        }
      }
      return true;

    case 'h':
    case KEY_LEFT:
      if (tab->cursor_col > 0) {
        tab->cursor_col--;
        if (tab->cursor_col < tab->scroll_col) {
          tab->scroll_col = tab->cursor_col;
        }
        tui->app->needs_redraw = true;
      }
      return true;

    case 'l':
    case KEY_RIGHT:
      if (tab->data && tab->cursor_col < tab->data->num_columns - 1) {
        tab->cursor_col++;
        tui->app->needs_redraw = true;
      }
      return true;

    case 'g':
    case KEY_HOME:
      /* Go to first row */
      if (tab->data_offset > 0) {
        tab->data_offset = 0;
        app_refresh_data(tui->app);
      }
      tab->cursor_row = 0;
      tab->scroll_row = 0;
      tui->app->needs_redraw = true;
      return true;

    case 'G':
    case KEY_END:
      /* Go to last row */
      if (tab->total_rows > PAGE_SIZE) {
        tab->data_offset = tab->total_rows - PAGE_SIZE;
        app_refresh_data(tui->app);
      }
      if (tab->data && tab->data->num_rows > 0) {
        tab->cursor_row = tab->data->num_rows - 1;
        if (tab->cursor_row >= (size_t)(tui->content_height - 1)) {
          tab->scroll_row = tab->cursor_row - (size_t)(tui->content_height - 2);
        }
      }
      tui->app->needs_redraw = true;
      return true;

    case KEY_PPAGE:
      /* Page up */
      if (tab->cursor_row >= (size_t)(tui->content_height - 1)) {
        tab->cursor_row -= (size_t)(tui->content_height - 1);
        if (tab->scroll_row >= (size_t)(tui->content_height - 1)) {
          tab->scroll_row -= (size_t)(tui->content_height - 1);
        } else {
          tab->scroll_row = 0;
        }
      } else if (tab->data_offset > 0) {
        app_load_more(tui->app, false);
      } else {
        tab->cursor_row = 0;
        tab->scroll_row = 0;
      }
      tui->app->needs_redraw = true;
      return true;

    case KEY_NPAGE:
      /* Page down */
      {
        size_t page = (size_t)(tui->content_height - 1);
        if (tab->cursor_row + page < tab->data->num_rows) {
          tab->cursor_row += page;
          tab->scroll_row += page;
          if (tab->scroll_row + page > tab->data->num_rows) {
            tab->scroll_row = tab->data->num_rows > page ? tab->data->num_rows - page : 0;
          }
        } else if (tab->data_offset + tab->data->num_rows < tab->total_rows) {
          app_load_more(tui->app, true);
        } else {
          tab->cursor_row = tab->data->num_rows > 0 ? tab->data->num_rows - 1 : 0;
        }
        tui->app->needs_redraw = true;
      }
      return true;

    case 'f':
    case '/':
      /* Toggle filter panel */
      filters_toggle(tui, &tui->filters);
      return true;

    case '+':
    case '=':
      /* Add filter based on current cell */
      if (tui->filters.visible) {
        filters_add(tui, &tui->filters);
      } else {
        /* Show filters and add one */
        tui->filters.visible = true;
        filters_add(tui, &tui->filters);
      }
      return true;

    case '\n':
    case KEY_ENTER:
      /* Start inline editing */
      if (tab->schema) {
        /* Simple inline edit - show input dialog */
        char *new_value = NULL;
        LaceValue *current = NULL;
        if (tab->data && tab->cursor_row < tab->data->num_rows &&
            tab->cursor_col < tab->data->num_columns) {
          current = &tab->data->rows[tab->cursor_row].cells[tab->cursor_col];
        }
        char *initial = current ? lace_value_to_string(current) : NULL;
        const char *col_name = tab->schema->columns[tab->cursor_col].name;

        if (dialog_input(tui, "Edit Cell", col_name, initial, &new_value)) {
          /* Update the cell */
          if (new_value && tab->schema) {
            /* Find primary key columns */
            size_t pk_count = 0;
            size_t pk_indices[16];
            for (size_t i = 0; i < tab->schema->num_columns && pk_count < 16; i++) {
              if (tab->schema->columns[i].primary_key) {
                pk_indices[pk_count++] = i;
              }
            }

            if (pk_count > 0) {
              LacePkValue *pk_values = calloc(pk_count, sizeof(LacePkValue));
              if (pk_values) {
                for (size_t i = 0; i < pk_count; i++) {
                  pk_values[i].column = tab->schema->columns[pk_indices[i]].name;
                  pk_values[i].value = tab->data->rows[tab->cursor_row].cells[pk_indices[i]];
                }

                LaceValue new_val = {0};
                new_val.type = LACE_TYPE_TEXT;
                new_val.text.data = new_value;
                new_val.text.len = strlen(new_value);

                int err = lace_update(tui->app->client, tab->conn_id, tab->table_name,
                                      pk_values, pk_count, col_name, &new_val);
                free(pk_values);

                if (err == LACE_OK) {
                  app_refresh_data(tui->app);
                  app_set_status(tui->app, "Cell updated");
                } else {
                  app_set_error(tui->app, lace_client_error(tui->app->client));
                }
              }
            } else {
              app_set_error(tui->app, "No primary key - cannot edit");
            }
          }
        }
        free(initial);
        free(new_value);
      }
      return true;

    case 'x':
    case KEY_DC: /* Delete key */
      /* Delete current row */
      if (tab->schema) {
        if (dialog_confirm(tui, "Delete Row", "Are you sure you want to delete this row?")) {
          /* Find primary key columns */
          size_t pk_count = 0;
          size_t pk_indices[16];
          for (size_t i = 0; i < tab->schema->num_columns && pk_count < 16; i++) {
            if (tab->schema->columns[i].primary_key) {
              pk_indices[pk_count++] = i;
            }
          }

          if (pk_count > 0 && tab->data && tab->cursor_row < tab->data->num_rows) {
            LacePkValue *pk_values = calloc(pk_count, sizeof(LacePkValue));
            if (pk_values) {
              for (size_t i = 0; i < pk_count; i++) {
                pk_values[i].column = tab->schema->columns[pk_indices[i]].name;
                pk_values[i].value = tab->data->rows[tab->cursor_row].cells[pk_indices[i]];
              }

              int err = lace_delete(tui->app->client, tab->conn_id, tab->table_name,
                                    pk_values, pk_count);
              free(pk_values);

              if (err == LACE_OK) {
                app_refresh_data(tui->app);
                app_set_status(tui->app, "Row deleted");
              } else {
                app_set_error(tui->app, lace_client_error(tui->app->client));
              }
            }
          } else {
            app_set_error(tui->app, "No primary key - cannot delete");
          }
        }
      }
      return true;
    }
  }

  return true;
}

/* ==========================================================================
 * Main Loop
 * ========================================================================== */

#define PAGE_SIZE 500

void tui_run(TuiState *tui) {
  if (!tui || !tui->app) return;

  /* Try to restore previous session */
  if (tui->app->num_connections == 0) {
    session_restore(tui);
  }

  /* Initial draw */
  tui_draw(tui);

  while (tui->app->running) {
    /* Redraw if needed */
    if (tui->app->needs_redraw) {
      tui_draw(tui);
    }

    /* Get input */
    int ch = wgetch(tui->in_sidebar ? tui->sidebar_win : tui->main_win);

    /* Handle input */
    if (!tui_handle_input(tui, ch)) {
      tui->app->running = false;
    }
  }

  /* Save session on exit */
  session_save(tui);
}
