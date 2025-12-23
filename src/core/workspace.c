/*
 * lace - Database Viewer and Manager
 * Core Tab Navigation Implementation
 *
 * Note: Lifecycle functions (tab_init, tab_free_data, etc.) are in app_state.c
 * This file contains navigation and pagination operations.
 */

#include "workspace.h"
#include <stdlib.h>

/* ============================================================================
 * Tab Navigation Operations
 * ============================================================================
 */

bool tab_move_cursor(Tab *tab, int row_delta, int col_delta, int visible_rows) {
  if (!tab || !tab->data)
    return false;

  bool moved = false;
  size_t old_row = tab->cursor_row;
  size_t old_col = tab->cursor_col;

  /* Update row */
  if (row_delta < 0 && tab->cursor_row > 0) {
    tab->cursor_row--;
    moved = true;
  } else if (row_delta > 0 && tab->data->num_rows > 0 &&
             tab->cursor_row < tab->data->num_rows - 1) {
    tab->cursor_row++;
    moved = true;
  }

  /* Update column */
  if (col_delta < 0 && tab->cursor_col > 0) {
    tab->cursor_col--;
    moved = true;
  } else if (col_delta > 0 && tab->cursor_col < tab->data->num_columns - 1) {
    tab->cursor_col++;
    moved = true;
  }

  /* Adjust vertical scroll */
  if (visible_rows < 1)
    visible_rows = 1;

  if (tab->cursor_row < tab->scroll_row) {
    tab->scroll_row = tab->cursor_row;
  } else if (tab->cursor_row >= tab->scroll_row + (size_t)visible_rows) {
    tab->scroll_row = tab->cursor_row - visible_rows + 1;
  }

  return moved || (old_row != tab->cursor_row) || (old_col != tab->cursor_col);
}

void tab_page_up(Tab *tab, int visible_rows) {
  if (!tab || !tab->data)
    return;

  if (visible_rows < 1)
    visible_rows = 1;

  if (tab->cursor_row > (size_t)visible_rows) {
    tab->cursor_row -= visible_rows;
  } else {
    tab->cursor_row = 0;
  }

  if (tab->scroll_row > (size_t)visible_rows) {
    tab->scroll_row -= visible_rows;
  } else {
    tab->scroll_row = 0;
  }

  /* Ensure cursor visible */
  if (tab->cursor_row < tab->scroll_row) {
    tab->scroll_row = tab->cursor_row;
  } else if (tab->cursor_row >= tab->scroll_row + (size_t)visible_rows) {
    tab->scroll_row = tab->cursor_row - visible_rows + 1;
  }
}

void tab_page_down(Tab *tab, int visible_rows) {
  if (!tab || !tab->data)
    return;

  if (visible_rows < 1)
    visible_rows = 1;

  size_t target_row = tab->cursor_row + visible_rows;
  if (target_row >= tab->data->num_rows) {
    target_row = tab->data->num_rows > 0 ? tab->data->num_rows - 1 : 0;
  }

  tab->cursor_row = target_row;
  tab->scroll_row += visible_rows;

  size_t max_scroll = tab->data->num_rows > (size_t)visible_rows
                          ? tab->data->num_rows - visible_rows
                          : 0;
  if (tab->scroll_row > max_scroll) {
    tab->scroll_row = max_scroll;
  }

  /* Ensure cursor visible */
  if (tab->cursor_row < tab->scroll_row) {
    tab->scroll_row = tab->cursor_row;
  } else if (tab->cursor_row >= tab->scroll_row + (size_t)visible_rows) {
    tab->scroll_row = tab->cursor_row - visible_rows + 1;
  }
}

void tab_home(Tab *tab) {
  if (!tab)
    return;
  tab->cursor_row = 0;
  tab->cursor_col = 0;
  tab->scroll_row = 0;
  tab->scroll_col = 0;
}

void tab_end(Tab *tab, int visible_rows) {
  if (!tab || !tab->data)
    return;

  if (visible_rows < 1)
    visible_rows = 1;

  tab->cursor_row = tab->data->num_rows > 0 ? tab->data->num_rows - 1 : 0;

  tab->scroll_row = tab->data->num_rows > (size_t)visible_rows
                        ? tab->data->num_rows - visible_rows
                        : 0;
}

void tab_column_first(Tab *tab) {
  if (!tab)
    return;
  tab->cursor_col = 0;
  tab->scroll_col = 0;
}

void tab_column_last(Tab *tab) {
  if (!tab || !tab->data)
    return;
  tab->cursor_col =
      tab->data->num_columns > 0 ? tab->data->num_columns - 1 : 0;
}

/* ============================================================================
 * Tab Pagination State
 * ============================================================================
 */

int tab_check_data_edge(Tab *tab, size_t threshold) {
  if (!tab || !tab->data)
    return 0;

  size_t rows_from_start = tab->cursor_row;
  size_t rows_from_end = tab->data->num_rows > tab->cursor_row
                             ? tab->data->num_rows - tab->cursor_row
                             : 0;

  if (rows_from_start < threshold && tab->loaded_offset > 0) {
    return -1; /* Near start, more data available before */
  }

  if (rows_from_end < threshold) {
    size_t loaded_end = tab->loaded_offset + tab->loaded_count;
    if (loaded_end < tab->total_rows) {
      return 1; /* Near end, more data available after */
    }
  }

  return 0; /* Not near edge or no more data */
}

bool tab_has_more_data_forward(Tab *tab) {
  if (!tab)
    return false;
  size_t loaded_end = tab->loaded_offset + tab->loaded_count;
  return loaded_end < tab->total_rows;
}

bool tab_has_more_data_backward(Tab *tab) {
  if (!tab)
    return false;
  return tab->loaded_offset > 0;
}

void tab_update_pagination(Tab *tab, size_t loaded_offset, size_t loaded_count,
                           size_t total_rows) {
  if (!tab)
    return;
  tab->loaded_offset = loaded_offset;
  tab->loaded_count = loaded_count;
  tab->total_rows = total_rows;
}
