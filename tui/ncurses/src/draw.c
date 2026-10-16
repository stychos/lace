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
#include "core/connstr_display.h"
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
    tui_print_fit(win, y, x, width, name ? name : "");

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
      /* Sort entries index the schema: find this result column there */
      size_t schema_col = db_schema_column_index(params->sort_schema,
                                                 data->columns[col].name);
      for (size_t i = 0; i < params->num_sort_entries; i++) {
        if (schema_col == SIZE_MAX)
          break;
        if (params->sort_entries[i].column == schema_col) {
          /* Show: arrow + direction + priority (e.g., "▲ asc, 1") */
          const char *arrow = (params->sort_entries[i].direction == SORT_ASC)
                                  ? "\xE2\x96\xB2"  /* ▲ UTF-8 */
                                  : "\xE2\x96\xBC"; /* ▼ UTF-8 */
          const char *dir_text =
              (params->sort_entries[i].direction == SORT_ASC) ? "asc" : "desc";
          char sort_info[24];
          snprintf(sort_info, sizeof(sort_info), "%s %s, %d", arrow, dir_text,
                   (int)(i + 1));
          tui_print_fit(win, y, x, width, sort_info);
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
        int cursor_x =
            tui_draw_edit_field(win, y, x, width, params->edit_buffer,
                                params->edit_pos, COLOR_PAIR(COLOR_EDIT));
        if (cursor_x >= 0) {
          /* Store cursor screen position for later use */
          if (state) {
            state->edit_cursor_y = y;
            state->edit_cursor_x = cursor_x;
          }
        }
      } else if (is_selected) {
        /* Check if this column is a primary key */
        size_t sc = db_schema_column_index(effective_schema, data->columns[col].name);
        bool is_pk_col = sc != SIZE_MAX && effective_schema->columns[sc].primary_key;

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
            tui_print_fit(win, y, x, width, safe ? safe : str);
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
        size_t sc = db_schema_column_index(effective_schema, data->columns[col].name);
        bool is_pk_col = sc != SIZE_MAX && effective_schema->columns[sc].primary_key;

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
            tui_print_fit(win, y, x, width, safe ? safe : str);
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

char *tui_connstr_display(const char *connstr) {
  return connstr_display(connstr);
}

void tui_draw_header(TuiState *state) {
  if (!state || !state->header_win || !state->header_visible)
    return;

  werase(state->header_win);
  wbkgd(state->header_win, COLOR_PAIR(COLOR_HEADER));

  /* What the user connected to, from the connection string they used (the
   * daemon's view of an SSH tunnel is a local socket path): never a
   * password, fitted to the left two thirds of the line */
  Connection *app_conn = TUI_TAB_CONNECTION(state);
  char *where = app_conn ? tui_connstr_display(app_conn->connstr) : NULL;
  if (where) {
    int max_x = getmaxx(state->header_win);
    tui_print_clip(state->header_win, 0, 1, max_x * 2 / 3, where);
    free(where);
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
  const ColumnDef *cdef =
      schema && col < schema->num_columns ? &schema->columns[col] : NULL;
  /* Placeholders the database fills in: auto-increment, generated, or a
   * required column with no default (left for the database to reject) */
  const char *auto_label = NULL;
  if (cdef && cdef->generated)
    auto_label = "GENERATED";
  else if (cdef && cdef->auto_increment)
    auto_label = "AI";
  else if (cdef && !cdef->nullable &&
           !(cdef->default_val && cdef->default_val[0]))
    auto_label = "required";
  bool is_auto_increment = auto_label != NULL;
  bool is_placeholder = state->new_row_placeholders[col] && !is_edited;

  if (is_editing) {
    /* Draw edit field with distinctive background */
    int cursor_x = tui_draw_edit_field(win, y, x, width,
                                       state->new_row_edit_buffer,
                                       state->new_row_edit_pos,
                                       COLOR_PAIR(COLOR_EDIT));
    if (cursor_x >= 0)
      wmove(win, y, cursor_x);
  } else if (is_selected) {
    wattron(win, COLOR_PAIR(COLOR_SELECTED));
    mvwhline(win, y, x, ' ', width);

    if (is_placeholder && is_auto_increment) {
      /* Auto-increment placeholder - show in dim */
      wattron(win, A_DIM);
      tui_print_fit(win, y, x, width, auto_label);
      wattroff(win, A_DIM);
    } else if (is_placeholder) {
      /* Default value placeholder - show in dim */
      wattron(win, A_DIM);
      const DbValue *val = &state->new_row_values[col];
      char *str = db_value_to_string(val);
      tui_print_fit(win, y, x, width, str ? str : "");
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
          tui_print_fit(win, y, x, width, safe ? safe : str);
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
      tui_print_fit(win, y, x, width, auto_label);
      wattroff(win, A_DIM);
    } else if (is_placeholder) {
      /* Default value placeholder - gray/dim */
      wattron(win, A_DIM);
      const DbValue *val = &state->new_row_values[col];
      char *str = db_value_to_string(val);
      tui_print_fit(win, y, x, width, str ? str : "");
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
          tui_print_fit(win, y, x, width, safe ? safe : str);
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
  Tab *overlay_tab = TUI_TAB(state);
  size_t scroll_col = overlay_tab ? overlay_tab->scroll_col : 0;

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

/* Print text centered on row y, clipped to the window width (UTF-8 aware) */
static void draw_centered_clip(WINDOW *win, int y, int win_cols,
                               const char *text) {
  int width = utf8_width(text, strlen(text));
  int x = width < win_cols ? (win_cols - width) / 2 : 0;
  tui_print_clip(win, y, x, win_cols - x, text);
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
  if (tui_filters_visible(state)) {
    filters_height = tui_get_filters_panel_height(state);
    tui_draw_filters_panel(state);
  }

  /* Get tab for model data, widget for view state */
  Tab *tab = TUI_TAB(state);
  VmTable *widget = state->vm_table;
  if (!tab) {
    wrefresh(state->main_win);
    return;
  }

  /* Read model data from Tab */
  ResultSet *data = tab->data;

  /* Keep widths in step with the loaded rows (pages loaded after the first
   * one used to keep the first page's widths and truncate wider values) */
  if (widget) {
    table_vm_update_column_widths(widget);
  }

  /* Column widths from the viewmodel; cursor and scroll from the tab */
  int *col_widths = widget ? widget->col_widths : tab->col_widths;
  size_t num_col_widths = widget ? widget->num_col_widths : tab->num_col_widths;
  size_t cursor_row = tab->cursor_row;
  size_t cursor_col = tab->cursor_col;
  size_t scroll_row = tab->scroll_row;
  size_t scroll_col = tab->scroll_col;

  bool is_editing = state->editing;
  char *edit_buffer = state->edit_buffer;
  size_t edit_pos = state->edit_pos;

  /* Check if there's a table error (e.g., table doesn't exist) */
  if (tab && tab->table_error) {
    int center_y = filters_height + (win_rows - filters_height) / 2;
    wattron(state->main_win, COLOR_PAIR(COLOR_ERROR_TEXT) | A_BOLD);
    draw_centered_clip(state->main_win, center_y - 2, win_cols,
                       "Failed to load table:");
    wattroff(state->main_win, A_BOLD);
    draw_centered_clip(state->main_win, center_y - 1, win_cols,
                       tab->table_name ? tab->table_name : "(unknown)");

    /* Show actual error message (clipped to the window) */
    draw_centered_clip(state->main_win, center_y + 1, win_cols,
                       tab->table_error);
    wattroff(state->main_win, COLOR_PAIR(COLOR_ERROR_TEXT));

    wattron(state->main_win, A_DIM);
    char *close_key = hotkey_get_display(state->app->config, HOTKEY_CLOSE_TAB);
    char hint[128];
    snprintf(hint, sizeof(hint), "Press [%s] to close this tab",
             close_key ? close_key : "-");
    free(close_key);
    draw_centered_clip(state->main_win, center_y + 3, win_cols, hint);
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
                           .selection_offset = tab->loaded_offset,
                           .is_focused = !tui_sidebar_focused(state) &&
                                         !tui_filters_focused(state),
                           .is_editing = is_editing,
                           .edit_buffer = edit_buffer,
                           .edit_pos = edit_pos,
                           .show_header_line = true,
                           .sort_entries = tab ? tab->sort_entries : NULL,
                           .num_sort_entries = tab ? tab->num_sort_entries : 0,
                           .sort_schema = tab ? tab->schema : NULL};

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
  tui_print_clip(state->main_win, center_y + 2, connstr_x, max_connstr_len, connstr);
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


/* Row position at the right of the status bar, prefixed with the
 * connection's transaction state so an open (or lost) transaction is never
 * invisible */
static void draw_status_position(TuiState *state, int right_pos, const char *pos) {
  const char *tx = "";
  if (TUI_CONN(state) && TUI_CONN(state)->transaction_lost) {
    tx = "[TX LOST] ";
  } else if (TUI_CONN(state) && TUI_CONN(state)->in_transaction &&
             TUI_CONN(state)->transaction_failed) {
    tx = "[TX FAILED] ";
  } else if (TUI_CONN(state) && TUI_CONN(state)->in_transaction) {
    tx = "[TX] ";
  }
  char text[160];
  snprintf(text, sizeof(text), "%s%s", tx, pos);
  int len = (int)strlen(text);
  int x = right_pos - len;
  mvwprintw(state->status_win, 0, x < 0 ? 0 : x, "%s", text);
}

/* ============================================================================
 * Status Bar Drawing
 * ============================================================================
 */

/* Status bar, left: name, type and flags of a column, clipped to the bar */
static void draw_column_info(TuiState *state, const char *prefix,
                             const ColumnDef *col) {
  StringBuilder *sb = sb_new(128);
  sb_printf(sb, "%s%s", prefix, col->name ? col->name : "?");
  if (col->type_name)
    sb_printf(sb, " : %s", col->type_name);
  if (col->primary_key)
    sb_append(sb, " [PK]");
  if (col->auto_increment)
    sb_append(sb, " [AI]");
  if (!col->nullable)
    sb_append(sb, " NOT NULL");
  if (col->default_val)
    sb_printf(sb, " DEFAULT %s", col->default_val);
  char *info = sb_finish(sb);
  if (info) {
    tui_print_clip(state->status_win, 0, 1, state->term_cols - 2, info);
    free(info);
  }
}

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
  VmTable *widget = state->vm_table;
  bool query_results_active = false;
  if (tab && ui) {
    query_results_active = (tab->type == TAB_TYPE_QUERY &&
                            ui->query_focus_results && !ui->query_unfocused &&
                            tab->query_results);
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

    draw_column_info(state, "", display_col);
  } else if (state->adding_row && tab && tab->schema &&
             state->new_row_cursor_col < tab->schema->num_columns) {
    /* Add-row mode: show column info for current cursor position */
    ColumnDef *col = &tab->schema->columns[state->new_row_cursor_col];

    draw_column_info(state, "[+] ", col);
  } else if (tab && tab->schema && widget && tab->data &&
             tab->cursor_col < tab->data->num_columns) {
    /* The cursor is on a result column: its schema column by name */
    size_t sc = db_schema_column_index(tab->schema,
                                       tab->data->columns[tab->cursor_col].name);
    if (sc != SIZE_MAX) {
      draw_column_info(state, "", &tab->schema->columns[sc]);
    }
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
    if (tab->query_results->num_rows == 0) {
      snprintf(pos, sizeof(pos), "No rows");
    } else if (tab->query_paginated) {
      /* Paginated: show actual row number in total dataset ("N+" while the
       * total is still being counted) */
      size_t actual_row = tab->query_loaded_offset + tab->query_result_row + 1;
      if (tab->query_total_known) {
        snprintf(pos, sizeof(pos), "Row %zu/%zu", actual_row,
                 tab->query_total_rows);
      } else {
        snprintf(pos, sizeof(pos), "Row %zu/%zu+", actual_row,
                 tab->query_loaded_offset + tab->query_loaded_count);
      }
    } else {
      /* Non-paginated */
      snprintf(pos, sizeof(pos), "Row %zu/%zu", tab->query_result_row + 1,
               tab->query_results->num_rows);
    }
    draw_status_position(state, right_pos, pos);
  } else if (tab && tab->data && widget) {
    char pos[96];
    size_t actual_row = tab->loaded_offset + tab->cursor_row + 1;
    size_t total =
        tab->total_rows > 0 ? tab->total_rows : tab->data->num_rows;
    if (tab->data->num_rows == 0)
      actual_row = 0; /* "Row 0/0": nothing under the cursor */
    /* Show ~ prefix for approximate counts */
    bool approx = tab->row_count_approximate;
    bool filtered = has_active_filters(tab);
    if (filtered && tab->unfiltered_total_rows > 0) {
      snprintf(pos, sizeof(pos), "Row %zu/%s%zu [%zu]", actual_row,
               approx ? "~" : "", total, tab->unfiltered_total_rows);
    } else {
      snprintf(pos, sizeof(pos), "Row %zu/%s%zu", actual_row, approx ? "~" : "",
               total);
    }
    draw_status_position(state, right_pos, pos);
  } else if (TUI_CONN(state) &&
             (TUI_CONN(state)->in_transaction || TUI_CONN(state)->transaction_lost)) {
    draw_status_position(state, right_pos, "");
  }

  if (state->status_is_error) {
    wattroff(state->status_win, COLOR_PAIR(COLOR_ERROR_TEXT));
  }

  wrefresh(state->status_win);
}
