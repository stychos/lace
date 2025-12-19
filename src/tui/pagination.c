/*
 * lace - Database Viewer and Manager
 * Pagination and data loading
 */

#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>

/* Calculate column widths based on data */
void tui_calculate_column_widths(TuiState *state) {
  if (!state || !state->data)
    return;

  free(state->col_widths);
  state->num_col_widths = state->data->num_columns;
  state->col_widths = calloc(state->num_col_widths, sizeof(int));

  if (!state->col_widths)
    return;

  /* Start with column name widths */
  for (size_t i = 0; i < state->data->num_columns; i++) {
    const char *name = state->data->columns[i].name;
    int len = name ? (int)strlen(name) : 0;
    state->col_widths[i] = len < MIN_COL_WIDTH ? MIN_COL_WIDTH : len;
  }

  /* Check data widths */
  for (size_t row = 0; row < state->data->num_rows && row < 100; row++) {
    Row *r = &state->data->rows[row];
    if (!r->cells)
      continue;
    for (size_t col = 0; col < state->data->num_columns && col < r->num_cells;
         col++) {
      char *str = db_value_to_string(&r->cells[col]);
      if (str) {
        int len = (int)strlen(str);
        if (len > state->col_widths[col]) {
          state->col_widths[col] = len;
        }
        free(str);
      }
    }
  }

  /* Apply max width */
  for (size_t i = 0; i < state->num_col_widths; i++) {
    if (state->col_widths[i] > MAX_COL_WIDTH) {
      state->col_widths[i] = MAX_COL_WIDTH;
    }
  }
}

/* Get column width */
int tui_get_column_width(TuiState *state, size_t col) {
  if (!state || !state->col_widths || col >= state->num_col_widths) {
    return DEFAULT_COL_WIDTH;
  }
  return state->col_widths[col];
}

/* Load table data */
bool tui_load_table_data(TuiState *state, const char *table) {
  if (!state || !state->conn || !table)
    return false;

  /* Free old data */
  if (state->data) {
    db_result_free(state->data);
    state->data = NULL;
  }

  if (state->schema) {
    db_schema_free(state->schema);
    state->schema = NULL;
  }

  /* Load schema */
  char *err = NULL;
  state->schema = db_get_table_schema(state->conn, table, &err);
  if (err) {
    tui_set_error(state, "Schema: %s", err);
    free(err);
    /* Continue anyway - we can still show data */
  }

  /* Get total row count */
  int64_t count = db_count_rows(state->conn, table, &err);
  if (count < 0) {
    /* Fallback if COUNT fails */
    count = 0;
    free(err);
    err = NULL;
  }
  state->total_rows = (size_t)count;
  state->page_size = PAGE_SIZE;
  state->loaded_offset = 0;

  /* Load first page of data */
  state->data =
      db_query_page(state->conn, table, 0, PAGE_SIZE, NULL, false, &err);
  if (!state->data) {
    tui_set_error(state, "Query failed: %s", err ? err : "Unknown error");
    free(err);
    return false;
  }

  state->loaded_count = state->data->num_rows;

  /* Apply schema column names to result set */
  if (state->schema && state->data) {
    size_t min_cols = state->schema->num_columns;
    if (state->data->num_columns < min_cols) {
      min_cols = state->data->num_columns;
    }
    for (size_t i = 0; i < min_cols; i++) {
      if (state->schema->columns[i].name) {
        free(state->data->columns[i].name);
        state->data->columns[i].name = str_dup(state->schema->columns[i].name);
        state->data->columns[i].type = state->schema->columns[i].type;
      }
    }
  }

  /* Reset cursor */
  state->cursor_row = 0;
  state->cursor_col = 0;
  state->scroll_row = 0;
  state->scroll_col = 0;

  /* Calculate column widths */
  tui_calculate_column_widths(state);

  /* Update current workspace's pointers to prevent dangling references */
  if (state->num_workspaces > 0 &&
      state->current_workspace < state->num_workspaces) {
    Workspace *ws = &state->workspaces[state->current_workspace];
    ws->data = state->data;
    ws->schema = state->schema;
    ws->col_widths = state->col_widths;
    ws->num_col_widths = state->num_col_widths;
    ws->total_rows = state->total_rows;
    ws->loaded_offset = state->loaded_offset;
    ws->loaded_count = state->loaded_count;
  }

  /* Clear any previous status message so column info is shown */
  free(state->status_msg);
  state->status_msg = NULL;
  state->status_is_error = false;
  return true;
}

