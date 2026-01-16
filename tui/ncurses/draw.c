/*
 * Lace
 * Drawing functions
 *
 * Uses RenderBackend abstraction for portability while maintaining
 * ncurses compatibility during migration.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/config.h"
#include "core/app_state.h"
#include "util/connstr.h"
#include "render_helpers.h"
#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>

/* Maximum number of visible columns (limited by terminal width) */
#define MAX_VISIBLE_COLUMNS 256

/* ============================================================================
 * Helper Functions
 * ============================================================================
 */

/* Check if tab has active filters (filters that affect the query) */
static bool has_active_filters(Tab *tab) {
  if (!tab || tab->filters.num_filters == 0)
    return false;

  for (size_t i = 0; i < tab->filters.num_filters; i++) {
    ColumnFilter *cf = &tab->filters.filters[i];
    /* Filter is active if it has a value OR doesn't need one (IS NULL, etc.) */
    if (cf->value[0] != '\0' || !filter_op_needs_value(cf->op)) {
      return true;
    }
  }
  return false;
}

/* Helper to get column width from params */
static int grid_get_col_width(GridDrawParams *params, size_t col) {
  if (params->col_widths && col < params->num_col_widths) {
    return params->col_widths[col];
  }
  return DEFAULT_COL_WIDTH;
}

/* ============================================================================
 * Result Grid Drawing
 * ============================================================================
 */

/* Draw a result set grid - shared between table view and query results */
void tui_draw_result_grid(TuiState *state, GridDrawParams *params) {
  if (!params || !params->win || !params->data ||
      params->data->num_columns == 0 || !params->data->columns) {
    return;
  }

  WINDOW *win = params->win;
  ResultSet *data = params->data;
  int y = params->start_y;
  int x_base = params->start_x;
  int max_y = params->start_y + params->height;
  int max_x = params->start_x + params->width;

  /* Calculate column divider positions for intersection characters */
  int divider_positions[MAX_VISIBLE_COLUMNS];
  size_t num_dividers = 0;

  {
    int calc_x = x_base + 1;
    for (size_t col = params->scroll_col; col < data->num_columns; col++) {
      int width = grid_get_col_width(params, col);
      if (calc_x + width + 3 > max_x)
        break;
      calc_x += width + 1;
      if (num_dividers < MAX_VISIBLE_COLUMNS) {
        divider_positions[num_dividers++] = calc_x - 1;
      }
    }
  }

  /* Draw top border if requested */
  if (params->show_header_line && y < max_y) {
    wattron(win, A_BOLD | COLOR_PAIR(COLOR_BORDER));
    mvwhline(win, y, x_base, ACS_HLINE, params->width);
    /* Add T-junctions where column dividers will be */
    for (size_t i = 0; i < num_dividers; i++) {
      mvwaddch(win, y, divider_positions[i], ACS_TTEE);
    }
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

    /* Draw column name at full width */
    mvwprintw(win, y, x, "%-*.*s", width, width, name ? name : "");

    if (col == params->cursor_col && params->is_focused) {
      wattroff(win, A_REVERSE);
    }

    x += width + 1;
    wattron(win, COLOR_PAIR(COLOR_BORDER));
    mvwaddch(win, y, x - 1, ACS_VLINE);
    wattroff(win, COLOR_PAIR(COLOR_BORDER));
  }
  wattroff(win, A_BOLD);
  y++;

  if (y >= max_y)
    return;

  /* Second header row for sort indicators (only if sorting is active) */
  if (params->num_sort_entries > 0 && params->sort_entries) {
    x = x_base + 1;
    for (size_t col = params->scroll_col; col < data->num_columns; col++) {
      int width = grid_get_col_width(params, col);
      if (x + width + 3 > max_x)
        break;

      /* Check if this column is sorted */
      bool found = false;
      for (size_t i = 0; i < params->num_sort_entries; i++) {
        /* Bounds check: skip invalid column indices */
        if (params->sort_entries[i].column >= data->num_columns)
          continue;
        if (params->sort_entries[i].column == col) {
          /* Show: arrow + direction + priority (e.g., "▲ asc, 1") */
          const char *arrow = (params->sort_entries[i].direction == SORT_ASC)
                                  ? "\xE2\x96\xB2"  /* ▲ UTF-8 */
                                  : "\xE2\x96\xBC"; /* ▼ UTF-8 */
          const char *dir_text =
              (params->sort_entries[i].direction == SORT_ASC) ? "asc" : "desc";
          char sort_info[24];
          snprintf(sort_info, sizeof(sort_info), "%s %s, %d", arrow, dir_text,
                   (int)(i + 1));
          mvwprintw(win, y, x, "%-*.*s", width, width, sort_info);
          found = true;
          break;
        }
      }
      if (!found) {
        /* Empty cell for non-sorted columns */
        mvwprintw(win, y, x, "%-*s", width, "");
      }

      x += width + 1;
      wattron(win, COLOR_PAIR(COLOR_BORDER));
      mvwaddch(win, y, x - 1, ACS_VLINE);
      wattroff(win, COLOR_PAIR(COLOR_BORDER));
    }
    y++;

    if (y >= max_y)
      return;
  }

  /* Header separator */
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  mvwhline(win, y, x_base, ACS_HLINE, params->width);
  /* Add cross/plus junctions where column dividers are */
  for (size_t i = 0; i < num_dividers; i++) {
    mvwaddch(win, y, divider_positions[i], ACS_PLUS);
  }
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
    bool is_cursor_row = (row == params->cursor_row) && params->is_focused;

    /* Check if row is in selection set (for bulk operations) */
    bool is_marked_row = false;
    Tab *tab = state ? TUI_TAB(state) : NULL;
    if (tab) {
      size_t global_row = params->selection_offset + row;
      is_marked_row = tab_is_row_selected(tab, global_row);
    }

    /* Determine effective schema for PK detection */
    TableSchema *effective_schema = NULL;
    if (tab && tab->type == TAB_TYPE_QUERY && tab->query_source_schema) {
      effective_schema = tab->query_source_schema;
    } else if (tab) {
      effective_schema = tab->schema;
    }

    /* Apply row-level styling */
    if (is_marked_row || is_cursor_row) {
      wattron(win, A_BOLD);
    }

    for (size_t col = params->scroll_col;
         col < data->num_columns && col < r->num_cells; col++) {
      int width = grid_get_col_width(params, col);
      if (x + width + 3 > max_x)
        break;

      bool is_selected = is_cursor_row && (col == params->cursor_col);
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
        /* Check if this column is a primary key */
        bool is_pk_col = false;
        if (effective_schema && col < effective_schema->num_columns) {
          is_pk_col = effective_schema->columns[col].primary_key;
        }

        /* Use reverse video for PK on marked row to show white text */
        if (is_pk_col && is_marked_row) {
          wattron(win, A_REVERSE);
        } else {
          wattron(win, COLOR_PAIR(COLOR_SELECTED));
        }

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

        if (is_pk_col && is_marked_row) {
          wattroff(win, A_REVERSE);
        } else {
          wattroff(win, COLOR_PAIR(COLOR_SELECTED));
        }
      } else {
        DbValue *val = &r->cells[col];
        /* Check if this column is a primary key */
        bool is_pk_col = false;
        if (effective_schema && col < effective_schema->num_columns) {
          is_pk_col = effective_schema->columns[col].primary_key;
        }

        if (val->is_null) {
          wattron(win, COLOR_PAIR(COLOR_NULL));
          mvwprintw(win, y, x, "%-*s", width, "NULL");
          wattroff(win, COLOR_PAIR(COLOR_NULL));
        } else {
          char *str = db_value_to_string(val);
          if (str) {
            char *safe = tui_sanitize_for_display(str);
            if (is_pk_col && is_marked_row && is_cursor_row) {
              /* White text for PK on cursor row - no color attr needed */
            } else if (is_pk_col && is_marked_row) {
              wattron(win, COLOR_PAIR(COLOR_ERROR_TEXT));
            } else if (is_pk_col) {
              wattron(win, COLOR_PAIR(COLOR_PK));
            } else if (val->type == DB_TYPE_INT || val->type == DB_TYPE_FLOAT) {
              wattron(win, COLOR_PAIR(COLOR_NUMBER));
            }
            mvwprintw(win, y, x, "%-*.*s", width, width, safe ? safe : str);
            if (is_pk_col && is_marked_row && is_cursor_row) {
              /* White text for PK on cursor row - no color attr needed */
            } else if (is_pk_col && is_marked_row) {
              wattroff(win, COLOR_PAIR(COLOR_ERROR_TEXT));
            } else if (is_pk_col) {
              wattroff(win, COLOR_PAIR(COLOR_PK));
            } else if (val->type == DB_TYPE_INT || val->type == DB_TYPE_FLOAT) {
              wattroff(win, COLOR_PAIR(COLOR_NUMBER));
            }
            free(safe);
            free(str);
          }
        }
      }

      x += width + 1;
      wattron(win, COLOR_PAIR(COLOR_BORDER));
      mvwaddch(win, y, x - 1, ACS_VLINE);
      wattroff(win, COLOR_PAIR(COLOR_BORDER));
    }

    /* Remove row-level styling */
    if (is_marked_row || is_cursor_row) {
      wattroff(win, A_BOLD);
    }

    y++;
  }
}

