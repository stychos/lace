/*
 * Lace ncurses frontend
 * Sidebar - table list with filtering
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "sidebar.h"
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ==========================================================================
 * Helpers
 * ========================================================================== */

/* Case-insensitive substring match */
static bool str_contains_ci(const char *haystack, const char *needle) {
  if (!haystack || !needle || !*needle) return true;

  size_t needle_len = strlen(needle);
  size_t haystack_len = strlen(haystack);

  if (needle_len > haystack_len) return false;

  for (size_t i = 0; i <= haystack_len - needle_len; i++) {
    bool match = true;
    for (size_t j = 0; j < needle_len; j++) {
      if (tolower((unsigned char)haystack[i + j]) !=
          tolower((unsigned char)needle[j])) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

/* ==========================================================================
 * Sidebar Functions
 * ========================================================================== */

size_t sidebar_count_filtered(TuiState *tui, SidebarState *ss) {
  Connection *conn = app_current_connection(tui->app);
  if (!conn || !conn->tables) return 0;

  if (!ss->filter[0]) {
    return conn->num_tables;
  }

  size_t count = 0;
  for (size_t i = 0; i < conn->num_tables; i++) {
    if (str_contains_ci(conn->tables[i], ss->filter)) {
      count++;
    }
  }
  return count;
}

const char *sidebar_get_table(TuiState *tui, SidebarState *ss, size_t index) {
  Connection *conn = app_current_connection(tui->app);
  if (!conn || !conn->tables) return NULL;

  if (!ss->filter[0]) {
    return index < conn->num_tables ? conn->tables[index] : NULL;
  }

  size_t count = 0;
  for (size_t i = 0; i < conn->num_tables; i++) {
    if (str_contains_ci(conn->tables[i], ss->filter)) {
      if (count == index) return conn->tables[i];
      count++;
    }
  }
  return NULL;
}

void sidebar_draw(TuiState *tui, SidebarState *ss, WINDOW *win) {
  if (!win || !tui->app->sidebar_visible) return;

  int win_rows, win_cols;
  getmaxyx(win, win_rows, win_cols);

  werase(win);

  /* Draw border */
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  box(win, 0, 0);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));

  Connection *conn = app_current_connection(tui->app);
  size_t filtered_count = sidebar_count_filtered(tui, ss);

  /* Draw title */
  wattron(win, A_BOLD);
  if (ss->filter[0]) {
    mvwprintw(win, 1, 1, "Tables (%zu/%zu)",
              filtered_count, conn ? conn->num_tables : 0);
  } else {
    mvwprintw(win, 1, 1, "Tables (%zu)", conn ? conn->num_tables : 0);
  }
  wattroff(win, A_BOLD);

  /* Draw filter input if active */
  int content_start = 2;
  if (ss->filtering) {
    wattron(win, A_UNDERLINE);
    mvwprintw(win, 2, 1, "/%s", ss->filter);
    int pad = win_cols - 3 - (int)ss->filter_len;
    if (pad > 0) {
      wprintw(win, "%*s", pad, "");
    }
    wattroff(win, A_UNDERLINE);
    content_start = 3;
  } else if (ss->filter[0]) {
    wattron(win, COLOR_PAIR(COLOR_TITLE));
    mvwprintw(win, 2, 1, "/%s", ss->filter);
    wattroff(win, COLOR_PAIR(COLOR_TITLE));
    content_start = 3;
  }

  if (!conn || !conn->tables || conn->num_tables == 0) {
    mvwprintw(win, content_start + 1, 1, "(no tables)");
    wrefresh(win);
    return;
  }

  /* Calculate visible area */
  int visible_rows = win_rows - content_start - 1;
  if (visible_rows < 1) visible_rows = 1;

  /* Adjust scroll to keep selection visible */
  AppState *app = tui->app;

  /* Map selection to filtered index */
  size_t filtered_selection = 0;
  if (ss->filter[0]) {
    /* Find filtered index for current selection */
    size_t count = 0;
    for (size_t i = 0; i < conn->num_tables; i++) {
      if (str_contains_ci(conn->tables[i], ss->filter)) {
        if (conn->tables[i] == conn->tables[app->sidebar_selected]) {
          filtered_selection = count;
          break;
        }
        count++;
      }
    }
  } else {
    filtered_selection = app->sidebar_selected;
  }

  /* Adjust scroll */
  if (filtered_selection < app->sidebar_scroll) {
    app->sidebar_scroll = filtered_selection;
  }
  if (filtered_selection >= app->sidebar_scroll + (size_t)visible_rows) {
    app->sidebar_scroll = filtered_selection - visible_rows + 1;
  }

  /* Draw table list */
  size_t drawn = 0;
  size_t filtered_idx = 0;

  for (size_t i = 0; i < conn->num_tables && drawn < (size_t)visible_rows; i++) {
    const char *table = conn->tables[i];

    /* Apply filter */
    if (ss->filter[0] && !str_contains_ci(table, ss->filter)) {
      continue;
    }

    /* Skip to scroll position */
    if (filtered_idx < app->sidebar_scroll) {
      filtered_idx++;
      continue;
    }

    int y = content_start + (int)drawn;
    bool selected = (i == app->sidebar_selected);

    /* Highlight current table in sidebar */
    if (selected && tui->in_sidebar) {
      wattron(win, COLOR_PAIR(COLOR_SELECTED));
    } else if (selected) {
      wattron(win, A_BOLD);
    }

    /* Draw indicator */
    mvwaddch(win, y, 1, selected ? ACS_RARROW : ' ');

    /* Draw table name (truncated) */
    int name_width = win_cols - 4;
    mvwprintw(win, y, 2, "%-*.*s", name_width, name_width, table);

    if (selected && tui->in_sidebar) {
      wattroff(win, COLOR_PAIR(COLOR_SELECTED));
    } else if (selected) {
      wattroff(win, A_BOLD);
    }

    drawn++;
    filtered_idx++;
  }

  /* Scroll indicators */
  if (app->sidebar_scroll > 0) {
    mvwaddch(win, content_start - 1, win_cols - 2, ACS_UARROW);
  }
  if (app->sidebar_scroll + visible_rows < filtered_count) {
    mvwaddch(win, win_rows - 2, win_cols - 2, ACS_DARROW);
  }

  wrefresh(win);
}

bool sidebar_handle_input(TuiState *tui, SidebarState *ss, int ch) {
  if (!tui->in_sidebar && !ss->filtering) return false;

  Connection *conn = app_current_connection(tui->app);
  if (!conn) return false;

  /* Handle filter mode input */
  if (ss->filtering) {
    switch (ch) {
    case '\n':
    case KEY_ENTER:
      ss->filtering = false;
      tui->app->needs_redraw = true;
      return true;

    case 27: /* Escape */
      ss->filtering = false;
      ss->filter[0] = '\0';
      ss->filter_len = 0;
      tui->app->needs_redraw = true;
      return true;

    case KEY_BACKSPACE:
    case 127:
    case 8:
      if (ss->filter_len > 0) {
        ss->filter[--ss->filter_len] = '\0';
        /* Reset selection when filter changes */
        tui->app->sidebar_selected = 0;
        tui->app->sidebar_scroll = 0;
        tui->app->needs_redraw = true;
      }
      return true;

    default:
      if (ch >= 32 && ch < 127 && ss->filter_len < sizeof(ss->filter) - 1) {
        ss->filter[ss->filter_len++] = (char)ch;
        ss->filter[ss->filter_len] = '\0';
        /* Reset selection when filter changes */
        tui->app->sidebar_selected = 0;
        tui->app->sidebar_scroll = 0;
        tui->app->needs_redraw = true;
        return true;
      }
      break;
    }
    return false;
  }

  /* Normal sidebar navigation */
  size_t filtered_count = sidebar_count_filtered(tui, ss);

  switch (ch) {
  case 'j':
  case KEY_DOWN:
    if (filtered_count > 0) {
      /* Find next matching table */
      size_t start = tui->app->sidebar_selected;
      for (size_t i = start + 1; i < conn->num_tables; i++) {
        if (!ss->filter[0] || str_contains_ci(conn->tables[i], ss->filter)) {
          tui->app->sidebar_selected = i;
          tui->app->needs_redraw = true;
          break;
        }
      }
    }
    return true;

  case 'k':
  case KEY_UP:
    if (filtered_count > 0 && tui->app->sidebar_selected > 0) {
      /* Find previous matching table */
      for (size_t i = tui->app->sidebar_selected - 1; ; i--) {
        if (!ss->filter[0] || str_contains_ci(conn->tables[i], ss->filter)) {
          tui->app->sidebar_selected = i;
          tui->app->needs_redraw = true;
          break;
        }
        if (i == 0) break;
      }
    }
    return true;

  case '/':
  case 'f':
    sidebar_start_filter(tui, ss);
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

  case 27: /* Escape */
    if (ss->filter[0]) {
      sidebar_clear_filter(tui, ss);
    } else {
      tui->in_sidebar = false;
      tui->app->needs_redraw = true;
    }
    return true;
  }

  return false;
}

void sidebar_start_filter(TuiState *tui, SidebarState *ss) {
  ss->filtering = true;
  tui->app->needs_redraw = true;
}

void sidebar_clear_filter(TuiState *tui, SidebarState *ss) {
  ss->filter[0] = '\0';
  ss->filter_len = 0;
  ss->filtering = false;
  tui->app->sidebar_scroll = 0;
  tui->app->needs_redraw = true;
}
