/*
 * lace - Database Viewer and Manager
 * Drawing functions
 */

#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>

void tui_draw_header(TuiState *state) {
  if (!state || !state->header_win)
    return;

  werase(state->header_win);
  wbkgd(state->header_win, COLOR_PAIR(COLOR_HEADER));

  /* Draw title */
  mvwprintw(state->header_win, 0, 1, " lace ");

  if (state->conn && state->conn->database) {
    mvwprintw(state->header_win, 0, 8, "| %s ", state->conn->database);
  }

  /* Help hint */
  const char *help = "q:Quit t:Sidebar /:GoTo []:Tabs -:Close ?:Help";
  mvwprintw(state->header_win, 0, state->term_cols - (int)strlen(help) - 1,
            "%s", help);

  wrefresh(state->header_win);
}

void tui_draw_table(TuiState *state) {
  if (!state || !state->main_win)
    return;

  werase(state->main_win);

  /* Get actual window dimensions */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);

  if (!state->data || state->data->num_columns == 0) {
    mvwprintw(state->main_win, win_rows / 2, (win_cols - 7) / 2, "No data");
    wrefresh(state->main_win);
    return;
  }

  int y = 0;
  int x = 0;

  /* Draw column headers */
  wattron(state->main_win, A_BOLD | COLOR_PAIR(COLOR_BORDER));
  mvwhline(state->main_win, y, 0, ACS_HLINE, win_cols);
  y++;

  wattron(state->main_win, A_BOLD);
  x = 1;
  for (size_t col = state->scroll_col; col < state->data->num_columns; col++) {
    int width = tui_get_column_width(state, col);
    if (x + width + 3 > win_cols)
      break;

    /* Only show column header selection when sidebar is not focused */
    if (col == state->cursor_col && !state->sidebar_focused) {
      wattron(state->main_win, A_REVERSE);
    }

    const char *name = state->data->columns[col].name;
    mvwprintw(state->main_win, y, x, "%-*.*s", width, width, name ? name : "");

    if (col == state->cursor_col && !state->sidebar_focused) {
      wattroff(state->main_win, A_REVERSE);
    }

    x += width + 1;
    mvwaddch(state->main_win, y, x - 1, ACS_VLINE);
  }
  wattroff(state->main_win, A_BOLD);
  y++;

  wattron(state->main_win, COLOR_PAIR(COLOR_BORDER));
  mvwhline(state->main_win, y, 0, ACS_HLINE, win_cols);
  wattroff(state->main_win, COLOR_PAIR(COLOR_BORDER));
  y++;

  /* Draw data rows */
  if (!state->data->rows) {
    wrefresh(state->main_win);
    return;
  }

  for (size_t row = state->scroll_row;
       row < state->data->num_rows && y < win_rows; row++) {

    Row *r = &state->data->rows[row];
    if (!r->cells)
      continue;

    x = 1;
    /* Only show row selection when sidebar is not focused */
    bool is_selected_row =
        (row == state->cursor_row) && !state->sidebar_focused;

    if (is_selected_row) {
      wattron(state->main_win, A_BOLD);
    }

    for (size_t col = state->scroll_col;
         col < state->data->num_columns && col < r->num_cells; col++) {
      int width = tui_get_column_width(state, col);
      if (x + width + 3 > win_cols)
        break;

      /* Only show selection when sidebar is not focused */
      bool is_selected = is_selected_row && (col == state->cursor_col) &&
                         !state->sidebar_focused;
      bool is_editing = is_selected && state->editing;

      if (is_editing) {
        /* Draw edit field with distinctive background */
        wattron(state->main_win, COLOR_PAIR(COLOR_EDIT));
        mvwhline(state->main_win, y, x, ' ', width);

        /* Draw the edit buffer */
        const char *buf = state->edit_buffer ? state->edit_buffer : "";
        size_t buf_len = strlen(buf);

        /* Calculate scroll for long text */
        size_t scroll = 0;
        if (state->edit_pos >= (size_t)(width - 1)) {
          scroll = state->edit_pos - width + 2;
        }

        /* Draw visible portion of text */
        size_t draw_len = buf_len > scroll ? buf_len - scroll : 0;
        if (draw_len > (size_t)width)
          draw_len = width;
        if (draw_len > 0) {
          mvwaddnstr(state->main_win, y, x, buf + scroll, (int)draw_len);
        }

        wattroff(state->main_win, COLOR_PAIR(COLOR_EDIT));

        /* Draw cursor character with reverse video for visibility */
        int cursor_x = x + (int)(state->edit_pos - scroll);
        if (cursor_x >= x && cursor_x < x + width) {
          char cursor_char =
              (state->edit_pos < buf_len) ? buf[state->edit_pos] : ' ';
          wattron(state->main_win, A_REVERSE | A_BOLD);
          mvwaddch(state->main_win, y, cursor_x, cursor_char);
          wattroff(state->main_win, A_REVERSE | A_BOLD);
          wmove(state->main_win, y, cursor_x);
        }
      } else if (is_selected) {
        wattron(state->main_win, COLOR_PAIR(COLOR_SELECTED));

        DbValue *val = &r->cells[col];
        if (val->is_null) {
          mvwprintw(state->main_win, y, x, "%-*s", width, "NULL");
        } else {
          char *str = db_value_to_string(val);
          if (str) {
            char *safe = tui_sanitize_for_display(str);
            mvwprintw(state->main_win, y, x, "%-*.*s", width, width,
                      safe ? safe : str);
            free(safe);
            free(str);
          }
        }

        wattroff(state->main_win, COLOR_PAIR(COLOR_SELECTED));
      } else {
        DbValue *val = &r->cells[col];
        if (val->is_null) {
          wattron(state->main_win, COLOR_PAIR(COLOR_NULL));
          mvwprintw(state->main_win, y, x, "%-*s", width, "NULL");
          wattroff(state->main_win, COLOR_PAIR(COLOR_NULL));
        } else {
          char *str = db_value_to_string(val);
          if (str) {
            char *safe = tui_sanitize_for_display(str);
            if (val->type == DB_TYPE_INT || val->type == DB_TYPE_FLOAT) {
              wattron(state->main_win, COLOR_PAIR(COLOR_NUMBER));
            }
            mvwprintw(state->main_win, y, x, "%-*.*s", width, width,
                      safe ? safe : str);
            if (val->type == DB_TYPE_INT || val->type == DB_TYPE_FLOAT) {
              wattroff(state->main_win, COLOR_PAIR(COLOR_NUMBER));
            }
            free(safe);
            free(str);
          }
        }
      }

      x += width + 1;
      mvwaddch(state->main_win, y, x - 1, ACS_VLINE);
    }

    if (is_selected_row) {
      wattroff(state->main_win, A_BOLD);
    }

    y++;
  }

  wrefresh(state->main_win);
}

