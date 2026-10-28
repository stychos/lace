/*
 * Lace
 * Query tab results: column widths and the sliding window of loaded pages
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "query_internal.h"
#include "../../liblace/include/util/mem.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Calculate column widths for query results */
void query_calculate_result_widths(Tab *tab) {
  if (!tab->query_results || tab->query_results->num_columns == 0)
    return;

  size_t num_cols = tab->query_results->num_columns;

  free(tab->query_result_col_widths);
  tab->query_result_col_widths = safe_calloc(num_cols, sizeof(int));
  if (tab->num_user_widths > num_cols) /* another result's columns */
    tab_clear_user_widths(tab);
  tab->query_result_num_cols = num_cols;

  for (size_t c = 0; c < num_cols; c++)
    tab->query_result_col_widths[c] =
        grid_measure_column(tab->query_results, c, tab->query_source_schema, 0, 100);
  tab_apply_user_widths(tab, tab->query_result_col_widths, num_cols);
}

/* Load more rows at end of current query results */
bool query_load_more_rows(TuiState *state, Tab *tab) {
  if (!state || !TUI_CONN(state) || !tab || !tab->query_paginated ||
      !tab->query_base_sql || !tab->query_results)
    return false;

  size_t new_offset = tab->query_loaded_offset + tab->query_loaded_count;
  if (tab->query_total_known && new_offset >= tab->query_total_rows)
    return false;

  char *page_sql = query_page_sql(tab, TUI_PAGE_SIZE, new_offset);
  char *err = NULL;
  bool cancelled = false;
  ResultSet *more =
      query_run_derived(state, page_sql, "Loading rows...", &err, &cancelled);
  free(page_sql);

  if (!more || more->num_rows < TUI_PAGE_SIZE) {
    if (more || !err) {
      /* A short page is the end: the total is known now */
      tab->query_total_rows = new_offset + (more ? more->num_rows : 0);
      tab->query_total_known = true;
    } else if (!cancelled) {
      tui_set_error(state, "Load failed: %s", err);
    }
  }
  free(err);
  if (!more || more->num_rows == 0) {
    db_result_free(more);
    return false;
  }

  size_t old_count = tab->query_results->num_rows;
  size_t new_count = old_count + more->num_rows;
  tab->query_results->rows =
      safe_reallocarray(tab->query_results->rows, new_count, sizeof(Row));
  for (size_t i = 0; i < more->num_rows; i++) {
    tab->query_results->rows[old_count + i] = more->rows[i];
    more->rows[i].cells = NULL; /* Moved */
    more->rows[i].num_cells = 0;
  }
  tab->query_results->num_rows = new_count;
  tab->query_loaded_count = new_count;
  db_result_free(more);

  /* Trim old data to keep memory bounded */
  query_trim_loaded_data(state, tab);
  query_status_loaded(state, tab);
  return true;
}

/* Load previous rows (prepend to current query results) */
bool query_load_prev_rows(TuiState *state, Tab *tab) {
  if (!state || !TUI_CONN(state) || !tab || !tab->query_paginated ||
      !tab->query_base_sql || !tab->query_results)
    return false;
  if (tab->query_loaded_offset == 0)
    return false; /* Already at beginning */

  /* Exactly the rows before the window */
  size_t load_count = tab->query_loaded_offset > TUI_PAGE_SIZE
                          ? TUI_PAGE_SIZE
                          : tab->query_loaded_offset;
  size_t new_offset = tab->query_loaded_offset - load_count;

  char *page_sql = query_page_sql(tab, load_count, new_offset);
  char *err = NULL;
  bool cancelled = false;
  ResultSet *more =
      query_run_derived(state, page_sql, "Loading rows...", &err, &cancelled);
  free(page_sql);
  if (!more || more->num_rows == 0) {
    if (err && !cancelled) {
      tui_set_error(state, "Load failed: %s", err);
    }
    free(err);
    db_result_free(more);
    return false;
  }
  free(err);

  size_t old_count = tab->query_results->num_rows;
  size_t new_count = old_count + more->num_rows;
  Row *new_rows = safe_reallocarray(NULL, new_count, sizeof(Row));
  for (size_t i = 0; i < more->num_rows; i++) {
    new_rows[i] = more->rows[i];
    more->rows[i].cells = NULL; /* Moved */
    more->rows[i].num_cells = 0;
  }
  for (size_t i = 0; i < old_count; i++) {
    new_rows[more->num_rows + i] = tab->query_results->rows[i];
  }
  free(tab->query_results->rows);
  tab->query_results->rows = new_rows;
  tab->query_results->num_rows = new_count;

  /* Adjust cursor position (it's now offset by the prepended rows) */
  tab->query_result_row += more->num_rows;
  tab->query_result_scroll_row += more->num_rows;
  tab->query_loaded_offset = new_offset;
  tab->query_loaded_count = new_count;
  db_result_free(more);

  query_trim_loaded_data(state, tab);
  query_status_loaded(state, tab);
  return true;
}

