/*
 * Lace
 * Cursor and page navigation
 *
 * Uses VmTable for cursor/scroll state access.
 * TUI-specific logic remains for pagination loading and scroll adjustment.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "tui_internal.h"
#include "../../viewmodel/vm_table.h"

/* Helper to get VmTable, returns NULL if not valid for table navigation */
static VmTable *get_vm_table(TuiState *state) {
  if (!state || !state->vm_table)
    return NULL;
  if (!vm_table_valid(state->vm_table))
    return NULL;
  return state->vm_table;
}

void tui_move_cursor(TuiState *state, int row_delta, int col_delta) {
  VmTable *vm = get_vm_table(state);
  if (!vm)
    return;

  size_t cursor_row, cursor_col;
  vm_table_get_cursor(vm, &cursor_row, &cursor_col);
  size_t loaded_rows = vm_table_row_count(vm);
  size_t num_cols = vm_table_col_count(vm);
  size_t total_rows = vm_table_total_rows(vm);
  size_t loaded_offset = vm_table_loaded_offset(vm);
  size_t loaded_count = vm_table_loaded_count(vm);

  /* Update row */
  if (row_delta < 0 && cursor_row > 0) {
    cursor_row--;
  } else if (row_delta > 0 && loaded_rows > 0 &&
             cursor_row < loaded_rows - 1) {
    cursor_row++;
  } else if (row_delta > 0 && loaded_rows > 0 &&
             cursor_row == loaded_rows - 1) {
    /* At last loaded row, check if more data exists */
    size_t loaded_end = loaded_offset + loaded_count;
    if (loaded_end < total_rows) {
      /* Load more data with blocking dialog */
      if (tui_load_page_with_dialog(state, true)) {
        /* Data loaded, move cursor */
        cursor_row++;
      }
    }
  } else if (row_delta < 0 && cursor_row == 0 && loaded_offset > 0) {
    /* At first loaded row but not at beginning of data */
    if (tui_load_page_with_dialog(state, false)) {
      /* Data prepended, cursor_row was adjusted by merge */
      cursor_row--;
    }
  }

  /* Update column */
  if (col_delta < 0 && cursor_col > 0) {
    cursor_col--;
  } else if (col_delta > 0 && num_cols > 0 && cursor_col < num_cols - 1) {
    cursor_col++;
  }

  /* Update cursor via viewmodel */
  vm_table_set_cursor(vm, cursor_row, cursor_col);

  /* Get actual main window dimensions (TUI-specific) */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);

  /* Account for filters panel if visible */
  int filters_height =
      state->filters_visible ? tui_get_filters_panel_height(state) : 0;

  /* Visible rows = main window height - 3 header rows - filters panel */
  int visible_rows = win_rows - 3 - filters_height;
  if (visible_rows < 1)
    visible_rows = 1;

  /* Get current scroll position from viewmodel */
  size_t scroll_row, scroll_col;
  vm_table_get_scroll(vm, &scroll_row, &scroll_col);

  /* Adjust scroll to keep cursor visible */
  if (cursor_row < scroll_row) {
    scroll_row = cursor_row;
  } else if (cursor_row >= scroll_row + (size_t)visible_rows) {
    scroll_row = cursor_row - visible_rows + 1;
  }

  /* Calculate visible columns using actual window width */
  int x = 1;
  size_t first_visible_col = scroll_col;
  size_t last_visible_col = scroll_col;

  for (size_t col = scroll_col; col < num_cols; col++) {
    int width = tui_get_column_width(state, col);
    if (x + width + 3 > win_cols)
      break;
    x += width + 1;
    last_visible_col = col;
  }

  if (cursor_col < first_visible_col) {
    scroll_col = cursor_col;
  } else if (cursor_col > last_visible_col) {
    /* Scroll right */
    scroll_col = cursor_col;
    /* Adjust to show as many columns as possible */
    x = 1;
    while (scroll_col > 0) {
      int width = tui_get_column_width(state, scroll_col);
      if (x + width + 3 > win_cols)
        break;
      x += width + 1;
      if (scroll_col == cursor_col)
        break;
      scroll_col--;
    }
  }

  /* Update scroll via viewmodel */
  vm_table_set_scroll(vm, scroll_row, scroll_col);

  /* Sync to compatibility layer (temporary - will be removed) */
  state->cursor_row = cursor_row;
  state->cursor_col = cursor_col;
  state->scroll_row = scroll_row;
  state->scroll_col = scroll_col;

  /* Check if we need to load more rows */
  tui_check_load_more(state);
}