/* ============================================================================
 * Header Drawing
 * ============================================================================
 */

void tui_draw_header(TuiState *state) {
  if (!state || !state->header_win || !state->header_visible)
    return;

  werase(state->header_win);
  wbkgd(state->header_win, COLOR_PAIR(COLOR_HEADER));

  /* Draw connection info: driver://host:port/database */
  if (state->conn) {
    int x = 1;

    /* Driver name */
    if (state->conn->driver && state->conn->driver->name) {
      mvwprintw(state->header_win, 0, x, "%s://", state->conn->driver->name);
      x += (int)strlen(state->conn->driver->name) + 3;
    }

    /* Host (for network databases) or just database path (for SQLite) */
    if (state->conn->host && state->conn->host[0]) {
      mvwprintw(state->header_win, 0, x, "%s", state->conn->host);
      x += (int)strlen(state->conn->host);
      if (state->conn->port > 0) {
        int port_len = snprintf(NULL, 0, ":%d", state->conn->port);
        mvwprintw(state->header_win, 0, x, ":%d", state->conn->port);
        x += port_len;
      }
      mvwprintw(state->header_win, 0, x, "/");
      x += 1;
    }

    /* Database name */
    if (state->conn->database) {
      mvwprintw(state->header_win, 0, x, "%s", state->conn->database);
    }
  }

  /* Right side: combined connection/workspace indicator */
  if (state->app) {
    Tab *tab = TUI_TAB(state);
    bool multi_conn = state->app->num_connections > 1;
    bool multi_ws = state->app->num_workspaces > 1;

    if (multi_conn || multi_ws) {
      char indicator[32];
      if (multi_conn && multi_ws) {
        snprintf(indicator, sizeof(indicator), "[C%zu W%zu]",
                 tab ? tab->connection_index + 1 : 1,
                 state->app->current_workspace + 1);
      } else if (multi_conn) {
        snprintf(indicator, sizeof(indicator), "[C%zu]",
                 tab ? tab->connection_index + 1 : 1);
      } else {
        snprintf(indicator, sizeof(indicator), "[W%zu]",
                 state->app->current_workspace + 1);
      }
      int ind_len = (int)strlen(indicator);
      int ind_x = state->term_cols - ind_len - 1;
      if (ind_x > 0) {
        mvwprintw(state->header_win, 0, ind_x, "%s", indicator);
      }
    }
  }

  wrefresh(state->header_win);
}