/* Load more rows at end of current data */
bool tui_load_more_rows(TuiState *state) {
  if (!state || !state->conn || !state->data || !state->tables)
    return false;
  if (state->current_table >= state->num_tables)
    return false;

  const char *table = state->tables[state->current_table];
  size_t new_offset = state->loaded_offset + state->loaded_count;

  /* Check if there are more rows to load */
  if (new_offset >= state->total_rows)
    return false;

  char *err = NULL;
  ResultSet *more = db_query_page(state->conn, table, new_offset, PAGE_SIZE,
                                  NULL, false, &err);
  if (!more || more->num_rows == 0) {
    if (more)
      db_result_free(more);
    free(err);
    return false;
  }

  /* Extend existing rows array */
  size_t old_count = state->data->num_rows;
  size_t new_count = old_count + more->num_rows;

  /* Validate source data consistency */
  if (more->num_rows > 0 && !more->rows) {
    db_result_free(more);
    return false;
  }

  /* Check for overflow and enforce maximum row limit (1M rows) */
  if (new_count < old_count || new_count > SIZE_MAX / sizeof(Row) ||
      new_count > 1000000) {
    db_result_free(more);
    return false;
  }

  Row *new_rows = realloc(state->data->rows, new_count * sizeof(Row));
  if (!new_rows) {
    db_result_free(more);
    return false;
  }

  state->data->rows = new_rows;

  /* Copy new rows */
  for (size_t i = 0; i < more->num_rows; i++) {
    state->data->rows[old_count + i] = more->rows[i];
    /* Clear source so free doesn't deallocate the cells we moved */
    more->rows[i].cells = NULL;
    more->rows[i].num_cells = 0;
  }

  state->data->num_rows = new_count;
  state->loaded_count = new_count;

  db_result_free(more);

  /* Trim old data to keep memory bounded */
  tui_trim_loaded_data(state);

  tui_set_status(state, "Loaded %zu/%zu rows", state->loaded_count,
                 state->total_rows);
  return true;
}

/* Load rows at specific offset (replaces current data) */
bool tui_load_rows_at(TuiState *state, size_t offset) {
  if (!state || !state->conn || !state->tables)
    return false;
  if (state->current_table >= state->num_tables)
    return false;

  const char *table = state->tables[state->current_table];

  /* Clamp offset */
  if (offset >= state->total_rows) {
    offset = state->total_rows > PAGE_SIZE ? state->total_rows - PAGE_SIZE : 0;
  }

  char *err = NULL;
  ResultSet *data =
      db_query_page(state->conn, table, offset, PAGE_SIZE, NULL, false, &err);
  if (!data) {
    tui_set_error(state, "Query failed: %s", err ? err : "Unknown error");
    free(err);
    return false;
  }

  /* Free old data but keep schema */
  if (state->data) {
    db_result_free(state->data);
  }
  state->data = data;
  state->loaded_offset = offset;
  state->loaded_count = data->num_rows;

  /* Apply schema column names */
  if (state->schema && state->data) {
    size_t min_cols = state->schema->num_columns;
    if (state->data->num_columns < min_cols) {
      min_cols = state->data->num_columns;
    }
    for (size_t i = 0; i < min_cols; i++) {
      if (state->schema->columns[i].name) {
        free(state->data->columns[i].name);
        state->data->columns[i].name = str_dup(state->schema->columns[i].name);
        state->data->columns[i].type = state->schema->columns[i].type;
      }
    }
  }

  /* Update current workspace's pointers to prevent dangling references */
  if (state->num_workspaces > 0 &&
      state->current_workspace < state->num_workspaces) {
    Workspace *ws = &state->workspaces[state->current_workspace];
    ws->data = state->data;
    ws->col_widths = state->col_widths;
    ws->num_col_widths = state->num_col_widths;
    ws->total_rows = state->total_rows;
    ws->loaded_offset = state->loaded_offset;
    ws->loaded_count = state->loaded_count;
  }

  return true;
}

