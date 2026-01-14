/*
 * Lace
 * Query tab implementation - Main coordinator
 *
 * Uses QueryViewModel as source of truth for query editor state.
 * Access via tui_query_widget_for_tab() to get the view model.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../util/mem.h"
#include "../../viewmodel/query_viewmodel.h"
#include "query_internal.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Note: History recording is now handled automatically by the database layer
 * via the history callback set up in app_add_connection(). */

/* Create a new query tab */
bool tab_create_query(TuiState *state) {
  if (!state || !state->app)
    return false;

  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws) {
    /* No workspace - need to create one first */
    ws = app_create_workspace(state->app);
    if (!ws)
      return false;
  }

  /* Get connection index - use current tab's connection if available */
  size_t connection_index = 0;
  Tab *current_tab = TUI_TAB(state);
  if (current_tab) {
    connection_index = current_tab->connection_index;
  }

  /* Save current tab first */
  if (ws->num_tabs > 0) {
    tab_save(state);
  }

  /* Find next query number by scanning existing tabs */
  int max_query_num = 0;
  for (size_t i = 0; i < ws->num_tabs; i++) {
    Tab *t = &ws->tabs[i];
    if (t->type == TAB_TYPE_QUERY && t->table_name) {
      int num = 0;
      if (sscanf(t->table_name, "Query %d", &num) == 1) {
        if (num > max_query_num)
          max_query_num = num;
      }
    }
  }

  /* Create new tab with connection reference */
  Tab *tab = workspace_create_query_tab(ws, connection_index);
  if (!tab)
    return false;

  /* Ensure UITabState capacity for new tab */
  tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                             ws->current_tab);

  /* Initialize UITabState for new query tab (source of truth) */
  UITabState *ui = TUI_TAB_UI(state);
  if (ui) {
    /* Inherit current sidebar state */
    ui->sidebar_visible = state->sidebar_visible;
    ui->sidebar_focused = false;
    ui->sidebar_highlight = state->sidebar_highlight;
    ui->sidebar_scroll = state->sidebar_scroll;
    ui->sidebar_filter_len = state->sidebar_filter_len;
    memcpy(ui->sidebar_filter, state->sidebar_filter,
           sizeof(ui->sidebar_filter));

    /* Query tab has no filters panel */
    ui->filters_visible = false;
    ui->filters_focused = false;

    /* Query starts with focus on editor, not results */
    ui->query_focus_results = false;
  }

  /* Update tab name (free the default "Query" name set by
   * workspace_create_query_tab) */
  free(tab->table_name);
  tab->table_name = str_printf("Query %d", max_query_num + 1);
  if (!tab->table_name) {
    workspace_close_tab(ws, ws->current_tab);
    return false;
  }

  /* Initialize query buffer */
  tab->query_capacity = QUERY_INITIAL_CAPACITY;
  tab->query_text = safe_calloc(tab->query_capacity, 1);
  tab->query_len = 0;
  tab->query_cursor = 0;
  tab->query_scroll_line = 0;
  tab->query_scroll_col = 0;

  /* Query mode doesn't use table data - Tab starts clean */

  /* Reset TUI state for new tab - all panels start unfocused, filters closed */
  state->sidebar_focused = false;
  state->filters_visible = false;
  state->filters_focused = false;
  state->filters_was_focused = false;
  state->filters_editing = false;
  state->filters_cursor_row = 0;
  state->filters_cursor_col = 0;
  state->filters_scroll = 0;

  /* Initialize widgets for the new query tab */
  if (ui) {
    tui_init_query_tab_widgets(state, ui, tab);
  }

  char *run_key = hotkey_get_display(state->app->config, HOTKEY_EXECUTE_QUERY);
  char *all_key = hotkey_get_display(state->app->config, HOTKEY_EXECUTE_ALL);
  tui_set_status(state, "Query tab opened. %s to run, %s to run all",
                 run_key ? run_key : "Ctrl+R", all_key ? all_key : "Ctrl+A");
  free(run_key);
  free(all_key);
  return true;
}

/* Legacy wrapper for compatibility */
bool workspace_create_query(TuiState *state) { return tab_create_query(state); }

