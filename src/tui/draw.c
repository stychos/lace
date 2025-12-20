/*
 * lace - Database Viewer and Manager
 * Drawing functions
 */

#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>

/* Helper to get column width from params */
static int grid_get_col_width(GridDrawParams *params, size_t col) {
  if (params->col_widths && col < params->num_col_widths) {
    return params->col_widths[col];
  }
  return DEFAULT_COL_WIDTH;
}

/* Draw a result set grid - shared between table view and query results */
void tui_draw_result_grid(TuiState *state, GridDrawParams *params) {
  if (!params || !params->win || !params->data ||
      params->data->num_columns == 0) {
    return;
  }

  WINDOW *win = params->win;
  ResultSet *data = params->data;
  int y = params->start_y;
  int x_base = params->start_x;
  int max_y = params->start_y + params->height;
  int max_x = params->start_x + params->width;

  /* Draw top border if requested */
  if (params->show_header_line && y < max_y) {
    wattron(win, A_BOLD | COLOR_PAIR(COLOR_BORDER));
    mvwhline(win, y, x_base, ACS_HLINE, params->width);
    wattroff(win, A_BOLD | COLOR_PAIR(COLOR_BORDER));
    y++;
  }

  if (y >= max_y)
    return;

  /* Draw column headers */
  wattron(win, A_BOLD);
  int x = x_base + 1;
  for (size_t col = params->scroll_col; col < data->num_columns; col++) {
    int width = grid_get_col_width(params, col);
    if (x + width + 3 > max_x)
      break;

    /* Show column header selection when focused */
    if (col == params->cursor_col && params->is_focused) {
      wattron(win, A_REVERSE);
    }

    const char *name = data->columns[col].name;
    mvwprintw(win, y, x, "%-*.*s", width, width, name ? name : "");

    if (col == params->cursor_col && params->is_focused) {
      wattroff(win, A_REVERSE);
    }

    x += width + 1;
    mvwaddch(win, y, x - 1, ACS_VLINE);
  }
  wattroff(win, A_BOLD);
  y++;

  if (y >= max_y)
    return;

  /* Header separator */
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  mvwhline(win, y, x_base, ACS_HLINE, params->width);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));
  y++;

  /* Draw data rows */
  if (!data->rows) {
    return;
  }

  for (size_t row = params->scroll_row; row < data->num_rows && y < max_y;
       row++) {

    Row *r = &data->rows[row];
    if (!r->cells)
      continue;

    x = x_base + 1;
    bool is_selected_row = (row == params->cursor_row) && params->is_focused;

    if (is_selected_row) {
      wattron(win, A_BOLD);
    }

    for (size_t col = params->scroll_col;
         col < data->num_columns && col < r->num_cells; col++) {
      int width = grid_get_col_width(params, col);
      if (x + width + 3 > max_x)
        break;

      bool is_selected = is_selected_row && (col == params->cursor_col);
      bool is_editing_cell = is_selected && params->is_editing;

      if (is_editing_cell) {
        /* Draw edit field with distinctive background */
        wattron(win, COLOR_PAIR(COLOR_EDIT));
        mvwhline(win, y, x, ' ', width);

        /* Draw the edit buffer */
        const char *buf = params->edit_buffer ? params->edit_buffer : "";
        size_t buf_len = strlen(buf);

        /* Calculate scroll for long text */
        size_t scroll = 0;
        if (params->edit_pos >= (size_t)(width - 1)) {
          scroll = params->edit_pos - width + 2;
        }

        /* Draw visible portion of text */
        size_t draw_len = buf_len > scroll ? buf_len - scroll : 0;
        if (draw_len > (size_t)width)
          draw_len = width;
        if (draw_len > 0) {
          mvwaddnstr(win, y, x, buf + scroll, (int)draw_len);
        }

        wattroff(win, COLOR_PAIR(COLOR_EDIT));

        /* Draw cursor character with reverse video for visibility */
        int cursor_x = x + (int)(params->edit_pos - scroll);
        if (cursor_x >= x && cursor_x < x + width) {
          char cursor_char =
              (params->edit_pos < buf_len) ? buf[params->edit_pos] : ' ';
          wattron(win, A_REVERSE | A_BOLD);
          mvwaddch(win, y, cursor_x, cursor_char);
          wattroff(win, A_REVERSE | A_BOLD);
          wmove(win, y, cursor_x);
        }
      } else if (is_selected) {
        wattron(win, COLOR_PAIR(COLOR_SELECTED));

        DbValue *val = &r->cells[col];
        if (val->is_null) {
          mvwprintw(win, y, x, "%-*s", width, "NULL");
        } else {
          char *str = db_value_to_string(val);
          if (str) {
            char *safe = tui_sanitize_for_display(str);
            mvwprintw(win, y, x, "%-*.*s", width, width, safe ? safe : str);
            free(safe);
            free(str);
          }
        }

        wattroff(win, COLOR_PAIR(COLOR_SELECTED));
      } else {
        DbValue *val = &r->cells[col];
        if (val->is_null) {
          wattron(win, COLOR_PAIR(COLOR_NULL));
          mvwprintw(win, y, x, "%-*s", width, "NULL");
          wattroff(win, COLOR_PAIR(COLOR_NULL));
        } else {
          char *str = db_value_to_string(val);
          if (str) {
            char *safe = tui_sanitize_for_display(str);
            if (val->type == DB_TYPE_INT || val->type == DB_TYPE_FLOAT) {
              wattron(win, COLOR_PAIR(COLOR_NUMBER));
            }
            mvwprintw(win, y, x, "%-*.*s", width, width, safe ? safe : str);
            if (val->type == DB_TYPE_INT || val->type == DB_TYPE_FLOAT) {
              wattroff(win, COLOR_PAIR(COLOR_NUMBER));
            }
            free(safe);
            free(str);
          }
        }
      }

      x += width + 1;
      mvwaddch(win, y, x - 1, ACS_VLINE);
    }

    if (is_selected_row) {
      wattroff(win, A_BOLD);
    }

    y++;
  }
  (void)state; /* May be used for future enhancements */
}

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
  int help_len = (int)strlen(help);
  int help_x = state->term_cols - help_len - 1;
  if (help_x > 0) {
    mvwprintw(state->header_win, 0, help_x, "%s", help);
  }

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

  /* Use the shared grid drawing function */
  GridDrawParams params = {.win = state->main_win,
                           .start_y = 0,
                           .start_x = 0,
                           .height = win_rows,
                           .width = win_cols,
                           .data = state->data,
                           .col_widths = state->col_widths,
                           .num_col_widths = state->num_col_widths,
                           .cursor_row = state->cursor_row,
                           .cursor_col = state->cursor_col,
                           .scroll_row = state->scroll_row,
                           .scroll_col = state->scroll_col,
                           .is_focused = !state->sidebar_focused,
                           .is_editing = state->editing,
                           .edit_buffer = state->edit_buffer,
                           .edit_pos = state->edit_pos,
                           .show_header_line = true};

  tui_draw_result_grid(state, &params);
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

  /* Check if we're in a query workspace with results focus */
  Workspace *ws = NULL;
  bool query_results_active = false;
  if (state->num_workspaces > 0) {
    ws = &state->workspaces[state->current_workspace];
    query_results_active = (ws->type == WORKSPACE_TYPE_QUERY &&
                            ws->query_focus_results && ws->query_results);
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
  } else if (query_results_active && ws->query_results->columns &&
             ws->query_result_col < ws->query_results->num_columns) {
    /* Query results column info */
    ColumnDef *col = &ws->query_results->columns[ws->query_result_col];
    ColumnDef *schema_col = NULL;

    /* Try to get richer info from schema if available */
    if (ws->query_source_schema && col->name) {
      for (size_t i = 0; i < ws->query_source_schema->num_columns; i++) {
        if (ws->query_source_schema->columns[i].name &&
            strcmp(ws->query_source_schema->columns[i].name, col->name) == 0) {
          schema_col = &ws->query_source_schema->columns[i];
          break;
        }
      }
    }

    /* Use schema column if found, otherwise use result column */
    ColumnDef *display_col = schema_col ? schema_col : col;

    /* Build column info string */
    char info[256];
    int pos = 0;

    /* Column name */
    pos += snprintf(info + pos, sizeof(info) - pos, "%s",
                    display_col->name ? display_col->name : "?");

    /* Type */
    if (display_col->type_name) {
      pos += snprintf(info + pos, sizeof(info) - pos, " : %s",
                      display_col->type_name);
    }

    /* Flags */
    if (display_col->primary_key) {
      pos += snprintf(info + pos, sizeof(info) - pos, " [PK]");
    }
    if (!display_col->nullable) {
      pos += snprintf(info + pos, sizeof(info) - pos, " NOT NULL");
    }
    if (display_col->default_val) {
      snprintf(info + pos, sizeof(info) - pos, " DEFAULT %s",
               display_col->default_val);
    }

    mvwprintw(state->status_win, 0, 1, "%s", info);
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
  if (query_results_active) {
    /* Query results row position */
    char pos[64];
    if (ws->query_paginated && ws->query_total_rows > 0) {
      /* Paginated: show actual row number in total dataset */
      size_t actual_row = ws->query_loaded_offset + ws->query_result_row + 1;
      snprintf(pos, sizeof(pos), "Row %zu/%zu", actual_row,
               ws->query_total_rows);
    } else {
      /* Non-paginated */
      snprintf(pos, sizeof(pos), "Row %zu/%zu", ws->query_result_row + 1,
               ws->query_results->num_rows);
    }
    mvwprintw(state->status_win, 0, state->term_cols - (int)strlen(pos) - 1,
              "%s", pos);
  } else if (state->data) {
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
    /* Only scroll in main area */
    if (mouse_x >= sidebar_width) {
      int scroll_amount = 3; /* Scroll 3 rows at a time */

      /* Check if we're in a query workspace with results */
      if (state->num_workspaces > 0) {
        Workspace *ws = &state->workspaces[state->current_workspace];
        if (ws->type == WORKSPACE_TYPE_QUERY && ws->query_results &&
            ws->query_results->num_rows > 0) {
          /* Calculate results area position */
          int win_rows = state->term_rows - 4;
          int editor_height = (win_rows - 1) * 3 / 10;
          if (editor_height < 3)
            editor_height = 3;
          int results_start_y = 2 + editor_height + 1;

          if (mouse_y >= results_start_y) {
            /* Scroll query results using helper that handles pagination */
            ws->query_focus_results = true;
            int delta = is_scroll_up ? -scroll_amount : scroll_amount;
            tui_query_scroll_results(state, delta);
            state->sidebar_focused = false;
            return true;
          }
        }
      }

      /* Handle table data scrolling */
      if (state->data && state->data->num_rows > 0) {
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
            state->cursor_row =
                state->data->num_rows > 0 ? state->data->num_rows - 1 : 0;
          }
        }

        /* Adjust scroll to keep cursor visible using actual main window height */
        int main_rows, main_cols;
        getmaxyx(state->main_win, main_rows, main_cols);
        (void)main_cols;
        int visible_rows = main_rows - 3; /* Minus header rows in main window */
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
        if (is_double) {
          /* Double-click: close the tab */
          Workspace *target_ws = &state->workspaces[i];

          /* Check if query tab has content */
          if (target_ws->type == WORKSPACE_TYPE_QUERY &&
              ((target_ws->query_text && target_ws->query_len > 0) ||
               target_ws->query_results)) {
            /* Ask for confirmation */
            if (!tui_show_confirm_dialog(
                    state, "Close query tab with unsaved content?")) {
              return true; /* User cancelled */
            }
          }

          /* Switch to tab first if not current, then close */
          if (i != state->current_workspace) {
            workspace_switch(state, i);
          }
          workspace_close(state);
          state->sidebar_focused = false;
        } else {
          /* Single click: switch to tab */
          if (i != state->current_workspace) {
            workspace_switch(state, i);
            state->sidebar_focused = false;
          }
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

    /* Validate sidebar_row is non-negative before use */
    if (sidebar_row < 0) {
      return true;
    }

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
            /* Double-click: always open in new tab */
            workspace_create(state, actual_idx);
            state->sidebar_focused = false;
          } else {
            /* Single click: change current tab's table or switch to existing */
            if (state->num_workspaces > 0) {
              Workspace *ws = &state->workspaces[state->current_workspace];
              if (ws->type == WORKSPACE_TYPE_QUERY) {
                /* Query tab active - check if table tab already exists */
                bool found = false;
                for (size_t i = 0; i < state->num_workspaces; i++) {
                  Workspace *w = &state->workspaces[i];
                  if (w->type == WORKSPACE_TYPE_TABLE &&
                      w->table_index == actual_idx) {
                    /* Found existing tab - switch to it */
                    workspace_switch(state, i);
                    found = true;
                    break;
                  }
                }
                if (!found) {
                  /* No existing tab - create new one */
                  workspace_create(state, actual_idx);
                }
                state->sidebar_focused = false;
              } else if (state->current_table != actual_idx) {
                /* Table tab - update current workspace with new table */
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

  /* Check if click is in query tab area */
  if (mouse_x >= sidebar_width && state->num_workspaces > 0) {
    Workspace *ws = &state->workspaces[state->current_workspace];
    if (ws->type == WORKSPACE_TYPE_QUERY) {
      state->sidebar_filter_active = false;
      state->sidebar_focused = false;

      /* If currently editing query results, save the edit first */
      if (ws->query_result_editing) {
        tui_query_confirm_result_edit(state);
      }

      /* Get actual main window dimensions */
      int main_win_rows, main_win_cols;
      getmaxyx(state->main_win, main_win_rows, main_win_cols);

      /* Calculate query view layout */
      int win_rows = main_win_rows;
      int editor_height = (win_rows - 1) * 3 / 10; /* 30% for editor */
      if (editor_height < 3)
        editor_height = 3;
      int results_start_y = 2 + editor_height + 1; /* screen coords */
      int results_header_y =
          results_start_y + 1; /* "Results (N rows)" header */
      int results_data_y =
          results_header_y + 3; /* After header + col names + separator */

      if (mouse_y < results_start_y) {
        /* Clicked in editor area */
        ws->query_focus_results = false;
      } else if (ws->query_results && ws->query_results->num_rows > 0 &&
                 mouse_y >= results_data_y) {
        /* Clicked in results data area */
        ws->query_focus_results = true;

        /* Calculate which row was clicked */
        int clicked_row = mouse_y - results_data_y;
        size_t target_row = ws->query_result_scroll_row + (size_t)clicked_row;

        if (target_row < ws->query_results->num_rows) {
          /* Calculate which column was clicked */
          int rel_x = mouse_x - sidebar_width;
          int x_pos = 1;
          size_t target_col = ws->query_result_scroll_col;

          for (size_t col = ws->query_result_scroll_col;
               col < ws->query_results->num_columns; col++) {
            int width = DEFAULT_COL_WIDTH;
            if (ws->query_result_col_widths &&
                col < ws->query_result_num_cols) {
              width = ws->query_result_col_widths[col];
            }
            if (rel_x >= x_pos && rel_x < x_pos + width) {
              target_col = col;
              break;
            }
            x_pos += width + 1;
            if (x_pos > main_win_cols)
              break;
            target_col = col + 1;
          }

          if (target_col < ws->query_results->num_columns) {
            ws->query_result_row = target_row;
            ws->query_result_col = target_col;

            /* Double-click: enter edit mode */
            if (is_double && !ws->query_result_editing) {
              tui_query_start_result_edit(state);
            }
          }
        }
      } else {
        /* Clicked in results header area */
        ws->query_focus_results = true;
      }
      return true;
    }
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

    /* Get actual main window dimensions */
    int table_win_rows, table_win_cols;
    getmaxyx(state->main_win, table_win_rows, table_win_cols);
    (void)table_win_rows;

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
          if (x_pos > table_win_cols)
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