void tui_draw_status(TuiState *state) {
  if (!state || !state->status_win)
    return;

  werase(state->status_win);

  if (state->status_is_error) {
    wbkgd(state->status_win, COLOR_PAIR(COLOR_ERROR));
  } else {
    wbkgd(state->status_win, COLOR_PAIR(COLOR_STATUS));
  }

  /* Left: show table name when sidebar focused, otherwise column info */
  if (state->sidebar_focused && state->tables && state->num_tables > 0) {
    /* Show highlighted table name */
    size_t actual_idx =
        tui_get_filtered_table_index(state, state->sidebar_highlight);
    if (actual_idx < state->num_tables && state->tables[actual_idx]) {
      const char *name = state->tables[actual_idx];
      mvwprintw(state->status_win, 0, 1, "%s", name);
    }
  } else if (state->schema && state->cursor_col < state->schema->num_columns) {
    ColumnDef *col = &state->schema->columns[state->cursor_col];

    /* Build column info string */
    char info[256];
    int pos = 0;

    /* Column name */
    pos += snprintf(info + pos, sizeof(info) - pos, "%s",
                    col->name ? col->name : "?");

    /* Type */
    if (col->type_name) {
      pos += snprintf(info + pos, sizeof(info) - pos, " : %s", col->type_name);
    }

    /* Flags */
    if (col->primary_key) {
      pos += snprintf(info + pos, sizeof(info) - pos, " [PK]");
    }
    if (!col->nullable) {
      pos += snprintf(info + pos, sizeof(info) - pos, " NOT NULL");
    }
    if (col->default_val) {
      snprintf(info + pos, sizeof(info) - pos, " DEFAULT %s", col->default_val);
    }

    mvwprintw(state->status_win, 0, 1, "%s", info);
  }

  /* Center: status/error message */
  if (state->status_msg) {
    int msg_len = (int)strlen(state->status_msg);
    int center_x = (state->term_cols - msg_len) / 2;
    if (center_x < 1)
      center_x = 1;
    mvwprintw(state->status_win, 0, center_x, "%s", state->status_msg);
  }

  /* Right: row position */
  if (state->data) {
    char pos[64];
    size_t actual_row = state->loaded_offset + state->cursor_row + 1;
    size_t total =
        state->total_rows > 0 ? state->total_rows : state->data->num_rows;
    snprintf(pos, sizeof(pos), "Row %zu/%zu", actual_row, total);
    mvwprintw(state->status_win, 0, state->term_cols - (int)strlen(pos) - 1,
              "%s", pos);
  }

  wrefresh(state->status_win);
}