/* Load query result rows at specific offset (replaces current data) */
bool query_load_rows_at(TuiState *state, Tab *tab, size_t offset) {
  if (!state || !state->conn || !tab || !tab->query_paginated ||
      !tab->query_base_sql)
    return false;

  /* Clamp offset */
  if (offset >= tab->query_total_rows) {
    offset = tab->query_total_rows > PAGE_SIZE
                 ? tab->query_total_rows - PAGE_SIZE
                 : 0;
  }

  char *paginated_sql = str_printf("%s LIMIT %d OFFSET %zu",
                                   tab->query_base_sql, PAGE_SIZE, offset);
  if (!paginated_sql)
    return false;

  char *err = NULL;
  ResultSet *data = db_query(state->conn, paginated_sql, &err);
  free(paginated_sql);

  if (!data) {
    tui_set_error(state, "Query failed: %s", err ? err : "Unknown error");
    free(err);
    return false;
  }

  /* Free old results */
  if (tab->query_results) {
    db_result_free(tab->query_results);
  }

  tab->query_results = data;
  tab->query_loaded_offset = offset;
  tab->query_loaded_count = data->num_rows;

  /* Recalculate column widths */
  free(tab->query_result_col_widths);
  tab->query_result_col_widths = NULL;
  if (data->num_columns > 0) {
    tab->query_result_col_widths = safe_calloc(data->num_columns, sizeof(int));
    for (size_t c = 0; c < data->num_columns; c++) {
      int w = data->columns[c].name ? (int)strlen(data->columns[c].name) : 0;
      if (w < 8)
        w = 8;
      for (size_t r = 0; r < data->num_rows && r < 100; r++) {
        if (r < data->num_rows && c < data->rows[r].num_cells) {
          DbValue *v = &data->rows[r].cells[c];
          int vw = 0;
          if (v->type == DB_TYPE_TEXT && v->text.data) {
            vw = (int)strlen(v->text.data);
          } else if (v->type == DB_TYPE_INT) {
            vw = 12;
          } else if (v->type == DB_TYPE_FLOAT) {
            vw = 15;
          }
          if (vw > w)
            w = vw;
        }
      }
      if (w > 50)
        w = 50;
      tab->query_result_col_widths[c] = w;
    }
  }

  tui_set_status(state, "Loaded %zu/%zu rows", tab->query_loaded_count,
                 tab->query_total_rows);
  return true;
}

