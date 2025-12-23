/*
 * lace - Database Viewer and Manager
 * Sidebar rendering and input handling
 */

#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>

/* Count filtered tables */
size_t tui_count_filtered_tables(TuiState *state) {
  if (state->sidebar_filter_len == 0)
    return state->num_tables;

  size_t count = 0;
  for (size_t i = 0; i < state->num_tables; i++) {
    if (state->tables[i] &&
        tui_str_istr(state->tables[i], state->sidebar_filter)) {
      count++;
    }
  }
  return count;
}

/* Get actual table index from filtered index */
size_t tui_get_filtered_table_index(TuiState *state, size_t filtered_idx) {
  if (state->sidebar_filter_len == 0)
    return filtered_idx;

  size_t count = 0;
  for (size_t i = 0; i < state->num_tables; i++) {
    if (state->tables[i] &&
        tui_str_istr(state->tables[i], state->sidebar_filter)) {
      if (count == filtered_idx)
        return i;
      count++;
    }
  }
  return 0;
}

/* Get sidebar highlight for a table index */
size_t tui_get_sidebar_highlight_for_table(TuiState *state, size_t table_idx) {
  if (state->sidebar_filter_len == 0)
    return table_idx;

  size_t count = 0;
  for (size_t i = 0; i < state->num_tables; i++) {
    if (state->tables[i] &&
        tui_str_istr(state->tables[i], state->sidebar_filter)) {
      if (i == table_idx)
        return count;
      count++;
    }
  }
  return 0; /* Table not in filtered list, default to first */
}

/* Update sidebar name scroll animation */
void tui_update_sidebar_scroll_animation(TuiState *state) {
  if (!state || !state->sidebar_focused || !state->tables)
    return;

  /* Reset scroll when highlight changes */
  if (state->sidebar_highlight != state->sidebar_last_highlight) {
    state->sidebar_name_scroll = 0;
    state->sidebar_name_scroll_dir = 1;
    state->sidebar_name_scroll_delay = 3; /* Initial pause */
    state->sidebar_last_highlight = state->sidebar_highlight;
    return;
  }

  /* Get highlighted table name */
  size_t actual_idx =
      tui_get_filtered_table_index(state, state->sidebar_highlight);
  if (actual_idx >= state->num_tables)
    return;

  const char *name = state->tables[actual_idx];
  if (!name)
    return;

  int max_name_len = SIDEBAR_WIDTH - 4;
  if (max_name_len < 1)
    max_name_len = 1;
  int name_len = (int)strlen(name);

  /* Only animate if name is truncated */
  if (name_len <= max_name_len) {
    state->sidebar_name_scroll = 0;
    return;
  }

  int max_scroll = name_len - max_name_len;

  /* Handle pause at ends */
  if (state->sidebar_name_scroll_delay > 0) {
    state->sidebar_name_scroll_delay--;
    return;
  }

  /* Update scroll position */
  if (state->sidebar_name_scroll_dir > 0) {
    /* Scrolling right (showing more of the end) */
    if ((int)state->sidebar_name_scroll < max_scroll) {
      state->sidebar_name_scroll++;
    } else {
      /* Reached end, pause and reverse */
      state->sidebar_name_scroll_dir = -1;
      state->sidebar_name_scroll_delay = 5;
    }
  } else {
    /* Scrolling left (back to start) */
    if (state->sidebar_name_scroll > 0) {
      state->sidebar_name_scroll--;
    } else {
      /* Reached start, pause and reverse */
      state->sidebar_name_scroll_dir = 1;
      state->sidebar_name_scroll_delay = 5;
    }
  }
}