/* Handle mouse events */
bool tui_handle_mouse_event(TuiState *state) {
  MEVENT event;
  if (getmouse(&event) != OK)
    return false;

  int mouse_y = event.y;
  int mouse_x = event.x;
  bool is_double = (event.bstate & BUTTON1_DOUBLE_CLICKED) != 0;
  bool is_click = (event.bstate & BUTTON1_CLICKED) != 0;
  bool is_scroll_up = (event.bstate & BUTTON4_PRESSED) != 0;
  bool is_scroll_down = (event.bstate & BUTTON5_PRESSED) != 0;

  /* Determine click location */
  int sidebar_width = state->sidebar_visible ? SIDEBAR_WIDTH : 0;

  /* Handle scroll wheel - scroll relative to cursor */
  if (is_scroll_up || is_scroll_down) {
    /* Only scroll in main table area */
    if (mouse_x >= sidebar_width && state->data && state->data->num_rows > 0) {
      int scroll_amount = 3; /* Scroll 3 rows at a time */

      if (is_scroll_up) {
        /* Scroll up - move cursor up */
        if (state->cursor_row >= (size_t)scroll_amount) {
          state->cursor_row -= scroll_amount;
        } else {
          state->cursor_row = 0;
        }
      } else {
        /* Scroll down - move cursor down */
        state->cursor_row += scroll_amount;
        if (state->cursor_row >= state->data->num_rows) {
          state->cursor_row = state->data->num_rows - 1;
        }
      }

      /* Adjust scroll to keep cursor visible */
      int visible_rows = state->term_rows - 6;
      if (visible_rows < 1)
        visible_rows = 1;
      if (state->cursor_row < state->scroll_row) {
        state->scroll_row = state->cursor_row;
      } else if (state->cursor_row >=
                 state->scroll_row + (size_t)visible_rows) {
        state->scroll_row = state->cursor_row - visible_rows + 1;
      }

      /* Check if we need to load more rows (pagination) */
      tui_check_load_more(state);

      state->sidebar_focused = false;
    }
    return true;
  }

  if (!is_click && !is_double)
    return false;

  /* Check if click is on tab bar (row 1) */
  if (mouse_y == 1 && state->num_workspaces > 0) {
    /* If currently editing, save the edit first */
    if (state->editing) {
      tui_confirm_edit(state);
    }

    /* Calculate which tab was clicked based on x position */
    int x = 0;
    for (size_t i = 0; i < state->num_workspaces; i++) {
      Workspace *ws = &state->workspaces[i];
      if (!ws->active)
        continue;

      const char *name = ws->table_name ? ws->table_name : "?";
      int tab_width = (int)strlen(name) + 4; /* " name  " with padding */

      if (mouse_x >= x && mouse_x < x + tab_width) {
        /* Clicked on this tab - switch to it */
        if (i != state->current_workspace) {
          workspace_switch(state, i);
          state->sidebar_focused = false;
        }
        return true;
      }

      x += tab_width;
      if (x > state->term_cols)
        break;
    }
    return true; /* Clicked on tab bar but not on a specific tab */
  }

  /* Check if click is in sidebar */
  if (state->sidebar_visible && mouse_x < sidebar_width) {
    /* If currently editing, save the edit first */
    if (state->editing) {
      tui_confirm_edit(state);
    }

    /* Sidebar layout (inside sidebar_win which starts at screen y=2):
       row 0 = border+title, row 1 = filter, row 2 = separator, row 3+ = table
       list */
    int sidebar_row = mouse_y - 2; /* Convert to sidebar_win coordinates */

    /* Click on filter field (row 1 in sidebar_win = screen row 2) */
    if (sidebar_row == 1) {
      state->sidebar_focused = true;
      state->sidebar_filter_active = true;
      return true;
    }

    /* Clicking elsewhere in sidebar deactivates filter */
    state->sidebar_filter_active = false;

    /* Click on table list */
    int list_start_y = 3; /* First table entry row in sidebar window */
    int clicked_row = sidebar_row - list_start_y;

    if (clicked_row >= 0) {
      size_t filtered_count = tui_count_filtered_tables(state);

      size_t target_idx = state->sidebar_scroll + (size_t)clicked_row;
      if (target_idx < filtered_count) {
        /* Find the actual table index */
        size_t actual_idx = tui_get_filtered_table_index(state, target_idx);
        if (actual_idx < state->num_tables) {
          /* Update sidebar highlight */
          state->sidebar_highlight = target_idx;
          state->sidebar_focused = true;

          if (is_double) {
            /* Double-click: open in new tab */
            workspace_create(state, actual_idx);
            state->sidebar_focused = false;
          } else {
            /* Single click: change current tab's table */
            if (state->num_workspaces > 0) {
              /* Update current workspace with new table */
              Workspace *ws = &state->workspaces[state->current_workspace];
              if (state->current_table != actual_idx) {
                state->current_table = actual_idx;
                /* Free old workspace data */
                free(ws->table_name);
                ws->table_name = str_dup(state->tables[actual_idx]);
                ws->table_index = actual_idx;
                /* Load new table data */
                tui_load_table_data(state, state->tables[actual_idx]);
                /* Update workspace with new data */
                ws->data = state->data;
                ws->schema = state->schema;
                ws->col_widths = state->col_widths;
                ws->num_col_widths = state->num_col_widths;
                ws->total_rows = state->total_rows;
                ws->loaded_offset = state->loaded_offset;
                ws->loaded_count = state->loaded_count;
                ws->cursor_row = 0;
                ws->cursor_col = 0;
                ws->scroll_row = 0;
                ws->scroll_col = 0;
              }
            } else {
              /* No tabs yet - create first one */
              workspace_create(state, actual_idx);
            }
          }
          return true;
        }
      }
    }
    return true;
  }

  /* Check if click is in main table area */
  if (mouse_x >= sidebar_width) {
    /* Clicking in main area deactivates sidebar filter */
    state->sidebar_filter_active = false;

    /* If currently editing, save the edit first */
    if (state->editing) {
      tui_confirm_edit(state);
    }

    if (!state->data || state->data->num_rows == 0) {
      return true; /* No data to select, but filter is deactivated */
    }

    /* Adjust coordinates relative to main window (starts at screen y=2) */
    int rel_x = mouse_x - sidebar_width;
    int rel_y = mouse_y - 2;

    /* Data rows start at y=3 in main window (after header line, column names,
     * separator) */
    int data_start_y = 3;
    int clicked_data_row = rel_y - data_start_y;

    if (clicked_data_row >= 0) {
      /* Calculate which row was clicked */
      size_t target_row = state->scroll_row + (size_t)clicked_data_row;

      if (target_row < state->data->num_rows) {
        /* Calculate which column was clicked */
        int x_pos = 1; /* Data starts at x=1 */
        size_t target_col = state->scroll_col;

        for (size_t col = state->scroll_col; col < state->data->num_columns;
             col++) {
          int width = tui_get_column_width(state, col);
          if (rel_x >= x_pos && rel_x < x_pos + width) {
            target_col = col;
            break;
          }
          x_pos += width + 1; /* +1 for separator */
          if (x_pos > state->term_cols)
            break;
          target_col = col + 1;
        }

        if (target_col < state->data->num_columns) {
          /* Update cursor position */
          state->cursor_row = target_row;
          state->cursor_col = target_col;
          state->sidebar_focused = false;

          /* Check if we need to load more rows (pagination) */
          tui_check_load_more(state);

          /* Double-click: enter edit mode */
          if (is_double) {
            tui_start_edit(state);
          }

          return true;
        }
      }
    }
  }

  return false;
}