/* ============================================================================
 * Add Row Drawing
 * ============================================================================
 */

/* Helper to draw a single cell in the new row */
static void draw_add_row_cell(WINDOW *win, int y, int x, int width,
                              TuiState *state, size_t col, bool is_selected,
                              bool is_editing) {
  Tab *tab = TUI_TAB(state);
  TableSchema *schema = tab ? tab->schema : NULL;
  bool is_edited = state->new_row_edited && state->new_row_edited[col];
  bool is_auto_increment = schema && col < schema->num_columns &&
                           schema->columns[col].auto_increment;
  bool is_placeholder = state->new_row_placeholders[col] && !is_edited;

  if (is_editing) {
    /* Draw edit field with distinctive background */
    wattron(win, COLOR_PAIR(COLOR_EDIT));
    mvwhline(win, y, x, ' ', width);

    const char *buf =
        state->new_row_edit_buffer ? state->new_row_edit_buffer : "";
    size_t buf_len = strlen(buf);

    /* Calculate scroll for long text */
    size_t scroll = 0;
    if (state->new_row_edit_pos >= (size_t)(width - 1)) {
      scroll = state->new_row_edit_pos - width + 2;
    }

    /* Draw visible portion of text */
    size_t draw_len = buf_len > scroll ? buf_len - scroll : 0;
    if (draw_len > (size_t)width)
      draw_len = width;
    if (draw_len > 0) {
      mvwaddnstr(win, y, x, buf + scroll, (int)draw_len);
    }

    wattroff(win, COLOR_PAIR(COLOR_EDIT));

    /* Draw cursor */
    int cursor_x = x + (int)(state->new_row_edit_pos - scroll);
    if (cursor_x >= x && cursor_x < x + width) {
      char cursor_char = (state->new_row_edit_pos < buf_len)
                             ? buf[state->new_row_edit_pos]
                             : ' ';
      wattron(win, A_REVERSE | A_BOLD);
      mvwaddch(win, y, cursor_x, cursor_char);
      wattroff(win, A_REVERSE | A_BOLD);
      wmove(win, y, cursor_x);
    }
  } else if (is_selected) {
    wattron(win, COLOR_PAIR(COLOR_SELECTED));
    mvwhline(win, y, x, ' ', width);

    if (is_placeholder && is_auto_increment) {
      /* Auto-increment placeholder - show in dim */
      wattron(win, A_DIM);
      mvwprintw(win, y, x, "%-*.*s", width, width, "AI");
      wattroff(win, A_DIM);
    } else if (is_placeholder) {
      /* Default value placeholder - show in dim */
      wattron(win, A_DIM);
      const DbValue *val = &state->new_row_values[col];
      char *str = db_value_to_string(val);
      mvwprintw(win, y, x, "%-*.*s", width, width, str ? str : "");
      free(str);
      wattroff(win, A_DIM);
    } else {
      const DbValue *val = &state->new_row_values[col];
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
    }
    wattroff(win, COLOR_PAIR(COLOR_SELECTED));
  } else {
    /* Non-selected cell */
    if (is_placeholder && is_auto_increment) {
      /* Auto-increment placeholder - gray/dim */
      wattron(win, A_DIM);
      mvwprintw(win, y, x, "%-*.*s", width, width, "AI");
      wattroff(win, A_DIM);
    } else if (is_placeholder) {
      /* Default value placeholder - gray/dim */
      wattron(win, A_DIM);
      const DbValue *val = &state->new_row_values[col];
      char *str = db_value_to_string(val);
      mvwprintw(win, y, x, "%-*.*s", width, width, str ? str : "");
      free(str);
      wattroff(win, A_DIM);
    } else {
      const DbValue *val = &state->new_row_values[col];
      if (val->is_null) {
        wattron(win, COLOR_PAIR(COLOR_NULL));
        mvwprintw(win, y, x, "%-*s", width, "NULL");
        wattroff(win, COLOR_PAIR(COLOR_NULL));
      } else {
        char *str = db_value_to_string(val);
        if (str) {
          char *safe = tui_sanitize_for_display(str);
          mvwprintw(win, y, x, "%-*.*s", width, width, safe ? safe : str);
          free(safe);
          free(str);
        }
      }
    }
  }
}

/* Draw the add-row as overlay at the bottom of the table */
static void tui_draw_add_row_overlay(TuiState *state, GridDrawParams *params) {
  if (!state || !state->adding_row || !params || !params->win)
    return;

  WINDOW *win = params->win;
  int win_rows, win_cols;
  getmaxyx(win, win_rows, win_cols);
  (void)win_cols;

  /* Calculate the Y position for the overlay */
  int overlay_y = win_rows - 2;
  if (overlay_y < params->start_y + 3)
    return;

  /* Draw separator line */
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  mvwhline(win, overlay_y, params->start_x, ACS_HLINE, params->width);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));

  int row_y = overlay_y + 1;

  /* Use the table's scroll_col for horizontal alignment */
  TableWidget *widget = TUI_TABLE_WIDGET(state);
  size_t scroll_col = widget ? widget->base.state.scroll_col : 0;

  /* Draw cells aligned with the main table */
  int x = params->start_x + 1;
  ResultSet *data = params->data;
  if (!data)
    return;

  for (size_t col = scroll_col; col < data->num_columns; col++) {
    if (col >= state->new_row_num_cols)
      break;

    int width = grid_get_col_width(params, col);
    if (x + width + 3 > params->start_x + params->width)
      break;

    bool is_selected = (col == state->new_row_cursor_col);
    bool is_editing = is_selected && state->new_row_cell_editing;

    draw_add_row_cell(win, row_y, x, width, state, col, is_selected,
                      is_editing);

    x += width + 1;
    wattron(win, COLOR_PAIR(COLOR_BORDER));
    mvwaddch(win, row_y, x - 1, ACS_VLINE);
    wattroff(win, COLOR_PAIR(COLOR_BORDER));
  }
}