void tui_page_up(TuiState *state) {
  VmTable *vm = get_vm_table(state);
  if (!vm || !state->main_win)
    return;

  /* Get actual main window height (TUI-specific) */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);
  (void)win_cols;

  /* Account for filters panel if visible */
  int filters_height =
      state->filters_visible ? tui_get_filters_panel_height(state) : 0;

  int page_size = win_rows - 3 - filters_height;
  if (page_size < 1)
    page_size = 1;

  size_t cursor_row, cursor_col;
  vm_table_get_cursor(vm, &cursor_row, &cursor_col);
  size_t scroll_row, scroll_col;
  vm_table_get_scroll(vm, &scroll_row, &scroll_col);
  size_t loaded_offset = vm_table_loaded_offset(vm);

  /* Check if we're at the beginning of loaded data but not at start of table */
  if (cursor_row < (size_t)page_size && loaded_offset > 0) {
    /* Need to load previous data - show blocking dialog */
    tui_load_page_with_dialog(state, false);
    /* After prepend, cursor_row was adjusted - re-read */
    vm_table_get_cursor(vm, &cursor_row, &cursor_col);
    vm_table_get_scroll(vm, &scroll_row, &scroll_col);
  }

  if (cursor_row > (size_t)page_size) {
    cursor_row -= page_size;
  } else {
    cursor_row = 0;
  }

  if (scroll_row > (size_t)page_size) {
    scroll_row -= page_size;
  } else {
    scroll_row = 0;
  }

  /* Ensure cursor remains visible after scroll adjustment */
  if (cursor_row < scroll_row) {
    scroll_row = cursor_row;
  } else if (cursor_row >= scroll_row + (size_t)page_size) {
    scroll_row = cursor_row - page_size + 1;
  }

  /* Update via viewmodel */
  vm_table_set_cursor(vm, cursor_row, cursor_col);
  vm_table_set_scroll(vm, scroll_row, scroll_col);

  /* Sync to compatibility layer (temporary) */
  state->cursor_row = cursor_row;
  state->scroll_row = scroll_row;

  /* Check if we need to load previous rows (for speculative loading) */
  tui_check_load_more(state);
}

void tui_page_down(TuiState *state) {
  VmTable *vm = get_vm_table(state);
  if (!vm || !state->main_win)
    return;

  /* Get actual main window height (TUI-specific) */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);
  (void)win_cols;

  /* Account for filters panel if visible */
  int filters_height =
      state->filters_visible ? tui_get_filters_panel_height(state) : 0;

  int page_size = win_rows - 3 - filters_height;
  if (page_size < 1)
    page_size = 1;

  size_t cursor_row, cursor_col;
  vm_table_get_cursor(vm, &cursor_row, &cursor_col);
  size_t scroll_row, scroll_col;
  vm_table_get_scroll(vm, &scroll_row, &scroll_col);
  size_t loaded_rows = vm_table_row_count(vm);
  size_t total_rows = vm_table_total_rows(vm);
  size_t loaded_offset = vm_table_loaded_offset(vm);
  size_t loaded_count = vm_table_loaded_count(vm);

  size_t target_row = cursor_row + page_size;

  /* Check if target row is beyond loaded data but more exists */
  if (target_row >= loaded_rows) {
    size_t loaded_end = loaded_offset + loaded_count;
    if (loaded_end < total_rows) {
      /* Need to load more data - show blocking dialog */
      tui_load_page_with_dialog(state, true);
      /* Re-read loaded row count after loading */
      loaded_rows = vm_table_row_count(vm);
    }
    /* Clamp to available data */
    target_row = loaded_rows > 0 ? loaded_rows - 1 : 0;
  }

  cursor_row = target_row;

  scroll_row += page_size;
  size_t max_scroll =
      loaded_rows > (size_t)page_size ? loaded_rows - page_size : 0;
  if (scroll_row > max_scroll) {
    scroll_row = max_scroll;
  }

  /* Ensure cursor remains visible after scroll adjustment */
  if (cursor_row < scroll_row) {
    scroll_row = cursor_row;
  } else if (cursor_row >= scroll_row + (size_t)page_size) {
    scroll_row = cursor_row - page_size + 1;
  }

  /* Update via viewmodel */
  vm_table_set_cursor(vm, cursor_row, cursor_col);
  vm_table_set_scroll(vm, scroll_row, scroll_col);

  /* Sync to compatibility layer (temporary) */
  state->cursor_row = cursor_row;
  state->scroll_row = scroll_row;

  /* Check if we need to load more rows (for speculative loading) */
  tui_check_load_more(state);
}

void tui_home(TuiState *state) {
  VmTable *vm = get_vm_table(state);
  if (!vm)
    return;

  /* If we're not at the beginning, load the first page with dialog */
  size_t loaded_offset = vm_table_loaded_offset(vm);
  if (loaded_offset > 0) {
    if (!tui_load_rows_at_with_dialog(state, 0)) {
      return; /* Cancelled or failed */
    }
  }

  /* Update via viewmodel */
  vm_table_set_cursor(vm, 0, 0);
  vm_table_set_scroll(vm, 0, 0);

  /* Sync to compatibility layer (temporary) */
  state->cursor_row = 0;
  state->cursor_col = 0;
  state->scroll_row = 0;
  state->scroll_col = 0;
}