/* Draw the query tab */
void tui_draw_query(TuiState *state) {
  if (!state || !state->main_win)
    return;

  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || tab->type != TAB_TYPE_QUERY || !ui)
    return;

  /* Use QueryViewModel for cursor/scroll/focus state */
  QueryWidget *qw = tui_query_widget_for_tab(state);
  size_t query_cursor, query_scroll_line;
  bool focus_results;

  if (qw) {
    query_cursor = qw->cursor_offset;
    query_scroll_line = qw->base.state.scroll_row;
    focus_results = (qw->focus_panel == QUERY_FOCUS_RESULTS);
  } else {
    query_cursor = tab->query_cursor;
    query_scroll_line = tab->query_scroll_line;
    focus_results = ui->query_focus_results;
  }

  werase(state->main_win);

  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);

  /* Split view: editor on top, results on bottom */
  int editor_height =
      (win_rows - 1) * 3 / 10; /* 30% for editor, -1 for separator */
  if (editor_height < 3)
    editor_height = 3; /* Minimum 3 lines for editor */
  int results_start = editor_height + 1;
  (void)(win_rows -
         results_start); /* results_height calculated but not stored */

  /* Build line cache */
  QueryLineInfo *lines = NULL;
  size_t num_lines = 0;
  query_rebuild_line_cache(tab, &lines, &num_lines);

  /* Get cursor line/col using widget cursor if available */
  size_t cursor_line, cursor_col;
  size_t saved_cursor = tab->query_cursor;
  tab->query_cursor = query_cursor; /* Temporarily set for line calculation */
  query_cursor_to_line_col(tab, &cursor_line, &cursor_col);
  tab->query_cursor = saved_cursor;

  /* Adjust scroll to keep cursor visible */
  if (cursor_line < query_scroll_line) {
    query_scroll_line = cursor_line;
  } else if (cursor_line >= query_scroll_line + (size_t)(editor_height - 1)) {
    query_scroll_line = cursor_line - (size_t)editor_height + 2;
  }
  /* Sync scroll back to widget/tab */
  if (qw) {
    qw->base.state.scroll_row = query_scroll_line;
  }
  tab->query_scroll_line = query_scroll_line;

  /* Find current query bounds for highlighting */
  size_t query_start = 0, query_end = 0;
  bool has_query_bounds =
      query_find_bounds_at_cursor(tab->query_text, query_cursor, &query_start, &query_end);

  /* Draw editor area */
  if (!focus_results) {
    wattron(state->main_win, A_BOLD);
  }
  mvwprintw(state->main_win, 0, 1,
            "SQL Query (^R: run, ^A: all, ^T: transaction, ^W: switch)");
  if (!ui->query_focus_results) {
    wattroff(state->main_win, A_BOLD);
  }

  /* Draw query text */
  for (int y = 1; y < editor_height &&
                  tab->query_scroll_line + (size_t)(y - 1) < num_lines;
       y++) {
    size_t line_idx = tab->query_scroll_line + (size_t)(y - 1);
    QueryLineInfo *li = &lines[line_idx];

    /* Check if this line is within the current query bounds */
    size_t line_start = li->start;
    size_t line_end = li->start + li->len;
    bool line_in_query =
        has_query_bounds && line_end > query_start && line_start < query_end;

    /* Dim lines outside the current query */
    bool is_dimmed = has_query_bounds && !line_in_query;

    /* Line number - dim if outside current query, normal otherwise */
    if (is_dimmed) {
      wattron(state->main_win, A_DIM);
    }
    mvwprintw(state->main_win, y, 0, "%3zu", line_idx + 1);
    if (is_dimmed) {
      wattroff(state->main_win, A_DIM);
    }

    /* Line content - dim if outside current query */
    if (is_dimmed) {
      wattron(state->main_win, A_DIM);
    }

    int x = 4;
    for (size_t i = 0; i < li->len && x < win_cols - 1; i++) {
      char c = tab->query_text[li->start + i];
      if (c == '\t') {
        /* Tab as spaces */
        for (int t = 0; t < 4 && x < win_cols - 1; t++) {
          mvwaddch(state->main_win, y, x++, ' ');
        }
      } else if (c >= 32 && c < 127) {
        mvwaddch(state->main_win, y, x++, c);
      }
    }

    if (is_dimmed) {
      wattroff(state->main_win, A_DIM);
    }

    /* Draw cursor if on this line and editor focused */
    if (!ui->query_focus_results && line_idx == cursor_line) {
      int cursor_x = 4 + (int)cursor_col;
      if (cursor_x < win_cols) {
        char cursor_char = ' ';
        if (cursor_col < li->len) {
          cursor_char = tab->query_text[li->start + cursor_col];
          if (cursor_char < 32 || cursor_char >= 127)
            cursor_char = ' ';
        }
        wattron(state->main_win, A_REVERSE);
        mvwaddch(state->main_win, y, cursor_x, cursor_char);
        wattroff(state->main_win, A_REVERSE);
      }
    }
  }

  /* Draw scrollbar if content exceeds visible area */
  int visible_rows = editor_height - 1; /* Minus header line */
  if (num_lines > (size_t)visible_rows && visible_rows > 0) {
    int scroll_x = win_cols - 1;
    int thumb_pos =
        (int)(tab->query_scroll_line * (size_t)visible_rows / num_lines);
    int thumb_size = (visible_rows * visible_rows) / (int)num_lines;
    if (thumb_size < 1)
      thumb_size = 1;

    wattron(state->main_win, A_DIM);
    for (int i = 0; i < visible_rows; i++) {
      mvwaddch(state->main_win, 1 + i, scroll_x,
               (i >= thumb_pos && i < thumb_pos + thumb_size) ? ACS_CKBOARD
                                                              : ACS_VLINE);
    }
    wattroff(state->main_win, A_DIM);
  }

  free(lines);

  /* Draw separator */
  wattron(state->main_win, COLOR_PAIR(COLOR_BORDER));
  mvwhline(state->main_win, editor_height, 0, ACS_HLINE, win_cols);
  wattroff(state->main_win, COLOR_PAIR(COLOR_BORDER));

  /* Draw results area */
  if (tab->query_error) {
    /* Show error */
    wattron(state->main_win, COLOR_PAIR(COLOR_ERROR));
    mvwprintw(state->main_win, results_start, 1, "Error: %s", tab->query_error);
    wattroff(state->main_win, COLOR_PAIR(COLOR_ERROR));
  } else if (tab->query_results && tab->query_results->num_columns > 0) {
    /* Use shared grid drawing function */
    int results_height = win_rows - results_start;
    GridDrawParams params = {.win = state->main_win,
                             .start_y = results_start,
                             .start_x = 0,
                             .height = results_height,
                             .width = win_cols,
                             .data = tab->query_results,
                             .col_widths = tab->query_result_col_widths,
                             .num_col_widths = tab->query_result_num_cols,
                             .cursor_row = tab->query_result_row,
                             .cursor_col = tab->query_result_col,
                             .scroll_row = tab->query_result_scroll_row,
                             .scroll_col = tab->query_result_scroll_col,
                             .selection_offset = tab->query_loaded_offset,
                             .is_focused = ui->query_focus_results,
                             .is_editing = ui->query_result_editing,
                             .edit_buffer = ui->query_result_edit_buf,
                             .edit_pos = ui->query_result_edit_pos,
                             .show_header_line = false,
                             .sort_entries = NULL,
                             .num_sort_entries = 0};

    tui_draw_result_grid(state, &params);
  } else if (tab->query_exec_success) {
    /* Show success message for non-SELECT queries */
    wattron(state->main_win, COLOR_PAIR(COLOR_STATUS));
    if (tab->query_affected > 0) {
      mvwprintw(state->main_win, results_start + 1, 1, "%lld rows affected",
                (long long)tab->query_affected);
    } else {
      mvwprintw(state->main_win, results_start + 1, 1,
                "Statement executed successfully");
    }
    wattroff(state->main_win, COLOR_PAIR(COLOR_STATUS));
  } else {
    /* No results yet */
    wattron(state->main_win, A_DIM);
    char *exec_key =
        hotkey_get_display(state->app->config, HOTKEY_EXECUTE_QUERY);
    mvwprintw(state->main_win, results_start + 1, 1,
              "Enter SQL and press %s to execute",
              exec_key ? exec_key : "Ctrl+R");
    free(exec_key);
    wattroff(state->main_win, A_DIM);
  }

  wrefresh(state->main_win);
}

/* Public wrapper for starting edit from mouse handler */
void tui_query_start_result_edit(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !ui)
    return;
  if (tab->type != TAB_TYPE_QUERY || !ui->query_focus_results)
    return;
  query_result_start_edit(state, tab);
}

/* Public wrapper for confirming edit from mouse handler */
void tui_query_confirm_result_edit(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !ui)
    return;
  if (tab->type != TAB_TYPE_QUERY || !ui->query_result_editing)
    return;
  query_result_confirm_edit(state, tab);
}

