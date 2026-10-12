/*
 * Lace
 * Query tab drawing: the editor / results split and the screen
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include "query_internal.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int query_editor_height(int win_rows) {
  int h = (win_rows - 1) * 3 / 10; /* 30% for the editor, -1 for separator */
  return h < 3 ? 3 : h;
}

int query_visible_result_rows(TuiState *state) {
  int rows = getmaxy(state->main_win);
  int visible =
      rows - query_editor_height(rows) - 1 - QUERY_RESULTS_HEADER_ROWS;
  return visible < 1 ? 1 : visible;
}

/* Draw the query tab */
void tui_draw_query(TuiState *state) {
  if (!state || !state->main_win)
    return;

  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || tab->type != TAB_TYPE_QUERY || !ui)
    return;

  size_t query_cursor = tab->query_cursor;
  size_t query_scroll_line = tab->query_scroll_line;
  bool focus_results = ui->query_focus_results;
  bool focus_editor = !ui->query_unfocused && !focus_results;

  werase(state->main_win);

  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);

  /* Split view: editor on top, results on bottom */
  int editor_height = query_editor_height(win_rows);
  int results_start = editor_height + 1;

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
  /* Sync scroll back to tab */
  tab->query_scroll_line = query_scroll_line;

  /* Find current query bounds for highlighting */
  size_t query_start = 0, query_end = 0;
  bool has_query_bounds =
      query_find_bounds_at_cursor(tab->query_text, query_cursor,
                                  db_conn_driver_name(TUI_CONN(state)),
                                  &query_start, &query_end);

  /* Draw editor area */
  if (focus_editor) {
    wattron(state->main_win, A_BOLD);
  }
  mvwprintw(state->main_win, 0, 1, "%s",
            ui->query_unfocused
                ? "SQL Query (Enter/Tab: edit, ^R: run, Down: results)"
                : "SQL Query (^R: run, ^A: all, ^T: transaction, Esc: leave)");
  if (focus_editor) {
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
    const char *ltext = tab->query_text + li->start;
    for (size_t i = 0; i < li->len && x < win_cols - 1;) {
      size_t next = utf8_next(ltext, li->len, i);
      unsigned char c = (unsigned char)ltext[i];
      if (c == '\t') {
        /* Tab as spaces */
        for (int t = 0; t < 4 && x < win_cols - 1; t++) {
          mvwaddch(state->main_win, y, x++, ' ');
        }
      } else if (c >= 32 && c != 127) {
        int w = utf8_width(ltext + i, next - i);
        if (x + w > win_cols - 1)
          break;
        mvwaddnstr(state->main_win, y, x, ltext + i, (int)(next - i));
        x += w;
      }
      i = next;
    }

    if (is_dimmed) {
      wattroff(state->main_win, A_DIM);
    }

    /* Draw cursor if on this line and editor focused */
    if (focus_editor && line_idx == cursor_line) {
      /* Tabs are drawn 4 columns wide */
      const char *ltext = tab->query_text + li->start;
      size_t ccol = cursor_col < li->len ? cursor_col : li->len;
      int cursor_x = 4 + (int)query_display_width(ltext, ccol);
      if (cursor_x < win_cols) {
        wattron(state->main_win, A_REVERSE);
        if (ccol < li->len && (unsigned char)ltext[ccol] >= 32 &&
            ltext[ccol] != 127)
          mvwaddnstr(state->main_win, y, cursor_x, ltext + ccol,
                     (int)(utf8_next(ltext, li->len, ccol) - ccol));
        else
          mvwaddch(state->main_win, y, cursor_x, ' ');
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
                             .is_focused = ui->query_focus_results &&
                                           !ui->query_unfocused,
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