/* ============================================================================
 * Table Drawing
 * ============================================================================
 */

void tui_draw_table(TuiState *state) {
  if (!state || !state->main_win)
    return;

  werase(state->main_win);

  /* Get actual window dimensions */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);

  /* Calculate filters panel height and draw if visible */
  int filters_height = 0;
  if (state->filters_visible) {
    filters_height = tui_get_filters_panel_height(state);
    tui_draw_filters_panel(state);
  }

  /* Get tab for model data, widget for view state */
  Tab *tab = TUI_TAB(state);
  TableWidget *widget = TUI_TABLE_WIDGET(state);
  if (!tab)
    return;

  /* Read model data from Tab */
  ResultSet *data = tab->data;

  /* Read view state from TableWidget (source of truth) */
  int *col_widths = widget ? widget->col_widths : tab->col_widths;
  size_t num_col_widths = widget ? widget->num_col_widths : tab->num_col_widths;
  size_t cursor_row = widget ? widget->base.state.cursor_row : 0;
  size_t cursor_col = widget ? widget->base.state.cursor_col : 0;
  size_t scroll_row = widget ? widget->base.state.scroll_row : 0;
  size_t scroll_col = widget ? widget->base.state.scroll_col : 0;

  bool is_editing = state->editing;
  char *edit_buffer = state->edit_buffer;
  size_t edit_pos = state->edit_pos;

  /* Check if there's a table error (e.g., table doesn't exist) */
  if (tab && tab->table_error) {
    int center_y = filters_height + (win_rows - filters_height) / 2;
    wattron(state->main_win, COLOR_PAIR(COLOR_ERROR_TEXT) | A_BOLD);
    const char *msg1 = "Table does not exist:";
    mvwprintw(state->main_win, center_y - 1, (win_cols - (int)strlen(msg1)) / 2,
              "%s", msg1);
    wattroff(state->main_win, A_BOLD);
    mvwprintw(state->main_win, center_y,
              (win_cols - (int)strlen(tab->table_name)) / 2, "%s",
              tab->table_name ? tab->table_name : "(unknown)");
    wattroff(state->main_win, COLOR_PAIR(COLOR_ERROR_TEXT));

    wattron(state->main_win, A_DIM);
    char *close_key = hotkey_get_display(state->app->config, HOTKEY_CLOSE_TAB);
    char hint[128];
    snprintf(hint, sizeof(hint), "Press [%s] to close this tab",
             close_key ? close_key : "-");
    free(close_key);
    mvwprintw(state->main_win, center_y + 2, (win_cols - (int)strlen(hint)) / 2,
              "%s", hint);
    wattroff(state->main_win, A_DIM);
    wrefresh(state->main_win);
    return;
  }

  if (!data || data->num_columns == 0 || !data->columns) {
    int msg_y = filters_height + (win_rows - filters_height) / 2;
    mvwprintw(state->main_win, msg_y, (win_cols - 7) / 2, "No data");
    wrefresh(state->main_win);
    return;
  }

  /* Use the shared grid drawing function */
  GridDrawParams params = {.win = state->main_win,
                           .start_y = filters_height,
                           .start_x = 0,
                           .height = win_rows - filters_height,
                           .width = win_cols,
                           .data = data,
                           .col_widths = col_widths,
                           .num_col_widths = num_col_widths,
                           .cursor_row = cursor_row,
                           .cursor_col = cursor_col,
                           .scroll_row = scroll_row,
                           .scroll_col = scroll_col,
                           .selection_offset = widget ? widget->loaded_offset : 0,
                           .is_focused = !tui_sidebar_focused(state) &&
                                         !tui_filters_focused(state),
                           .is_editing = is_editing,
                           .edit_buffer = edit_buffer,
                           .edit_pos = edit_pos,
                           .show_header_line = true,
                           .sort_entries = tab ? tab->sort_entries : NULL,
                           .num_sort_entries = tab ? tab->num_sort_entries : 0};

  tui_draw_result_grid(state, &params);

  /* Draw add-row overlay if in add-row mode */
  if (state->adding_row && state->new_row_values &&
      state->new_row_num_cols > 0) {
    tui_draw_add_row_overlay(state, &params);
  }

  wrefresh(state->main_win);
}

/* ============================================================================
 * Connection Tab Drawing
 * ============================================================================
 */