/* Load previous rows (prepend to current data) */
bool tui_load_prev_rows(TuiState *state) {
  if (!state || !state->conn || !state->data || !state->tables)
    return false;
  if (state->current_table >= state->num_tables)
    return false;
  if (state->loaded_offset == 0)
    return false; /* Already at beginning */

  const char *table = state->tables[state->current_table];

  /* Calculate how many rows to load before current offset */
  size_t load_count = PAGE_SIZE;
  size_t new_offset = 0;
  if (state->loaded_offset > load_count) {
    new_offset = state->loaded_offset - load_count;
  } else {
    load_count = state->loaded_offset;
    new_offset = 0;
  }

  char *err = NULL;
  ResultSet *more = db_query_page(state->conn, table, new_offset, load_count,
                                  NULL, false, &err);
  if (!more || more->num_rows == 0) {
    if (more)
      db_result_free(more);
    free(err);
    return false;
  }

  /* Prepend rows to existing data */
  size_t old_count = state->data->num_rows;
  size_t new_count = old_count + more->num_rows;

  /* Check for overflow */
  if (new_count < old_count || new_count > SIZE_MAX / sizeof(Row)) {
    db_result_free(more);
    return false;
  }

  Row *new_rows = malloc(new_count * sizeof(Row));
  if (!new_rows) {
    db_result_free(more);
    return false;
  }

  /* Copy new rows first (prepend) */
  for (size_t i = 0; i < more->num_rows; i++) {
    new_rows[i] = more->rows[i];
    /* Clear source so free doesn't deallocate the cells we moved */
    more->rows[i].cells = NULL;
    more->rows[i].num_cells = 0;
  }

  /* Then copy old rows */
  for (size_t i = 0; i < old_count; i++) {
    new_rows[more->num_rows + i] = state->data->rows[i];
  }

  /* Free old array (but not the cells which we moved) */
  free(state->data->rows);
  state->data->rows = new_rows;
  state->data->num_rows = new_count;

  /* Adjust cursor position (it's now offset by the prepended rows) */
  state->cursor_row += more->num_rows;
  state->scroll_row += more->num_rows;

  /* Update tracking */
  state->loaded_offset = new_offset;
  state->loaded_count = new_count;

  db_result_free(more);

  /* Trim old data to keep memory bounded */
  tui_trim_loaded_data(state);

  tui_set_status(state, "Loaded %zu/%zu rows", state->loaded_count,
                 state->total_rows);
  return true;
}

/* Trim loaded data to keep memory bounded */
void tui_trim_loaded_data(TuiState *state) {
  if (!state || !state->data || state->data->num_rows == 0)
    return;

  size_t max_rows = MAX_LOADED_PAGES * PAGE_SIZE;
  if (state->loaded_count <= max_rows)
    return;

  /* Calculate cursor's page within loaded data */
  size_t cursor_page = state->cursor_row / PAGE_SIZE;
  size_t total_pages = (state->loaded_count + PAGE_SIZE - 1) / PAGE_SIZE;

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
    /* Trim from the end that's farther from cursor */
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
  size_t trim_start = keep_start_page * PAGE_SIZE;
  size_t trim_end = keep_end_page * PAGE_SIZE;
  if (trim_end > state->loaded_count)
    trim_end = state->loaded_count;

  /* Check if we actually need to trim */
  if (trim_start == 0 && trim_end >= state->loaded_count)
    return;

  /* Free rows before trim_start */
  for (size_t i = 0; i < trim_start; i++) {
    Row *row = &state->data->rows[i];
    for (size_t j = 0; j < row->num_cells; j++) {
      db_value_free(&row->cells[j]);
    }
    free(row->cells);
  }

  /* Free rows after trim_end */
  for (size_t i = trim_end; i < state->loaded_count; i++) {
    Row *row = &state->data->rows[i];
    for (size_t j = 0; j < row->num_cells; j++) {
      db_value_free(&row->cells[j]);
    }
    free(row->cells);
  }

  /* Move remaining rows to beginning of array */
  size_t new_count = trim_end - trim_start;
  if (trim_start > 0) {
    memmove(state->data->rows, state->data->rows + trim_start,
            new_count * sizeof(Row));
  }

  /* Resize array (optional, realloc to shrink) */
  Row *new_rows = realloc(state->data->rows, new_count * sizeof(Row));
  if (new_rows) {
    state->data->rows = new_rows;
  }
  state->data->num_rows = new_count;

  /* Adjust cursor and scroll positions */
  if (state->cursor_row >= trim_start) {
    state->cursor_row -= trim_start;
  } else {
    state->cursor_row = 0;
  }

  if (state->scroll_row >= trim_start) {
    state->scroll_row -= trim_start;
  } else {
    state->scroll_row = 0;
  }

  /* Update tracking */
  state->loaded_offset += trim_start;
  state->loaded_count = new_count;
}

/* Check if more rows need to be loaded based on cursor position */
void tui_check_load_more(TuiState *state) {
  if (!state || !state->data)
    return;

  /* If cursor is within LOAD_THRESHOLD of the END, load more at end */
  size_t rows_from_end = state->data->num_rows > state->cursor_row
                             ? state->data->num_rows - state->cursor_row
                             : 0;

  if (rows_from_end < LOAD_THRESHOLD) {
    /* Check if there are more rows to load at end */
    size_t loaded_end = state->loaded_offset + state->loaded_count;
    if (loaded_end < state->total_rows) {
      tui_load_more_rows(state);
    }
  }

  /* If cursor is within LOAD_THRESHOLD of the BEGINNING, load previous rows */
  if (state->cursor_row < LOAD_THRESHOLD && state->loaded_offset > 0) {
    tui_load_prev_rows(state);
  }
}