/* Public wrapper for scrolling query results (used by mouse handler) */
void tui_query_scroll_results(TuiState *state, int delta) {
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;
  if (tab->type != TAB_TYPE_QUERY || !tab->query_results)
    return;
  if (tab->query_results->num_rows == 0)
    return;

  /* Calculate visible rows for scroll adjustment using actual main window */
  int main_rows, main_cols;
  getmaxyx(state->main_win, main_rows, main_cols);
  (void)main_cols;
  int win_rows = main_rows;
  int editor_height = (win_rows - 1) * 3 / 10;
  if (editor_height < 3)
    editor_height = 3;
  int visible = win_rows - editor_height - 4;
  if (visible < 1)
    visible = 1;

  if (delta < 0) {
    /* Scroll up */
    size_t amount = (size_t)(-delta);
    if (tab->query_result_row >= amount) {
      tab->query_result_row -= amount;
    } else {
      tab->query_result_row = 0;
    }
  } else if (delta > 0) {
    /* Scroll down */
    tab->query_result_row += (size_t)delta;
    if (tab->query_result_row >= tab->query_results->num_rows) {
      tab->query_result_row = tab->query_results->num_rows > 0
                                  ? tab->query_results->num_rows - 1
                                  : 0;
    }
  }

  /* Adjust scroll position */
  if (tab->query_result_row < tab->query_result_scroll_row) {
    tab->query_result_scroll_row = tab->query_result_row;
  } else if (tab->query_result_row >=
             tab->query_result_scroll_row + (size_t)visible) {
    tab->query_result_scroll_row = tab->query_result_row - visible + 1;
  }

  /* Check if more rows need to be loaded */
  query_check_load_more(state, tab);
}