void tui_draw_connection_tab(TuiState *state) {
  if (!state || !state->main_win)
    return;

  werase(state->main_win);

  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);

  Tab *tab = TUI_TAB(state);
  /* Get full connection string from connection object */
  const char *connstr = NULL;
  char *masked_connstr = NULL;
  if (tab) {
    Connection *conn = app_get_connection(state->app, tab->connection_index);
    if (conn) {
      /* Mask password for display */
      masked_connstr = connstr_mask_password(conn->connstr);
      connstr = masked_connstr;
    }
  }
  if (!connstr) {
    connstr = tab ? tab->table_name : "Connection";
  }

  /* Center the content vertically */
  int center_y = win_rows / 2 - 2;
  if (center_y < 1)
    center_y = 1;

  /* Draw connection info */
  wattron(state->main_win, A_BOLD);
  const char *title = "Connection";
  int title_x = (win_cols - (int)strlen(title)) / 2;
  if (title_x < 0)
    title_x = 0;
  mvwprintw(state->main_win, center_y, title_x, "%s", title);
  wattroff(state->main_win, A_BOLD);

  /* Show connection string (truncated if needed) */
  wattron(state->main_win, COLOR_PAIR(COLOR_STATUS));
  int max_connstr_len = win_cols - 4;
  if (max_connstr_len < 10)
    max_connstr_len = 10;
  int connstr_x = (win_cols - (int)strlen(connstr)) / 2;
  if (connstr_x < 2)
    connstr_x = 2;
  mvwprintw(state->main_win, center_y + 2, connstr_x, "%.*s", max_connstr_len,
            connstr);
  wattroff(state->main_win, COLOR_PAIR(COLOR_STATUS));

  free(masked_connstr);

  /* Show instruction */
  wattron(state->main_win, A_DIM);
  const char *hint = "Select a table from the sidebar to view data";
  int hint_x = (win_cols - (int)strlen(hint)) / 2;
  if (hint_x < 0)
    hint_x = 0;
  mvwprintw(state->main_win, center_y + 4, hint_x, "%s", hint);

  char *close_key = hotkey_get_display(state->app->config, HOTKEY_CLOSE_TAB);
  char hint2[128];
  snprintf(hint2, sizeof(hint2), "Press [%s] to close this connection",
           close_key ? close_key : "-");
  free(close_key);
  int hint2_x = (win_cols - (int)strlen(hint2)) / 2;
  if (hint2_x < 0)
    hint2_x = 0;
  mvwprintw(state->main_win, center_y + 5, hint2_x, "%s", hint2);
  wattroff(state->main_win, A_DIM);

  wrefresh(state->main_win);
}

/* ============================================================================
 * Status Bar Drawing
 * ============================================================================
 */