/* Handle sidebar input when focused */
bool tui_handle_sidebar_input(TuiState *state, int ch) {
  if (!state->sidebar_focused)
    return false;

  /* Handle filter input mode */
  if (state->sidebar_filter_active) {
    switch (ch) {
    case 27: /* Escape */
      state->sidebar_filter_active = false;
      break;

    case '\n':
    case KEY_ENTER:
    case KEY_DOWN:
      state->sidebar_filter_active = false;
      state->sidebar_highlight = 0;
      state->sidebar_scroll = 0;
      break;

    case KEY_BACKSPACE:
    case 127:
    case 8:
      if (state->sidebar_filter_len > 0) {
        state->sidebar_filter[--state->sidebar_filter_len] = '\0';
        state->sidebar_highlight = 0;
        state->sidebar_scroll = 0;
      }
      break;

    default:
      if (ch >= 32 && ch < 127 &&
          state->sidebar_filter_len < sizeof(state->sidebar_filter) - 1) {
        state->sidebar_filter[state->sidebar_filter_len++] = (char)ch;
        state->sidebar_filter[state->sidebar_filter_len] = '\0';
        state->sidebar_highlight = 0;
        state->sidebar_scroll = 0;
      }
      break;
    }
    /* Sync focus state to Tab so it persists across tab switches */
    tab_sync_focus(state);
    return true;
  }

  size_t filtered_count = tui_count_filtered_tables(state);

  switch (ch) {
  case KEY_UP:
  case 'k':
    if (state->sidebar_highlight > 0) {
      state->sidebar_highlight--;
    } else {
      /* At top of list, move to filter field */
      state->sidebar_filter_active = true;
    }
    break;

  case KEY_DOWN:
  case 'j':
    if (filtered_count > 0 && state->sidebar_highlight < filtered_count - 1) {
      state->sidebar_highlight++;
    }
    break;

  case KEY_RIGHT:
  case 'l':
    /* Save sidebar position and move focus back to filters or table view */
    state->sidebar_last_position = state->sidebar_highlight;
    state->sidebar_focused = false;
    if (state->filters_visible && state->filters_was_focused) {
      state->filters_focused = true;
    }
    break;

  case '\n':
  case KEY_ENTER:
    /* Select the highlighted table - open in current tab or create first tab */
    if (filtered_count > 0 && state->sidebar_highlight < filtered_count) {
      size_t actual_idx =
          tui_get_filtered_table_index(state, state->sidebar_highlight);

      Workspace *ws = TUI_WORKSPACE(state);
      if (!ws || ws->num_tabs == 0) {
        /* No tabs yet - create first one */
        tab_create(state, actual_idx);
      } else {
        Tab *tab = TUI_TAB(state);
        if (tab && tab->type == TAB_TYPE_QUERY) {
          /* Query tab active - check if table tab already exists for same connection */
          bool found = false;
          size_t current_conn = tab->connection_index;
          for (size_t i = 0; i < ws->num_tabs; i++) {
            Tab *t = &ws->tabs[i];
            if (t->type == TAB_TYPE_TABLE && t->table_index == actual_idx &&
                t->connection_index == current_conn) {
              /* Found existing tab for same table and connection - switch to it */
              tab_switch(state, i);
              found = true;
              break;
            }
          }
          if (!found) {
            /* No existing tab - create new one */
            tab_create(state, actual_idx);
          }
        } else if (tab && actual_idx != state->current_table) {
          /* Replace current tab's table */
          /* Free old data */
          db_result_free(tab->data);
          db_schema_free(tab->schema);
          free(tab->col_widths);
          free(tab->table_name);

          /* Update tab */
          tab->table_index = actual_idx;
          tab->table_name = str_dup(state->tables[actual_idx]);

          /* Clear filters when switching to a different table */
          filters_clear(&tab->filters);

          /* Clear state and load new table */
          state->data = NULL;
          state->schema = NULL;
          state->col_widths = NULL;
          state->num_col_widths = 0;
          state->current_table = actual_idx;

          tui_load_table_data(state, state->tables[actual_idx]);

          /* Save to tab */
          tab->data = state->data;
          tab->schema = state->schema;
          tab->col_widths = state->col_widths;
          tab->num_col_widths = state->num_col_widths;
          tab->total_rows = state->total_rows;
          tab->loaded_offset = state->loaded_offset;
          tab->loaded_count = state->loaded_count;
          tab->cursor_row = state->cursor_row;
          tab->cursor_col = state->cursor_col;
          tab->scroll_row = state->scroll_row;
          tab->scroll_col = state->scroll_col;
        }
      }
      state->sidebar_focused = false;
    }
    break;

  case '+':
  case '=':
    /* Open highlighted table in new tab */
    if (filtered_count > 0 && state->sidebar_highlight < filtered_count) {
      size_t actual_idx =
          tui_get_filtered_table_index(state, state->sidebar_highlight);
      tab_create(state, actual_idx);
      state->sidebar_focused = false;
    }
    break;

  case 'f':
  case 'F':
  case '/':
    /* Activate filter */
    state->sidebar_filter_active = true;
    break;

  case 27: /* Escape */
    /* Clear filter if set, otherwise unfocus sidebar */
    if (state->sidebar_filter_len > 0) {
      state->sidebar_filter[0] = '\0';
      state->sidebar_filter_len = 0;
      state->sidebar_highlight = 0;
      state->sidebar_scroll = 0;
    } else {
      state->sidebar_focused = false;
    }
    break;

  case 't':
  case 'T':
  case KEY_F(9): /* F9 - toggle sidebar */
    /* Close sidebar immediately */
    state->sidebar_visible = false;
    state->sidebar_focused = false;
    state->sidebar_filter[0] = '\0';
    state->sidebar_filter_len = 0;
    tui_recreate_windows(state);
    tui_calculate_column_widths(state);
    break;

  case '?':
  case KEY_F(1):
    tui_show_help(state);
    break;

  case 'q':
  case 'Q':
  case 'p':
  case 'P':
  case 'r':
  case 'R':
  case '[':
  case ']':
  case '{':
  case '}':
  case '-':
  case '_':
  case 's':
  case 'S':
  case 'c':
  case 'C':
  case 'm':
  case 'M':
  case 'b':
  case 'B':
  case 7:         /* Ctrl+G - Go to row */
  case KEY_F(2):  /* Connect */
  case KEY_F(3):  /* Schema */
  case KEY_F(5):  /* Go to row */
  case KEY_F(6):  /* Next tab */
  case KEY_F(7):  /* Previous tab */
  case KEY_F(10): /* Quit */
  case 24:        /* Ctrl+X - Quit */
    /* Pass through to main handler for global hotkeys */
    return false;

  default:
    break;
  }

  /* Sync focus state to Tab so it persists across tab switches */
  tab_sync_focus(state);
  return true;
}