/* Handle query tab input */
bool tui_handle_query_input(TuiState *state, const UiEvent *event) {
  if (!state)
    return false;
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || tab->type != TAB_TYPE_QUERY || !ui)
    return false;

  int key_char = render_event_get_char(event);
  const Config *cfg = state->app ? state->app->config : NULL;

  /* Handle edit mode first if active */
  if (ui->query_result_editing) {
    return query_result_handle_edit_input(state, tab, event);
  }

  /* Ctrl+W or Esc toggles focus between editor and results */
  if (hotkey_matches(cfg, event, HOTKEY_QUERY_SWITCH_FOCUS)) {
    ui->query_focus_results = !ui->query_focus_results;
    return true;
  }

  /* Handle results navigation when focused on results */
  if (ui->query_focus_results) {
    if (!tab->query_results) {
      /* No results yet - up arrow switches to editor */
      if (hotkey_matches(cfg, event, HOTKEY_MOVE_UP)) {
        ui->query_focus_results = false;
        return true;
      }
      return false;
    }

    /* Enter - start editing */
    if (hotkey_matches(cfg, event, HOTKEY_EDIT_INLINE)) {
      query_result_start_edit(state, tab);
      return true;
    }

    /* e or F4 - start modal editing */
    if (hotkey_matches(cfg, event, HOTKEY_EDIT_MODAL)) {
      query_result_start_modal_edit(state, tab);
      return true;
    }

    /* Ctrl+N or n - set cell to NULL */
    if (hotkey_matches(cfg, event, HOTKEY_SET_NULL)) {
      query_result_set_cell_direct(state, tab, true);
      return true;
    }

    /* Ctrl+D or d - set cell to empty string */
    if (hotkey_matches(cfg, event, HOTKEY_SET_EMPTY)) {
      query_result_set_cell_direct(state, tab, false);
      return true;
    }

    /* c or Ctrl+K - copy cell to clipboard */
    if (hotkey_matches(cfg, event, HOTKEY_CELL_COPY)) {
      query_result_cell_copy(state, tab);
      return true;
    }

    /* v or Ctrl+U - paste from clipboard to cell */
    if (hotkey_matches(cfg, event, HOTKEY_CELL_PASTE)) {
      query_result_cell_paste(state, tab);
      return true;
    }

    /* x or Delete - delete row(s) */
    if (hotkey_matches(cfg, event, HOTKEY_DELETE_ROW)) {
      query_result_delete_row(state, tab);
      return true;
    }

    /* Space - toggle row selection */
    if (hotkey_matches(cfg, event, HOTKEY_TOGGLE_SELECTION)) {
      /* Overflow check for global row calculation */
      if (tab->query_result_row > SIZE_MAX - tab->query_loaded_offset)
        return true; /* Silently ignore on overflow */
      size_t global_row = tab->query_loaded_offset + tab->query_result_row;
      tab_toggle_selection(tab, global_row);
      return true;
    }

    /* Escape - clear selections (only if there are selections) */
    if (hotkey_matches(cfg, event, HOTKEY_CLEAR_SELECTIONS)) {
      if (tab->num_selected > 0) {
        tab_clear_selections(tab);
        return true;
      }
      /* If no selections, let Escape switch focus to editor */
    }

    /* r/R or Ctrl+R - refresh query results */
    if (hotkey_matches(cfg, event, HOTKEY_REFRESH) ||
        hotkey_matches(cfg, event, HOTKEY_EXECUTE_QUERY)) {
      /* Re-execute the base SQL if available, otherwise find query at cursor.
       * IMPORTANT: Must copy the SQL before calling query_execute because
       * query_execute frees tab->query_base_sql before re-using it. */
      char *sql_copy = NULL;
      if (tab->query_base_sql && *tab->query_base_sql) {
        sql_copy = str_dup(tab->query_base_sql);
      } else if (tab->query_text && *tab->query_text) {
        /* Fall back to finding query at cursor */
        sql_copy = query_find_at_cursor(tab->query_text, tab->query_cursor);
      }
      if (sql_copy && *sql_copy) {
        query_execute(state, sql_copy);
      }
      free(sql_copy);
      return true;
    }

    /* Up / k - move cursor up */
    if (hotkey_matches(cfg, event, HOTKEY_MOVE_UP)) {
      if (tab->query_result_row > 0) {
        tab->query_result_row--;
        /* Adjust scroll */
        if (tab->query_result_row < tab->query_result_scroll_row) {
          tab->query_result_scroll_row = tab->query_result_row;
        }
        query_check_load_more(state, tab);
      } else {
        /* At top row - switch focus to editor */
        ui->query_focus_results = false;
      }
      return true;
    }

    /* Down / j - move cursor down */
    if (hotkey_matches(cfg, event, HOTKEY_MOVE_DOWN)) {
      if (tab->query_results->num_rows > 0 &&
          tab->query_result_row < tab->query_results->num_rows - 1) {
        tab->query_result_row++;
        /* Adjust scroll based on actual main window height */
        int down_rows, down_cols;
        getmaxyx(state->main_win, down_rows, down_cols);
        (void)down_cols;
        int editor_height = (down_rows - 1) * 3 / 10;
        if (editor_height < 3)
          editor_height = 3;
        int visible = down_rows - editor_height - 4;
        if (visible < 1)
          visible = 1;
        if (tab->query_result_row >=
            tab->query_result_scroll_row + (size_t)visible) {
          tab->query_result_scroll_row = tab->query_result_row - visible + 1;
        }
        query_check_load_more(state, tab);
      }
      return true;
    }

    /* Left / h - move cursor left */
    if (hotkey_matches(cfg, event, HOTKEY_MOVE_LEFT)) {
      if (tab->query_result_col > 0) {
        tab->query_result_col--;
        /* Adjust horizontal scroll */
        if (tab->query_result_col < tab->query_result_scroll_col) {
          tab->query_result_scroll_col = tab->query_result_col;
        }
      } else if (state->sidebar_visible) {
        /* At left-most column - move focus to sidebar */
        state->sidebar_focused = true;
        /* Restore last sidebar position */
        state->sidebar_highlight = state->sidebar_last_position;
      }
      return true;
    }

    /* Right / l - move cursor right */
    if (hotkey_matches(cfg, event, HOTKEY_MOVE_RIGHT)) {
      if (tab->query_result_col < tab->query_results->num_columns - 1) {
        tab->query_result_col++;
        /* Adjust horizontal scroll to keep cursor visible using actual main
         * window */
        int right_rows, right_cols;
        getmaxyx(state->main_win, right_rows, right_cols);
        (void)right_rows;
        int avail_width = right_cols;
        int x = 1;
        size_t last_visible = tab->query_result_scroll_col;
        for (size_t col = tab->query_result_scroll_col;
             col < tab->query_results->num_columns; col++) {
          int w = tab->query_result_col_widths
                      ? tab->query_result_col_widths[col]
                      : 15;
          if (x + w + 3 > avail_width)
            break;
          x += w + 1;
          last_visible = col;
        }
        if (tab->query_result_col > last_visible) {
          tab->query_result_scroll_col = tab->query_result_col;
          /* Adjust to show as many columns as possible */
          x = 1;
          while (tab->query_result_scroll_col > 0) {
            int w =
                tab->query_result_col_widths
                    ? tab->query_result_col_widths[tab->query_result_scroll_col]
                    : 15;
            if (x + w + 3 > avail_width)
              break;
            x += w + 1;
            if (tab->query_result_scroll_col == tab->query_result_col)
              break;
            tab->query_result_scroll_col--;
          }
        }
      }
      return true;
    }

    /* Home */
    if (hotkey_matches(cfg, event, HOTKEY_FIRST_COL)) {
      tab->query_result_col = 0;
      tab->query_result_scroll_col = 0;
      return true;
    }

    /* End */
    if (hotkey_matches(cfg, event, HOTKEY_LAST_COL)) {
      if (tab->query_results->num_columns > 0) {
        tab->query_result_col = tab->query_results->num_columns - 1;
        /* Adjust horizontal scroll to show last column using actual main window
         */
        int end_rows, end_cols;
        getmaxyx(state->main_win, end_rows, end_cols);
        (void)end_rows;
        int avail_width = end_cols;
        tab->query_result_scroll_col = tab->query_result_col;
        /* Adjust to show as many columns as possible */
        int x = 1;
        while (tab->query_result_scroll_col > 0) {
          int w =
              tab->query_result_col_widths
                  ? tab->query_result_col_widths[tab->query_result_scroll_col]
                  : 15;
          if (x + w + 3 > avail_width)
            break;
          x += w + 1;
          if (tab->query_result_scroll_col == tab->query_result_col)
            break;
          tab->query_result_scroll_col--;
        }
      }
      return true;
    }

    /* Page Up */
    if (hotkey_matches(cfg, event, HOTKEY_PAGE_UP)) {
      /* Calculate visible rows in results area using actual main window */
      int ppage_rows, ppage_cols;
      getmaxyx(state->main_win, ppage_rows, ppage_cols);
      (void)ppage_cols;
      int editor_height = (ppage_rows - 1) * 3 / 10;
      if (editor_height < 3)
        editor_height = 3;
      int visible =
          ppage_rows - editor_height - 4; /* results area minus headers */
      if (visible < 1)
        visible = 1;

      if (tab->query_result_row > (size_t)visible) {
        tab->query_result_row -= visible;
      } else {
        tab->query_result_row = 0;
      }
      /* Adjust scroll to keep cursor visible */
      if (tab->query_result_row < tab->query_result_scroll_row) {
        tab->query_result_scroll_row = tab->query_result_row;
      }
      query_check_load_more(state, tab);
      return true;
    }

    /* Page Down */
    if (hotkey_matches(cfg, event, HOTKEY_PAGE_DOWN)) {
      /* Calculate visible rows in results area using actual main window */
      int npage_rows, npage_cols;
      getmaxyx(state->main_win, npage_rows, npage_cols);
      (void)npage_cols;
      int editor_height = (npage_rows - 1) * 3 / 10;
      if (editor_height < 3)
        editor_height = 3;
      int visible = npage_rows - editor_height - 4;
      if (visible < 1)
        visible = 1;

      tab->query_result_row += visible;
      if (tab->query_result_row >= tab->query_results->num_rows) {
        tab->query_result_row = tab->query_results->num_rows > 0
                                    ? tab->query_results->num_rows - 1
                                    : 0;
      }
      /* Adjust scroll to keep cursor visible */
      if (tab->query_result_row >=
          tab->query_result_scroll_row + (size_t)visible) {
        tab->query_result_scroll_row = tab->query_result_row - visible + 1;
      }
      query_check_load_more(state, tab);
      return true;
    }

    /* g or a - go to first row */
    if (hotkey_matches(cfg, event, HOTKEY_FIRST_ROW)) {
      tab->query_result_row = 0;
      tab->query_result_scroll_row = 0;
      query_check_load_more(state, tab);
      return true;
    }

    /* G or z - go to last row */
    if (hotkey_matches(cfg, event, HOTKEY_LAST_ROW)) {
      if (tab->query_results->num_rows > 0) {
        tab->query_result_row = tab->query_results->num_rows - 1;
      }
      query_check_load_more(state, tab);
      return true;
    }

    return false;
  }

  /* Handle editor input */

  /* Ctrl+R - run query under cursor */
  if (hotkey_matches(cfg, event, HOTKEY_EXECUTE_QUERY)) {
    char *query = query_find_at_cursor(tab->query_text, tab->query_cursor);
    if (query && *query) {
      query_execute(state, query);
    } else {
      tui_set_error(state, "No query at cursor");
    }
    free(query);
    return true;
  }

  /* Ctrl+A - run all queries */
  if (hotkey_matches(cfg, event, HOTKEY_EXECUTE_ALL)) {
    if (!tab->query_text || !*tab->query_text) {
      tui_set_error(state, "No queries to execute");
      return true;
    }

    /* Execute all queries separated by semicolons */
    char *text = str_dup(tab->query_text);
    char *p = text;
    int count = 0;
    int errors = 0;

    while (p && *p) {
      /* Skip whitespace */
      while (*p && isspace((unsigned char)*p))
        p++;
      if (!*p)
        break;

      /* Find end of query */
      char *end = p;
      bool in_string = false;
      char quote_char = 0;
      while (*end) {
        if (in_string) {
          if (*end == quote_char && (end == p || *(end - 1) != '\\')) {
            in_string = false;
          }
        } else {
          if (*end == '\'' || *end == '"') {
            in_string = true;
            quote_char = *end;
          } else if (*end == ';') {
            break;
          }
        }
        end++;
      }

      /* Extract query */
      char saved = *end;
      *end = '\0';

      /* Trim */
      char *q = p;
      while (*q && isspace((unsigned char)*q))
        q++;
      size_t qlen = strlen(q);
      while (qlen > 0 && isspace((unsigned char)q[qlen - 1]))
        qlen--;

      if (qlen > 0) {
        char *query = safe_malloc(qlen + 1);
        memcpy(query, q, qlen);
        query[qlen] = '\0';
        query_execute(state, query);
        count++;
        if (tab->query_error) {
          errors++;
        }
        free(query);
      }

      *end = saved;
      p = *end ? end + 1 : end;
    }

    free(text);

    if (errors > 0) {
      tui_set_error(state, "Executed %d queries, %d errors", count, errors);
    } else {
      tui_set_status(state, "Executed %d queries", count);
    }
    return true;
  }

  /* Ctrl+T - run all queries in a transaction */
  if (hotkey_matches(cfg, event, HOTKEY_EXECUTE_TRANSACTION)) {
    if (!tab->query_text || !*tab->query_text) {
      tui_set_error(state, "No queries to execute");
      return true;
    }

    if (!state->conn) {
      tui_set_error(state, "Not connected to database");
      return true;
    }

    /* Start transaction */
    char *err = NULL;
    db_exec(state->conn, "BEGIN", &err);
    if (err) {
      tui_set_error(state, "Failed to start transaction: %s", err);
      free(err);
      return true;
    }

    /* Execute all queries separated by semicolons */
    char *text = str_dup(tab->query_text);
    char *p = text;
    int count = 0;
    bool had_error = false;

    while (p && *p && !had_error) {
      /* Skip whitespace */
      while (*p && isspace((unsigned char)*p))
        p++;
      if (!*p)
        break;

      /* Find end of query */
      char *end = p;
      bool in_string = false;
      char quote_char = 0;
      while (*end) {
        if (in_string) {
          if (*end == quote_char && (end == p || *(end - 1) != '\\')) {
            in_string = false;
          }
        } else {
          if (*end == '\'' || *end == '"') {
            in_string = true;
            quote_char = *end;
          } else if (*end == ';') {
            break;
          }
        }
        end++;
      }

      /* Extract query */
      char saved = *end;
      *end = '\0';

      /* Trim */
      char *q = p;
      while (*q && isspace((unsigned char)*q))
        q++;
      size_t qlen = strlen(q);
      while (qlen > 0 && isspace((unsigned char)q[qlen - 1]))
        qlen--;

      if (qlen > 0) {
        char *query = safe_malloc(qlen + 1);
        memcpy(query, q, qlen);
        query[qlen] = '\0';
        query_execute(state, query);
        count++;
        if (tab->query_error) {
          had_error = true;
        }
        free(query);
      }

      *end = saved;
      p = *end ? end + 1 : end;
    }

    free(text);

    /* Commit or rollback */
    err = NULL;
    if (had_error) {
      db_exec(state->conn, "ROLLBACK", &err);
      free(err);
      tui_set_error(state, "Transaction rolled back after error in query %d",
                    count);
    } else {
      db_exec(state->conn, "COMMIT", &err);
      if (err) {
        tui_set_error(state, "Commit failed: %s", err);
        free(err);
      } else {
        tui_set_status(state, "Transaction committed (%d queries)", count);
      }
    }
    return true;
  }

  /* Up arrow - move cursor up one line */
  if (render_event_is_special(event, UI_KEY_UP)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    if (line > 0) {
      tab->query_cursor =
          query_line_col_to_cursor(tab, line - 1, col, lines, num_lines);
    }

    free(lines);
    return true;
  }

  /* Down arrow - move cursor down one line */
  if (render_event_is_special(event, UI_KEY_DOWN)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    if (line < num_lines - 1) {
      tab->query_cursor =
          query_line_col_to_cursor(tab, line + 1, col, lines, num_lines);
    } else if (tab->query_results && tab->query_results->num_rows > 0) {
      /* On last line - move focus to results panel */
      ui->query_focus_results = true;
    }

    free(lines);
    return true;
  }

  /* Left arrow */
  if (render_event_is_special(event, UI_KEY_LEFT)) {
    if (tab->query_cursor > 0) {
      tab->query_cursor--;
    } else if (state->sidebar_visible) {
      /* At top-left position - move focus to sidebar */
      state->sidebar_focused = true;
      /* Restore last sidebar position */
      state->sidebar_highlight = state->sidebar_last_position;
    }
    return true;
  }

  /* Right arrow */
  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    if (tab->query_cursor < tab->query_len) {
      tab->query_cursor++;
    } else if (tab->query_results && tab->query_results->num_rows > 0) {
      /* At end of text - move focus to results panel */
      ui->query_focus_results = true;
    }
    return true;
  }

  /* Home - move to start of line */
  if (render_event_is_special(event, UI_KEY_HOME)) {
    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    if (lines && line < num_lines) {
      tab->query_cursor = lines[line].start;
    }
    free(lines);
    return true;
  }

  /* End or Ctrl+E - move to end of line */
  if (render_event_is_special(event, UI_KEY_END) ||
      render_event_is_ctrl(event, 'E')) {
    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    if (lines && line < num_lines) {
      tab->query_cursor = lines[line].start + lines[line].len;
    }
    free(lines);
    return true;
  }

  /* Page Up */
  if (render_event_is_special(event, UI_KEY_PAGEUP)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    if (line > 10) {
      tab->query_cursor =
          query_line_col_to_cursor(tab, line - 10, col, lines, num_lines);
    } else {
      tab->query_cursor =
          query_line_col_to_cursor(tab, 0, col, lines, num_lines);
    }

    free(lines);
    return true;
  }

  /* Page Down */
  if (render_event_is_special(event, UI_KEY_PAGEDOWN)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    tab->query_cursor =
        query_line_col_to_cursor(tab, line + 10, col, lines, num_lines);

    free(lines);
    return true;
  }

  /* Backspace */
  if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
    query_backspace(tab);
    return true;
  }

  /* Delete */
  if (render_event_is_special(event, UI_KEY_DELETE)) {
    query_delete_char(tab);
    return true;
  }

  /* Enter - insert newline */
  if (render_event_is_special(event, UI_KEY_ENTER)) {
    query_insert_char(tab, '\n');
    return true;
  }

  /* Ctrl+K - cut full line (including newline) to buffer.
   * Consecutive cuts append to buffer so multiple lines can be cut together. */
  if (hotkey_matches(cfg, event, HOTKEY_CUT_LINE)) {
    /* Find start of current line */
    size_t start = tab->query_cursor;
    while (start > 0 && tab->query_text[start - 1] != '\n') {
      start--;
    }

    /* Find end of current line (including newline if present) */
    size_t end = tab->query_cursor;
    while (end < tab->query_len && tab->query_text[end] != '\n') {
      end++;
    }
    /* Include the newline character if present */
    if (end < tab->query_len && tab->query_text[end] == '\n') {
      end++;
    }

    size_t count = end - start;
    if (count > 0) {
      /* Check if this is a consecutive cut (append) or new cut (replace).
       * Consecutive = cursor at start of line (where last cut left it).
       * Any other action (typing, navigation, paste) moves cursor. */
      static size_t last_cut_cursor = SIZE_MAX;
      bool is_consecutive =
          (last_cut_cursor == start) && (state->clipboard_buffer != NULL);

      if (is_consecutive) {
        /* Append to existing buffer */
        size_t old_len = strlen(state->clipboard_buffer);
        /* Check for overflow before allocation */
        if (old_len > SIZE_MAX - count - 1) {
          /* Overflow - skip append */
        } else {
          state->clipboard_buffer =
              safe_realloc(state->clipboard_buffer, old_len + count + 1);
          memcpy(state->clipboard_buffer + old_len, tab->query_text + start, count);
          state->clipboard_buffer[old_len + count] = '\0';
        }
      } else {
        /* New cut - replace buffer, add newline if line doesn't end with one */
        free(state->clipboard_buffer);
        bool needs_newline = (tab->query_text[end - 1] != '\n');
        /* Check for overflow before allocation */
        size_t alloc_size = count + (needs_newline ? 1 : 0) + 1;
        if (count > SIZE_MAX - 2) {
          state->clipboard_buffer = NULL; /* Overflow */
        } else {
          state->clipboard_buffer = safe_malloc(alloc_size);
          memcpy(state->clipboard_buffer, tab->query_text + start, count);
          if (needs_newline) {
            state->clipboard_buffer[count] = '\n';
            state->clipboard_buffer[count + 1] = '\0';
          } else {
            state->clipboard_buffer[count] = '\0';
          }
        }
      }

      /* Copy to OS clipboard */
      if (state->clipboard_buffer) {
#ifdef __APPLE__
        FILE *p = popen("pbcopy", "w");
#else
        const char *cmd = getenv("WAYLAND_DISPLAY")
                              ? "wl-copy"
                              : "xclip -selection clipboard";
        FILE *p = popen(cmd, "w");
#endif
        if (p) {
          fwrite(state->clipboard_buffer, 1, strlen(state->clipboard_buffer),
                 p);
          pclose(p);
        }
      }

      /* Delete the text and move cursor to line start */
      memmove(tab->query_text + start, tab->query_text + end,
              tab->query_len - end + 1);
      tab->query_len -= count;
      tab->query_cursor = start;

      /* Track cursor position for consecutive cut detection. */
      last_cut_cursor = start;
    }
    return true;
  }

  /* Ctrl+U - paste from system clipboard first, then internal buffer if OS
   * inaccessible */
  if (hotkey_matches(cfg, event, HOTKEY_PASTE)) {
    char *paste_text = NULL;
    bool os_clipboard_accessible = false;

    /* Try to read from system clipboard first */
#ifdef __APPLE__
    FILE *p = popen("pbpaste", "r");
#else
    const char *cmd = getenv("WAYLAND_DISPLAY")
                          ? "wl-paste -n 2>/dev/null"
                          : "xclip -selection clipboard -o 2>/dev/null";
    FILE *p = popen(cmd, "r");
#endif
    if (p) {
      /* Read clipboard content */
      size_t capacity = 4096;
      size_t len = 0;
      paste_text = safe_malloc(capacity);
      int c;
      while ((c = fgetc(p)) != EOF) {
        if (len + 1 >= capacity) {
          /* Check for overflow before doubling */
          if (capacity > SIZE_MAX / 2)
            break;
          capacity *= 2;
          paste_text = safe_realloc(paste_text, capacity);
        }
        paste_text[len++] = (char)c;
      }
      paste_text[len] = '\0';
      int status = pclose(p);
      /* OS clipboard is accessible if command succeeded (status 0) */
      os_clipboard_accessible = (status == 0);
      if (!os_clipboard_accessible || (paste_text && paste_text[0] == '\0')) {
        free(paste_text);
        paste_text = NULL;
      }
    }

    /* Only fall back to internal buffer if OS clipboard is inaccessible */
    if (!os_clipboard_accessible && state->clipboard_buffer) {
      paste_text = strdup(state->clipboard_buffer);
    }

    /* Perform the paste */
    if (paste_text && paste_text[0]) {
      size_t paste_len = strlen(paste_text);
      if (query_ensure_capacity(tab, tab->query_len + paste_len + 1)) {
        /* Make room for pasted text */
        memmove(tab->query_text + tab->query_cursor + paste_len,
                tab->query_text + tab->query_cursor,
                tab->query_len - tab->query_cursor + 1);
        /* Insert pasted text */
        memcpy(tab->query_text + tab->query_cursor, paste_text, paste_len);
        tab->query_len += paste_len;
        tab->query_cursor += paste_len;
      }
    }
    free(paste_text);
    return true;
  }

  /* Printable character - insert */
  if (render_event_is_char(event) && key_char >= 32 && key_char < 127) {
    query_insert_char(tab, (char)key_char);
    return true;
  }

  return false;
}