void tui_end(TuiState *state) {
  VmTable *vm = get_vm_table(state);
  if (!vm || !state->main_win)
    return;

  size_t total_rows = vm_table_total_rows(vm);
  size_t loaded_offset = vm_table_loaded_offset(vm);
  size_t loaded_count = vm_table_loaded_count(vm);

  /* If we haven't loaded all rows, load the last page with dialog */
  size_t loaded_end = loaded_offset + loaded_count;
  if (loaded_end < total_rows) {
    /* Load last page of data */
    size_t last_page_offset =
        total_rows > PAGE_SIZE ? total_rows - PAGE_SIZE : 0;
    if (!tui_load_rows_at_with_dialog(state, last_page_offset)) {
      return; /* Cancelled or failed */
    }
  }

  size_t loaded_rows = vm_table_row_count(vm);
  size_t cursor_row = loaded_rows > 0 ? loaded_rows - 1 : 0;

  /* Get actual main window height (TUI-specific) */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);
  (void)win_cols;

  /* Account for filters panel if visible */
  int filters_height =
      state->filters_visible ? tui_get_filters_panel_height(state) : 0;

  int visible_rows = win_rows - 3 - filters_height;
  if (visible_rows < 1)
    visible_rows = 1;

  size_t scroll_row =
      loaded_rows > (size_t)visible_rows ? loaded_rows - visible_rows : 0;

  /* Get current column */
  size_t cursor_col;
  vm_table_get_cursor(vm, NULL, &cursor_col);

  /* Update via viewmodel */
  vm_table_set_cursor(vm, cursor_row, cursor_col);
  vm_table_set_scroll(vm, scroll_row, 0);

  /* Sync to compatibility layer (temporary) */
  state->cursor_row = cursor_row;
  state->scroll_row = scroll_row;
}

void tui_column_first(TuiState *state) {
  VmTable *vm = get_vm_table(state);
  if (!vm)
    return;

  size_t cursor_row;
  vm_table_get_cursor(vm, &cursor_row, NULL);

  /* Update via viewmodel */
  vm_table_set_cursor(vm, cursor_row, 0);
  vm_table_set_scroll(vm, state->scroll_row, 0);

  /* Sync to compatibility layer (temporary) */
  state->cursor_col = 0;
  state->scroll_col = 0;
}

void tui_column_last(TuiState *state) {
  VmTable *vm = get_vm_table(state);
  if (!vm)
    return;

  size_t num_cols = vm_table_col_count(vm);
  size_t cursor_col = num_cols > 0 ? num_cols - 1 : 0;

  size_t cursor_row;
  vm_table_get_cursor(vm, &cursor_row, NULL);
  size_t scroll_row, scroll_col;
  vm_table_get_scroll(vm, &scroll_row, &scroll_col);

  /* Check if cursor is already visible - no scroll needed (TUI-specific) */
  if (!state->main_win) {
    vm_table_set_cursor(vm, cursor_row, cursor_col);
    state->cursor_col = cursor_col;
    return;
  }

  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);
  (void)win_rows;

  /* Calculate last visible column with current scroll */
  int x = 1;
  size_t last_visible_col = scroll_col;
  for (size_t col = scroll_col; col < num_cols; col++) {
    int width = tui_get_column_width(state, col);
    if (x + width + 3 > win_cols)
      break;
    x += width + 1;
    last_visible_col = col;
  }

  /* Only scroll if cursor is not visible */
  if (cursor_col > last_visible_col) {
    scroll_col = cursor_col;
    x = 1;
    while (scroll_col > 0) {
      int width = tui_get_column_width(state, scroll_col);
      if (x + width + 3 > win_cols)
        break;
      x += width + 1;
      if (scroll_col == cursor_col)
        break;
      scroll_col--;
    }
  }

  /* Update via viewmodel */
  vm_table_set_cursor(vm, cursor_row, cursor_col);
  vm_table_set_scroll(vm, scroll_row, scroll_col);

  /* Sync to compatibility layer (temporary) */
  state->cursor_col = cursor_col;
  state->scroll_col = scroll_col;
}

void tui_next_table(TuiState *state) {
  if (!state || !state->tables || state->num_tables == 0)
    return;

  state->current_table++;
  if (state->current_table >= state->num_tables) {
    state->current_table = 0;
  }

  /* Clear filters when switching tables */
  Tab *tab = TUI_TAB(state);
  if (tab && tab->type == TAB_TYPE_TABLE) {
    filters_clear(&tab->filters);
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

  /* Clear filters when switching tables */
  Tab *tab = TUI_TAB(state);
  if (tab && tab->type == TAB_TYPE_TABLE) {
    filters_clear(&tab->filters);
  }

  tui_load_table_data(state, state->tables[state->current_table]);
}