/* Draw sidebar */
void tui_draw_sidebar(TuiState *state) {
  if (!state || !state->sidebar_win || !state->sidebar_visible)
    return;

  werase(state->sidebar_win);

  /* Draw border */
  wattron(state->sidebar_win, COLOR_PAIR(COLOR_BORDER));
  box(state->sidebar_win, 0, 0);
  wattroff(state->sidebar_win, COLOR_PAIR(COLOR_BORDER));

  /* Title */
  wattron(state->sidebar_win, A_BOLD);
  mvwprintw(state->sidebar_win, 0, 2, " Tables ");
  wattroff(state->sidebar_win, A_BOLD);

  int y = 1;
  int max_name_len = SIDEBAR_WIDTH - 4;

  /* Draw filter input - use COLOR_EDIT for active filter (black on yellow) */
  if (state->sidebar_filter_active) {
    wattron(state->sidebar_win, COLOR_PAIR(COLOR_EDIT));
  }
  mvwprintw(state->sidebar_win, y, 1, "/%-*.*s", max_name_len, max_name_len,
            state->sidebar_filter_len > 0 ? state->sidebar_filter : "");
  if (state->sidebar_filter_active) {
    wattroff(state->sidebar_win, COLOR_PAIR(COLOR_EDIT));
  }
  int filter_y = y;  /* Remember filter line position for cursor */
  y++;

  /* Separator */
  mvwhline(state->sidebar_win, y, 1, ACS_HLINE, SIDEBAR_WIDTH - 2);
  y++;

  if (!state->tables || state->num_tables == 0) {
    mvwprintw(state->sidebar_win, y, 2, "(no tables)");
    /* Position cursor in filter field if active */
    if (state->sidebar_filter_active) {
      curs_set(1);
      wmove(state->sidebar_win, filter_y, 2 + (int)state->sidebar_filter_len);
    }
    wrefresh(state->sidebar_win);
    return;
  }

  /* Get actual sidebar window height */
  int win_height, win_width;
  getmaxyx(state->sidebar_win, win_height, win_width);
  (void)win_width;

  /* Content area height: window height minus top border(1), filter(1), separator(1), bottom border(1) */
  int list_height = win_height - 4;

  /* Count filtered tables */
  size_t filtered_count = 0;
  for (size_t i = 0; i < state->num_tables; i++) {
    if (state->tables[i] &&
        (state->sidebar_filter_len == 0 ||
         tui_str_istr(state->tables[i], state->sidebar_filter))) {
      filtered_count++;
    }
  }

  if (filtered_count == 0) {
    mvwprintw(state->sidebar_win, y, 2, "(no matches)");
    /* Position cursor in filter field if active */
    if (state->sidebar_filter_active) {
      curs_set(1);
      wmove(state->sidebar_win, filter_y, 2 + (int)state->sidebar_filter_len);
    }
    wrefresh(state->sidebar_win);
    return;
  }

  /* Adjust scroll if highlight is out of view */
  if (state->sidebar_highlight < state->sidebar_scroll) {
    state->sidebar_scroll = state->sidebar_highlight;
  } else if (state->sidebar_highlight >=
             state->sidebar_scroll + (size_t)list_height) {
    state->sidebar_scroll = state->sidebar_highlight - list_height + 1;
  }

  /* Draw filtered tables */
  size_t filtered_idx = 0;
  for (size_t i = 0; i < state->num_tables && y < win_height - 1; i++) {
    const char *name = state->tables[i];
    if (!name)
      continue;

    /* Apply filter */
    if (state->sidebar_filter_len > 0 &&
        !tui_str_istr(name, state->sidebar_filter)) {
      continue;
    }

    /* Skip items before scroll offset */
    if (filtered_idx < state->sidebar_scroll) {
      filtered_idx++;
      continue;
    }

    bool is_highlighted = (filtered_idx == state->sidebar_highlight);
    bool is_current = (i == state->current_table);

    if (is_highlighted && state->sidebar_focused &&
        !state->sidebar_filter_active) {
      wattron(state->sidebar_win, A_REVERSE);
    }

    /* Truncate name if too long */
    char display_name[SIDEBAR_WIDTH];
    int name_len = (int)strlen(name);
    if (name_len > max_name_len) {
      if (is_highlighted && state->sidebar_focused &&
          !state->sidebar_filter_active) {
        /* Apply scroll animation for highlighted item */
        size_t scroll = state->sidebar_name_scroll;
        if (scroll > (size_t)(name_len - max_name_len)) {
          scroll = name_len - max_name_len;
        }
        snprintf(display_name, sizeof(display_name), "%.*s", max_name_len,
                 name + scroll);
      } else {
        snprintf(display_name, sizeof(display_name), "%.*s..", max_name_len - 2,
                 name);
      }
    } else {
      snprintf(display_name, sizeof(display_name), "%s", name);
    }

    if (is_current) {
      wattron(state->sidebar_win, A_BOLD);
    }

    mvwprintw(state->sidebar_win, y, 2, "%-*s", max_name_len, display_name);

    if (is_current) {
      wattroff(state->sidebar_win, A_BOLD);
    }
    if (is_highlighted && state->sidebar_focused &&
        !state->sidebar_filter_active) {
      wattroff(state->sidebar_win, A_REVERSE);
    }

    y++;
    filtered_idx++;
  }

  /* Position cursor in filter field if active, before refresh */
  if (state->sidebar_filter_active) {
    curs_set(1);
    wmove(state->sidebar_win, filter_y, 2 + (int)state->sidebar_filter_len);
  }

  wrefresh(state->sidebar_win);
}