/* Trim loaded query data to keep memory bounded */
void query_trim_loaded_data(TuiState *state, Tab *tab) {
  (void)state; /* Currently unused */

  if (!tab || !tab->query_results || tab->query_results->num_rows == 0)
    return;

  size_t max_rows = MAX_LOADED_PAGES * TUI_PAGE_SIZE;
  if (tab->query_loaded_count <= max_rows)
    return;

  /* Calculate cursor's page within loaded data */
  size_t cursor_page = tab->query_result_row / TUI_PAGE_SIZE;
  size_t total_pages = (tab->query_loaded_count + TUI_PAGE_SIZE - 1) / TUI_PAGE_SIZE;

  /* Determine pages to keep: TRIM_DISTANCE_PAGES on each side of cursor */
  size_t keep_start_page = 0;
  size_t keep_end_page = total_pages;

  if (cursor_page > TRIM_DISTANCE_PAGES) {
    keep_start_page = cursor_page - TRIM_DISTANCE_PAGES;
  }
  if (cursor_page + TRIM_DISTANCE_PAGES + 1 < total_pages) {
    keep_end_page = cursor_page + TRIM_DISTANCE_PAGES + 1;
  }

  /* Ensure we don't exceed MAX_LOADED_PAGES */
  size_t pages_to_keep = keep_end_page - keep_start_page;
  if (pages_to_keep > MAX_LOADED_PAGES) {
    size_t excess = pages_to_keep - MAX_LOADED_PAGES;
    size_t pages_before_cursor = cursor_page - keep_start_page;
    size_t pages_after_cursor = keep_end_page - cursor_page - 1;

    if (pages_before_cursor > pages_after_cursor) {
      keep_start_page += excess;
    } else {
      keep_end_page -= excess;
    }
  }

  /* Convert pages to row indices */
  size_t trim_start = keep_start_page * TUI_PAGE_SIZE;
  size_t trim_end = keep_end_page * TUI_PAGE_SIZE;
  if (trim_end > tab->query_loaded_count)
    trim_end = tab->query_loaded_count;

  /* Check if we actually need to trim */
  if (trim_start == 0 && trim_end >= tab->query_loaded_count)
    return;

  /* Free rows before trim_start */
  for (size_t i = 0; i < trim_start; i++) {
    Row *row = &tab->query_results->rows[i];
    for (size_t j = 0; j < row->num_cells; j++) {
      db_value_free(&row->cells[j]);
    }
    free(row->cells);
  }

  /* Free rows after trim_end */
  for (size_t i = trim_end; i < tab->query_loaded_count; i++) {
    Row *row = &tab->query_results->rows[i];
    for (size_t j = 0; j < row->num_cells; j++) {
      db_value_free(&row->cells[j]);
    }
    free(row->cells);
  }

  /* Move remaining rows to beginning of array */
  size_t new_count = trim_end - trim_start;
  if (trim_start > 0) {
    memmove(tab->query_results->rows, tab->query_results->rows + trim_start,
            new_count * sizeof(Row));
  }

  /* Resize array */
  tab->query_results->rows = safe_reallocarray(tab->query_results->rows, new_count, sizeof(Row));
  tab->query_results->num_rows = new_count;

  /* Adjust cursor and scroll positions */
  if (tab->query_result_row >= trim_start) {
    tab->query_result_row -= trim_start;
  } else {
    tab->query_result_row = 0;
  }

  if (tab->query_result_scroll_row >= trim_start) {
    tab->query_result_scroll_row -= trim_start;
  } else {
    tab->query_result_scroll_row = 0;
  }

  /* Update tracking */
  tab->query_loaded_offset += trim_start;
  tab->query_loaded_count = new_count;
}

/* Check if more rows need to be loaded based on cursor position */
void query_check_load_more(TuiState *state, Tab *tab) {
  if (!tab || !tab->query_results || !tab->query_paginated)
    return;

  /* If cursor is within LOAD_THRESHOLD of the END, load more at end */
  size_t rows_from_end =
      tab->query_results->num_rows > tab->query_result_row
          ? tab->query_results->num_rows - tab->query_result_row
          : 0;

  if (rows_from_end < LOAD_THRESHOLD) {
    /* Check if there are more rows to load at end */
    size_t loaded_end = tab->query_loaded_offset + tab->query_loaded_count;
    if (!tab->query_total_known || loaded_end < tab->query_total_rows) {
      query_load_more_rows(state, tab);
    }
  }

  /* If cursor is within LOAD_THRESHOLD of the BEGINNING, load previous rows */
  if (tab->query_result_row < LOAD_THRESHOLD && tab->query_loaded_offset > 0) {
    query_load_prev_rows(state, tab);
  }
}