void tui_draw_status(TuiState *state) {
  if (!state || !state->status_win || !state->status_visible)
    return;

  werase(state->status_win);

  if (state->status_is_error) {
    /* Error: default background with red text for better contrast */
    wbkgd(state->status_win, A_NORMAL);
    wattron(state->status_win, COLOR_PAIR(COLOR_ERROR_TEXT));
  } else {
    wbkgd(state->status_win, COLOR_PAIR(COLOR_STATUS));
  }

  /* Check if we're in a query tab with results focus */
  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  TableWidget *widget = TUI_TABLE_WIDGET(state);
  bool query_results_active = false;
  if (tab && ui) {
    query_results_active = (tab->type == TAB_TYPE_QUERY &&
                            ui->query_focus_results && tab->query_results);
  }

  /* Left: show table name when sidebar focused, otherwise column info */
  char **tables = TUI_TABLES(state);
  size_t num_tables = TUI_NUM_TABLES(state);
  if (tui_sidebar_focused(state) && tables && num_tables > 0) {
    /* Show highlighted table name */
    size_t actual_idx =
        tui_get_filtered_table_index(state, tui_sidebar_highlight(state));
    if (actual_idx < num_tables && tables[actual_idx]) {
      const char *name = tables[actual_idx];
      mvwprintw(state->status_win, 0, 1, "%s", name);
    }
  } else if (query_results_active && tab->query_results->columns &&
             tab->query_result_col < tab->query_results->num_columns) {
    /* Query results column info */
    ColumnDef *col = &tab->query_results->columns[tab->query_result_col];
    ColumnDef *schema_col = NULL;

    /* Try to get richer info from schema if available */
    if (tab->query_source_schema && col->name) {
      for (size_t i = 0; i < tab->query_source_schema->num_columns; i++) {
        if (tab->query_source_schema->columns[i].name &&
            strcmp(tab->query_source_schema->columns[i].name, col->name) == 0) {
          schema_col = &tab->query_source_schema->columns[i];
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
  } else if (state->adding_row && tab && tab->schema &&
             state->new_row_cursor_col < tab->schema->num_columns) {
    /* Add-row mode: show column info for current cursor position */
    ColumnDef *col = &tab->schema->columns[state->new_row_cursor_col];

    /* Build column info string */
    char info[256];
    int pos = 0;

    /* Column name */
    pos += snprintf(info + pos, sizeof(info) - pos, "[+] %s",
                    col->name ? col->name : "?");

    /* Type */
    if (col->type_name) {
      pos += snprintf(info + pos, sizeof(info) - pos, " : %s", col->type_name);
    }

    /* Flags */
    if (col->primary_key) {
      pos += snprintf(info + pos, sizeof(info) - pos, " [PK]");
    }
    if (col->auto_increment) {
      pos += snprintf(info + pos, sizeof(info) - pos, " [AI]");
    }
    if (!col->nullable) {
      pos += snprintf(info + pos, sizeof(info) - pos, " NOT NULL");
    }
    if (col->default_val) {
      snprintf(info + pos, sizeof(info) - pos, " DEFAULT %s", col->default_val);
    }

    mvwprintw(state->status_win, 0, 1, "%s", info);
  } else if (tab && tab->schema && widget &&
             widget->base.state.cursor_col < tab->schema->num_columns) {
    ColumnDef *col = &tab->schema->columns[widget->base.state.cursor_col];

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

  /* Right: row position and loading indicator */
  int right_pos = state->term_cols - 1;

  /* Background loading indicator */
  if (state->bg_loading_active) {
    const char *loading = "[Loading...]";
    int loading_len = (int)strlen(loading);
    right_pos -= loading_len + 1;
    wattron(state->status_win, A_BOLD);
    mvwprintw(state->status_win, 0, right_pos + 1, "%s", loading);
    wattroff(state->status_win, A_BOLD);
  }

  if (query_results_active) {
    /* Query results row position */
    char pos[64];
    if (tab->query_paginated && tab->query_total_rows > 0) {
      /* Paginated: show actual row number in total dataset */
      size_t actual_row = tab->query_loaded_offset + tab->query_result_row + 1;
      snprintf(pos, sizeof(pos), "Row %zu/%zu", actual_row,
               tab->query_total_rows);
    } else {
      /* Non-paginated */
      snprintf(pos, sizeof(pos), "Row %zu/%zu", tab->query_result_row + 1,
               tab->query_results->num_rows);
    }
    int pos_len = (int)strlen(pos);
    mvwprintw(state->status_win, 0, right_pos - pos_len, "%s", pos);
  } else if (tab && tab->data && widget) {
    char pos[96];
    size_t actual_row = widget->loaded_offset + widget->base.state.cursor_row + 1;
    size_t total =
        widget->total_rows > 0 ? widget->total_rows : tab->data->num_rows;
    /* Show ~ prefix for approximate counts */
    bool approx = widget->row_count_approximate;
    bool filtered = has_active_filters(tab);
    if (filtered && tab->unfiltered_total_rows > 0) {
      snprintf(pos, sizeof(pos), "Row %zu/%s%zu [%zu]", actual_row,
               approx ? "~" : "", total, tab->unfiltered_total_rows);
    } else {
      snprintf(pos, sizeof(pos), "Row %zu/%s%zu", actual_row, approx ? "~" : "",
               total);
    }
    int pos_len = (int)strlen(pos);
    mvwprintw(state->status_win, 0, right_pos - pos_len, "%s", pos);
  }

  if (state->status_is_error) {
    wattroff(state->status_win, COLOR_PAIR(COLOR_ERROR_TEXT));
  }

  wrefresh(state->status_win);
}

/* ============================================================================
 * Mouse Event Handling
 * ============================================================================
 */

bool tui_handle_mouse_event(TuiState *state, const UiEvent *event) {
  if (!event || event->type != UI_EVENT_MOUSE)
    return false;

  int mouse_y = event->mouse.y;
  int mouse_x = event->mouse.x;
  bool is_double = (event->mouse.action == UI_MOUSE_DOUBLE_CLICK);
  bool is_click = (event->mouse.action == UI_MOUSE_CLICK);
  bool is_scroll_up = (event->mouse.button == UI_MOUSE_SCROLL_UP);
  bool is_scroll_down = (event->mouse.button == UI_MOUSE_SCROLL_DOWN);

  /* Determine click location */
  int sidebar_width = state->sidebar_visible ? SIDEBAR_WIDTH : 0;

  /* Handle scroll wheel - scroll relative to cursor */
  if (is_scroll_up || is_scroll_down) {
    /* Only scroll in main area */
    if (mouse_x >= sidebar_width) {
      int scroll_amount = 3; /* Scroll 3 rows at a time */

      /* Check if we're in a query tab with results */
      Tab *scroll_tab = TUI_TAB(state);
      if (scroll_tab && scroll_tab->type == TAB_TYPE_QUERY &&
          scroll_tab->query_results &&
          scroll_tab->query_results->num_rows > 0) {
        /* Calculate results area position */
        int win_rows = state->term_rows - 4;
        int editor_height = (win_rows - 1) * 3 / 10;
        if (editor_height < 3)
          editor_height = 3;
        int results_start_y = 2 + editor_height + 1;

        if (mouse_y >= results_start_y) {
          /* Scroll query results using helper that handles pagination */
          UITabState *scroll_ui = TUI_TAB_UI(state);
          if (scroll_ui)
            scroll_ui->query_focus_results = true;
          int delta = is_scroll_up ? -scroll_amount : scroll_amount;
          tui_query_scroll_results(state, delta);
          tui_set_sidebar_focused(state, false);
          return true;
        }
      }

      /* Handle table data scrolling via VmTable */
      VmTable *scroll_vm = tui_vm_table(state);
      if (scroll_vm) {
        size_t cursor_row, cursor_col;
        vm_table_get_cursor(scroll_vm, &cursor_row, &cursor_col);
        size_t scroll_row, scroll_col;
        vm_table_get_scroll(scroll_vm, &scroll_row, &scroll_col);
        size_t loaded_rows = vm_table_row_count(scroll_vm);

        if (is_scroll_up) {
          /* Scroll up - move cursor up */
          if (cursor_row >= (size_t)scroll_amount) {
            cursor_row -= scroll_amount;
          } else {
            cursor_row = 0;
          }
        } else {
          /* Scroll down - move cursor down */
          cursor_row += scroll_amount;
          if (cursor_row >= loaded_rows) {
            cursor_row = loaded_rows > 0 ? loaded_rows - 1 : 0;
          }
        }

        /* Adjust scroll to keep cursor visible using actual main window height
         */
        int main_rows, main_cols;
        getmaxyx(state->main_win, main_rows, main_cols);
        (void)main_cols;
        int visible_rows = main_rows - 3; /* Minus header rows in main window */
        if (visible_rows < 1)
          visible_rows = 1;
        if (cursor_row < scroll_row) {
          scroll_row = cursor_row;
        } else if (cursor_row >= scroll_row + (size_t)visible_rows) {
          scroll_row = cursor_row - visible_rows + 1;
        }

        /* Update via viewmodel */
        vm_table_set_cursor(scroll_vm, cursor_row, cursor_col);
        vm_table_set_scroll(scroll_vm, scroll_row, scroll_col);

        /* Check if we need to load more rows (pagination) */
        tui_check_load_more(state);

        tui_set_sidebar_focused(state, false);
        tui_set_filters_focused(state, false);
      }
    }
    return true;
  }

  if (!is_click && !is_double)
    return false;

  /* Check if click is on tab bar (row 1) */
  if (mouse_y == 1 && TUI_NUM_WORKSPACES(state) > 0) {
    /* If currently editing, save the edit first */
    if (state->editing) {
      tui_confirm_edit(state);
    }

    /* Calculate which tab was clicked based on x position */
    Workspace *click_ws = TUI_WORKSPACE(state);
    if (!click_ws)
      return true;

    int x = 0;
    for (size_t i = 0; i < click_ws->num_tabs; i++) {
      Tab *click_tab = &click_ws->tabs[i];
      if (!click_tab->active)
        continue;

      const char *name = click_tab->table_name ? click_tab->table_name : "?";
      int tab_width = (int)strlen(name) + 4; /* " name  " with padding */

      if (mouse_x >= x && mouse_x < x + tab_width) {
        if (is_double) {
          /* Double-click: close the tab */
          Tab *target_tab = &click_ws->tabs[i];

          /* Check if query tab has content */
          if (target_tab->type == TAB_TYPE_QUERY &&
              ((target_tab->query_text && target_tab->query_len > 0) ||
               target_tab->query_results)) {
            /* Ask for confirmation */
            if (!tui_show_confirm_dialog(
                    state, "Close query tab with unsaved content?")) {
              return true; /* User cancelled */
            }
          }

          /* Switch to tab first if not current, then close */
          if (i != click_ws->current_tab) {
            tab_switch(state, i);
          }
          tab_close(state);
          tui_set_sidebar_focused(state, false);
          tui_set_filters_focused(state, false);
        } else {
          /* Single click: switch to tab */
          if (i != click_ws->current_tab) {
            tab_switch(state, i);
            tui_set_sidebar_focused(state, false);
            tui_set_filters_focused(state, false);
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
      tui_set_sidebar_focused(state, true);
      state->sidebar_filter_active = true;
      tui_set_filters_focused(state, false);
      return true;
    }

    /* Clicking elsewhere in sidebar deactivates filter */
    state->sidebar_filter_active = false;
    tui_set_filters_focused(state, false);

    /* Click on table list */
    int list_start_y = 3; /* First table entry row in sidebar window */
    int clicked_row = sidebar_row - list_start_y;

    if (clicked_row >= 0) {
      size_t filtered_count = tui_count_filtered_tables(state);
      char **tables = TUI_TABLES(state);
      size_t num_tables = TUI_NUM_TABLES(state);

      size_t target_idx = tui_sidebar_scroll(state) + (size_t)clicked_row;
      if (target_idx < filtered_count) {
        /* Find the actual table index */
        size_t actual_idx = tui_get_filtered_table_index(state, target_idx);
        if (actual_idx < num_tables && tables) {
          /* Update sidebar highlight */
          tui_set_sidebar_highlight(state, target_idx);
          tui_set_sidebar_focused(state, true);

          if (is_double) {
            /* Double-click: always open in new tab */
            tab_create(state, actual_idx);
            tui_set_sidebar_focused(state, false);
          } else {
            /* Single click: change current tab's table or switch to existing */
            Workspace *curr_ws = TUI_WORKSPACE(state);
            Tab *curr_tab = TUI_TAB(state);
            if (curr_ws && curr_tab) {
              if (curr_tab->type == TAB_TYPE_QUERY) {
                /* Query tab active - check if table tab already exists */
                bool found = false;
                for (size_t i = 0; i < curr_ws->num_tabs; i++) {
                  Tab *t = &curr_ws->tabs[i];
                  if (t->type == TAB_TYPE_TABLE &&
                      t->table_index == actual_idx) {
                    /* Found existing tab - switch to it */
                    tab_switch(state, i);
                    found = true;
                    break;
                  }
                }
                if (!found) {
                  /* No existing tab - create new one */
                  tab_create(state, actual_idx);
                }
                tui_set_sidebar_focused(state, false);
              } else if (curr_tab->type == TAB_TYPE_CONNECTION) {
                /* Connection tab active - convert to table tab */
                free(curr_tab->table_name);
                curr_tab->table_name = str_dup(tables[actual_idx]);
                curr_tab->type = TAB_TYPE_TABLE;
                curr_tab->table_index = actual_idx;

                /* Tab data will be loaded by tui_load_table_data */
                tui_set_sidebar_highlight(state, actual_idx);

                /* Update sidebar last position */
                state->sidebar_last_position = actual_idx;

                tui_load_table_data(state, tables[actual_idx]);
                tui_set_sidebar_focused(state, false);
              } else if (curr_tab->table_index != actual_idx) {
                /* Table tab - update current tab with new table */
                /* Update table name safely (copy first to avoid use-after-free
                 * if pointers were ever aliased) */
                char *new_name = str_dup(tables[actual_idx]);
                free(curr_tab->table_name);
                curr_tab->table_name = new_name;
                curr_tab->table_index = actual_idx;
                /* Clear filters when switching tables */
                filters_clear(&curr_tab->filters);
                /* Load new table data */
                tui_load_table_data(state, tables[actual_idx]);
              }
            } else {
              /* No tabs yet - create first one */
              tab_create(state, actual_idx);
            }
          }
          return true;
        }
      }
    }
    return true;
  }

  /* Check if click is in query tab area */
  Tab *query_click_tab = TUI_TAB(state);
  UITabState *query_click_ui = TUI_TAB_UI(state);
  if (mouse_x >= sidebar_width && query_click_tab && query_click_ui &&
      query_click_tab->type == TAB_TYPE_QUERY) {
    state->sidebar_filter_active = false;
    tui_set_sidebar_focused(state, false);
    tui_set_filters_focused(state, false);

    /* If currently editing query results, save the edit first */
    if (query_click_ui->query_result_editing) {
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
    /* Grid starts at results_start = editor_height + 1 (window coords)
     * In screen coords: 2 + editor_height + 1 (main_win starts at y=2)
     * Grid layout: column headers (1 row) + separator (1 row) + data */
    int results_start_y = 2 + editor_height + 1; /* screen coords */
    int results_data_y =
        results_start_y + 2; /* After col headers + separator */

    if (mouse_y < results_start_y) {
      /* Clicked in editor area */
      query_click_ui->query_focus_results = false;
    } else if (query_click_tab->query_results &&
               query_click_tab->query_results->num_rows > 0 &&
               mouse_y >= results_data_y) {
      /* Clicked in results data area */
      query_click_ui->query_focus_results = true;

      /* Calculate which row was clicked */
      int clicked_row = mouse_y - results_data_y;
      size_t target_row =
          query_click_tab->query_result_scroll_row + (size_t)clicked_row;

      if (target_row < query_click_tab->query_results->num_rows) {
        /* Calculate which column was clicked */
        int rel_x = mouse_x - sidebar_width;
        int x_pos = 1;
        size_t target_col = query_click_tab->query_result_scroll_col;

        for (size_t col = query_click_tab->query_result_scroll_col;
             col < query_click_tab->query_results->num_columns; col++) {
          int width = DEFAULT_COL_WIDTH;
          if (query_click_tab->query_result_col_widths &&
              col < query_click_tab->query_result_num_cols) {
            width = query_click_tab->query_result_col_widths[col];
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

        if (target_col < query_click_tab->query_results->num_columns) {
          query_click_tab->query_result_row = target_row;
          query_click_tab->query_result_col = target_col;

          /* Double-click: enter edit mode */
          if (is_double && !query_click_ui->query_result_editing) {
            tui_query_start_result_edit(state);
          }
        }
      }
    } else {
      /* Clicked in results header area */
      query_click_ui->query_focus_results = true;
    }
    return true;
  }

  /* Check if click is in filters panel (TABLE tabs only, when filters visible)
   */
  Tab *filters_click_tab = TUI_TAB(state);
  if (mouse_x >= sidebar_width && state->filters_visible && filters_click_tab &&
      filters_click_tab->type == TAB_TYPE_TABLE) {
    /* Main window starts at screen y=2 */
    int rel_y = mouse_y - 2;
    int rel_x = mouse_x - sidebar_width;
    int filters_height = tui_get_filters_panel_height(state);

    if (rel_y >= 0 && rel_y < filters_height) {
      /* Click is in filters panel area - delegate to filters handler */
      state->sidebar_filter_active = false;
      tui_handle_filters_click(state, rel_x, rel_y);
      return true;
    }
  }

  /* Check if click is in main table area */
  if (mouse_x >= sidebar_width) {
    /* Connection tab: clicking main area should not unfocus sidebar */
    Tab *main_click_tab = TUI_TAB(state);
    if (main_click_tab && main_click_tab->type == TAB_TYPE_CONNECTION) {
      return true; /* Ignore clicks in connection tab main area */
    }

    /* Clicking in main area deactivates sidebar filter and unfocuses panels */
    state->sidebar_filter_active = false;
    tui_set_sidebar_focused(state, false);
    tui_set_filters_focused(state, false);

    /* If currently editing, save the edit first */
    if (state->editing) {
      tui_confirm_edit(state);
    }

    /* Use VmTable for cursor/scroll access */
    VmTable *click_vm = tui_vm_table(state);
    if (!click_vm) {
      return true; /* No data to select, but focus is updated */
    }

    size_t loaded_rows = vm_table_row_count(click_vm);
    size_t num_cols = vm_table_col_count(click_vm);
    if (loaded_rows == 0 || num_cols == 0) {
      return true;
    }

    /* Get actual main window dimensions */
    int table_win_rows, table_win_cols;
    getmaxyx(state->main_win, table_win_rows, table_win_cols);
    (void)table_win_rows;

    /* Adjust coordinates relative to main window (starts at screen y=2) */
    int rel_x = mouse_x - sidebar_width;
    int rel_y = mouse_y - 2;

    /* Calculate data start row accounting for all header elements */
    int filters_height = 0;
    Tab *click_tab = TUI_TAB(state);
    if (state->filters_visible && click_tab &&
        click_tab->type == TAB_TYPE_TABLE) {
      filters_height = tui_get_filters_panel_height(state);
    }

    /* Data rows start after: filters panel + header line + column names +
     * sort indicator row (if sorting) + separator */
    int data_start_y =
        filters_height + 3; /* +3 = header line + col names + separator */

    /* Add extra row if sorting is active (sort indicator row) */
    if (click_tab && click_tab->num_sort_entries > 0) {
      data_start_y += 1;
    }

    int clicked_data_row = rel_y - data_start_y;

    if (clicked_data_row >= 0) {
      /* Get current scroll position */
      size_t scroll_row, scroll_col;
      vm_table_get_scroll(click_vm, &scroll_row, &scroll_col);

      /* Calculate which row was clicked */
      size_t target_row = scroll_row + (size_t)clicked_data_row;

      if (target_row < loaded_rows) {
        /* Calculate which column was clicked */
        int x_pos = 1; /* Data starts at x=1 */
        size_t target_col = scroll_col;

        for (size_t col = scroll_col; col < num_cols; col++) {
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

        if (target_col < num_cols) {
          /* Update cursor position via viewmodel */
          vm_table_set_cursor(click_vm, target_row, target_col);
          tui_set_sidebar_focused(state, false);
          tui_set_filters_focused(state, false);

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
