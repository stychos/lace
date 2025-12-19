/*
 * lace - Database Viewer and Manager
 * Cursor and page navigation
 */

#include "tui_internal.h"

void tui_move_cursor(TuiState *state, int row_delta, int col_delta) {
  if (!state || !state->data)
    return;

  /* Update row */
  if (row_delta < 0 && state->cursor_row > 0) {
    state->cursor_row--;
  } else if (row_delta > 0 && state->cursor_row < state->data->num_rows - 1) {
    state->cursor_row++;
  }

  /* Update column */
  if (col_delta < 0 && state->cursor_col > 0) {
    state->cursor_col--;
  } else if (col_delta > 0 &&
             state->cursor_col < state->data->num_columns - 1) {
    state->cursor_col++;
  }

  /* Adjust scroll */
  /* Visible rows = main window height - 3 header rows (border + headers +
   * separator) */
  int visible_rows = state->term_rows - 6;
  if (visible_rows < 1)
    visible_rows = 1;

  if (state->cursor_row < state->scroll_row) {
    state->scroll_row = state->cursor_row;
  } else if (state->cursor_row >= state->scroll_row + (size_t)visible_rows) {
    state->scroll_row = state->cursor_row - visible_rows + 1;
  }

  /* Calculate visible columns */
  int x = 1;
  size_t first_visible_col = state->scroll_col;
  size_t last_visible_col = state->scroll_col;

  for (size_t col = state->scroll_col; col < state->data->num_columns; col++) {
    int width = tui_get_column_width(state, col);
    if (x + width + 3 > state->term_cols)
      break;
    x += width + 1;
    last_visible_col = col;
  }

  if (state->cursor_col < first_visible_col) {
    state->scroll_col = state->cursor_col;
  } else if (state->cursor_col > last_visible_col) {
    /* Scroll right */
    state->scroll_col = state->cursor_col;
    /* Adjust to show as many columns as possible */
    x = 1;
    while (state->scroll_col > 0) {
      int width = tui_get_column_width(state, state->scroll_col);
      if (x + width + 3 > state->term_cols)
        break;
      x += width + 1;
      if (state->scroll_col == state->cursor_col)
        break;
      state->scroll_col--;
    }
  }

  /* Check if we need to load more rows */
  tui_check_load_more(state);
}

void tui_page_up(TuiState *state) {
  if (!state || !state->data)
    return;

  int page_size = state->term_rows - 6;
  if (page_size < 1)
    page_size = 1;

  if (state->cursor_row > (size_t)page_size) {
    state->cursor_row -= page_size;
  } else {
    state->cursor_row = 0;
  }

  if (state->scroll_row > (size_t)page_size) {
    state->scroll_row -= page_size;
  } else {
    state->scroll_row = 0;
  }

  /* Ensure cursor remains visible after scroll adjustment */
  if (state->cursor_row < state->scroll_row) {
    state->scroll_row = state->cursor_row;
  } else if (state->cursor_row >= state->scroll_row + (size_t)page_size) {
    state->scroll_row = state->cursor_row - page_size + 1;
  }

  /* Check if we need to load previous rows */
  tui_check_load_more(state);
}

void tui_page_down(TuiState *state) {
  if (!state || !state->data)
    return;

  int page_size = state->term_rows - 6;
  if (page_size < 1)
    page_size = 1;

  state->cursor_row += page_size;
  if (state->cursor_row >= state->data->num_rows) {
    state->cursor_row = state->data->num_rows - 1;
  }

  state->scroll_row += page_size;
  size_t max_scroll = state->data->num_rows > (size_t)page_size
                          ? state->data->num_rows - page_size
                          : 0;
  if (state->scroll_row > max_scroll) {
    state->scroll_row = max_scroll;
  }

  /* Ensure cursor remains visible after scroll adjustment */
  if (state->cursor_row < state->scroll_row) {
    state->scroll_row = state->cursor_row;
  } else if (state->cursor_row >= state->scroll_row + (size_t)page_size) {
    state->scroll_row = state->cursor_row - page_size + 1;
  }

  /* Check if we need to load more rows */
  tui_check_load_more(state);
}

void tui_home(TuiState *state) {
  if (!state)
    return;

  /* If we're not at the beginning, load the first page */
  if (state->loaded_offset > 0) {
    tui_load_rows_at(state, 0);
  }

  state->cursor_row = 0;
  state->cursor_col = 0;
  state->scroll_row = 0;
  state->scroll_col = 0;
}

void tui_end(TuiState *state) {
  if (!state || !state->data)
    return;

  /* If we haven't loaded all rows, load the last page */
  size_t loaded_end = state->loaded_offset + state->loaded_count;
  if (loaded_end < state->total_rows) {
    /* Load last page of data */
    size_t last_page_offset =
        state->total_rows > PAGE_SIZE ? state->total_rows - PAGE_SIZE : 0;
    tui_load_rows_at(state, last_page_offset);
  }

  state->cursor_row = state->data->num_rows > 0 ? state->data->num_rows - 1 : 0;
  state->cursor_col =
      state->data->num_columns > 0 ? state->data->num_columns - 1 : 0;

  int visible_rows = state->term_rows - 6;
  if (visible_rows < 1)
    visible_rows = 1;

  state->scroll_row = state->data->num_rows > (size_t)visible_rows
                          ? state->data->num_rows - visible_rows
                          : 0;
}

void tui_next_table(TuiState *state) {
  if (!state || !state->tables || state->num_tables == 0)
    return;

  state->current_table++;
  if (state->current_table >= state->num_tables) {
    state->current_table = 0;
  }

  tui_load_table_data(state, state->tables[state->current_table]);
}

void tui_prev_table(TuiState *state) {
  if (!state || !state->tables || state->num_tables == 0)
    return;

  if (state->current_table == 0) {
    state->current_table = state->num_tables - 1;
  } else {
    state->current_table--;
  }

  tui_load_table_data(state, state->tables[state->current_table]);
}
