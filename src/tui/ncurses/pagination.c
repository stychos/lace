/*
 * Lace
 * Pagination and data loading
 *
 * Uses VmTable for cursor/scroll state access where applicable.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../async/async.h"
#include "../../viewmodel/vm_table.h"
#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Helper to get VmTable, returns NULL if not valid */
static VmTable *get_vm_table(TuiState *state) {
  if (!state || !state->vm_table)
    return NULL;
  if (!vm_table_valid(state->vm_table))
    return NULL;
  return state->vm_table;
}

/* Calculate column widths based on data */
void tui_calculate_column_widths(TuiState *state) {
  if (!state || !state->data)
    return;

  /* Validate columns array exists */
  if (!state->data->columns || state->data->num_columns == 0)
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

/* Build WHERE clause for current tab filters */
static char *build_filter_where(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->filters.num_filters == 0)
    return NULL;

  if (!state->schema || !state->conn)
    return NULL;

  char *err = NULL;
  char *where = filters_build_where(&tab->filters, state->schema,
                                    state->conn->driver->name, &err);
  free(err);
  return where;
}

/* Build multi-column ORDER BY clause for current tab (NULL if no sorting)
 * Caller must free the returned string */
static char *build_order_clause(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->num_sort_entries == 0)
    return NULL;

  if (!state->schema || !state->conn)
    return NULL;

  /* Determine quote character based on driver */
  bool use_backtick = state->conn->driver &&
                      (strcmp(state->conn->driver->name, "mysql") == 0 ||
                       strcmp(state->conn->driver->name, "mariadb") == 0);

  /* Build ORDER BY clause */
  StringBuilder *sb = sb_new(128);
  if (!sb)
    return NULL;

  bool first_added = false;
  for (size_t i = 0; i < tab->num_sort_entries; i++) {
    SortEntry *entry = &tab->sort_entries[i];
    if (entry->column >= state->schema->num_columns)
      continue;

    const char *col_name = state->schema->columns[entry->column].name;
    if (!col_name)
      continue;

    /* Escape column name */
    char *escaped = use_backtick ? str_escape_identifier_backtick(col_name)
                                 : str_escape_identifier_dquote(col_name);
    if (!escaped) {
      sb_free(sb);
      return NULL;
    }

    /* Add separator if not first valid entry */
    if (first_added) {
      sb_append(sb, ", ");
    }
    first_added = true;

    /* Add column with direction */
    sb_printf(sb, "%s %s", escaped, entry->direction == SORT_ASC ? "ASC" : "DESC");
    free(escaped);
  }

  char *result = sb_to_string(sb);
  return result;
}

/* Load table data */
bool tui_load_table_data(TuiState *state, const char *table) {
  if (!state || !state->conn || !table)
    return false;

  /* Clear any previous error */
  Tab *tab = TUI_TAB(state);
  if (tab) {
    free(tab->table_error);
    tab->table_error = NULL;
  }

  /* Free old data */
  if (state->data) {
    db_result_free(state->data);
    state->data = NULL;
  }

  if (state->schema) {
    db_schema_free(state->schema);
    state->schema = NULL;
  }

  /* Load schema with progress dialog */
  AsyncOperation schema_op;
  async_init(&schema_op);
  schema_op.op_type = ASYNC_OP_GET_SCHEMA;
  schema_op.conn = state->conn;
  schema_op.table_name = str_dup(table);

  if (schema_op.table_name && async_start(&schema_op)) {
    bool completed =
        tui_show_processing_dialog(state, &schema_op, "Loading schema...");
    if (completed && schema_op.state == ASYNC_STATE_COMPLETED) {
      state->schema = (TableSchema *)schema_op.result;
    } else if (schema_op.state == ASYNC_STATE_CANCELLED) {
      async_free(&schema_op);
      tui_set_status(state, "Operation cancelled");
      return false;
    }
    /* Errors are non-fatal for schema - we can continue */
  }
  async_free(&schema_op);

  /* Update tab schema pointer for filter building */
  if (tab) {
    tab->schema = state->schema;
  }

  /* Build WHERE clause from filters */
  char *where_clause = build_filter_where(state);

  /* Get total row count with progress dialog (uses approximate if available) */
  AsyncOperation count_op;
  async_init(&count_op);
  count_op.conn = state->conn;
  count_op.table_name = str_dup(table);
  bool is_approximate = false;

  int64_t count = 0;
  if (where_clause) {
    /* Filtered count - must be exact */
    count_op.op_type = ASYNC_OP_COUNT_ROWS_WHERE;
    count_op.where_clause = str_dup(where_clause);
  } else {
    /* Unfiltered - can use approximate count */
    count_op.op_type = ASYNC_OP_COUNT_ROWS;
    count_op.use_approximate = true;
  }

  if (count_op.table_name && async_start(&count_op)) {
    bool completed =
        tui_show_processing_dialog(state, &count_op, "Counting rows...");
    if (completed && count_op.state == ASYNC_STATE_COMPLETED) {
      count = count_op.count;
      is_approximate = count_op.is_approximate;
    } else if (count_op.state == ASYNC_STATE_CANCELLED) {
      async_free(&count_op);
      free(where_clause);
      tui_set_status(state, "Operation cancelled");
      return false;
    }
  }
  async_free(&count_op);

  state->total_rows = count >= 0 ? (size_t)count : 0;
  state->page_size = PAGE_SIZE;
  state->loaded_offset = 0;

  /* Store approximate flag and unfiltered count in tab if available */
  if (tab) {
    tab->row_count_approximate = is_approximate;
    /* Store unfiltered total only when loading without filters */
    if (!where_clause) {
      tab->unfiltered_total_rows = state->total_rows;
    }
  }

  /* Load first page of data with progress dialog */
  AsyncOperation data_op;
  async_init(&data_op);
  data_op.conn = state->conn;
  data_op.table_name = str_dup(table);
  data_op.offset = 0;
  data_op.limit = PAGE_SIZE * PREFETCH_PAGES;
  data_op.order_by = build_order_clause(state);
  data_op.desc = false; /* Direction is in the clause */

  if (where_clause) {
    data_op.op_type = ASYNC_OP_QUERY_PAGE_WHERE;
    data_op.where_clause = str_dup(where_clause);
  } else {
    data_op.op_type = ASYNC_OP_QUERY_PAGE;
  }
  free(where_clause);

  if (!data_op.table_name || !async_start(&data_op)) {
    async_free(&data_op);
    tui_set_error(state, "Failed to start data load");
    return false;
  }

  bool completed =
      tui_show_processing_dialog(state, &data_op, "Loading data...");

  if (!completed || data_op.state == ASYNC_STATE_CANCELLED) {
    async_free(&data_op);
    tui_set_status(state, "Operation cancelled");
    return false;
  }

  if (data_op.state == ASYNC_STATE_ERROR) {
    const char *err_msg = data_op.error ? data_op.error : "Unknown error";
    tui_set_error(state, "Query failed: %s", err_msg);
    /* Store error in tab for display */
    if (tab) {
      free(tab->table_error);
      tab->table_error = str_dup(err_msg);
    }
    async_free(&data_op);
    return false;
  }

  state->data = (ResultSet *)data_op.result;
  async_free(&data_op);

  if (!state->data) {
    tui_set_error(state, "No data returned");
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

  /* Update current tab's pointers to prevent dangling references */
  if (tab) {
    tab->data = state->data;
    tab->schema = state->schema;
    tab->col_widths = state->col_widths;
    tab->num_col_widths = state->num_col_widths;
    tab->total_rows = state->total_rows;
    tab->loaded_offset = state->loaded_offset;
    tab->loaded_count = state->loaded_count;
    /* Sync cursor/scroll to tab so VmTable reads correct values */
    tab->cursor_row = state->cursor_row;
    tab->cursor_col = state->cursor_col;
    tab->scroll_row = state->scroll_row;
    tab->scroll_col = state->scroll_col;
  }

  /* Clear any previous status message so column info is shown */
  free(state->status_msg);
  state->status_msg = NULL;
  state->status_is_error = false;

  /* Bind VmTable to the current tab so navigation functions work */
  if (tab && tab->type == TAB_TYPE_TABLE) {
    if (!state->vm_table) {
      state->vm_table = vm_table_create(state->app, tab, NULL);
    } else {
      vm_table_bind(state->vm_table, tab);
    }
  }

  return true;
}

/* Refresh table data while preserving position */
bool tui_refresh_table(TuiState *state) {
  if (!state || !state->conn || !state->tables)
    return false;
  if (state->current_table >= state->num_tables)
    return false;

  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE || !tab->table_name)
    return false;

  /* Cancel any pending background load */
  tui_cancel_background_load(state);

  /* Save current position */
  size_t saved_cursor_row = state->cursor_row;
  size_t saved_cursor_col = state->cursor_col;
  size_t saved_scroll_row = state->scroll_row;
  size_t saved_scroll_col = state->scroll_col;
  size_t saved_offset = state->loaded_offset;

  /* Calculate absolute row position (offset + cursor) */
  size_t abs_row = saved_offset + saved_cursor_row;

  /* Reload table data */
  if (!tui_load_table_data(state, tab->table_name)) {
    return false;
  }

  /* Restore position, clamped to new bounds */
  if (state->data && state->data->num_rows > 0) {
    /* Try to load at the same offset if we were scrolled */
    if (saved_offset > 0 && abs_row < state->total_rows) {
      /* Load data at saved offset */
      size_t target_offset = saved_offset;
      if (target_offset >= state->total_rows) {
        target_offset =
            state->total_rows > PAGE_SIZE ? state->total_rows - PAGE_SIZE : 0;
      }

      if (target_offset > 0) {
        tui_load_rows_at_with_dialog(state, target_offset);
      }
    }

    /* Restore cursor within bounds */
    if (state->data->num_rows > 0) {
      state->cursor_row = saved_cursor_row < state->data->num_rows
                              ? saved_cursor_row
                              : state->data->num_rows - 1;
    } else {
      state->cursor_row = 0;
    }

    state->cursor_col =
        saved_cursor_col < state->data->num_columns
            ? saved_cursor_col
            : (state->data->num_columns > 0 ? state->data->num_columns - 1 : 0);

    /* Restore scroll within bounds */
    size_t max_scroll = state->data->num_rows > (size_t)state->content_rows
                            ? state->data->num_rows - state->content_rows
                            : 0;
    state->scroll_row =
        saved_scroll_row < max_scroll ? saved_scroll_row : max_scroll;
    state->scroll_col = saved_scroll_col;

    /* Ensure cursor is visible */
    if (state->cursor_row < state->scroll_row) {
      state->scroll_row = state->cursor_row;
    } else if (state->cursor_row >=
               state->scroll_row + (size_t)state->content_rows) {
      state->scroll_row = state->cursor_row - state->content_rows + 1;
    }
  }

  /* Update tab */
  tab->cursor_row = state->cursor_row;
  tab->cursor_col = state->cursor_col;
  tab->scroll_row = state->scroll_row;
  tab->scroll_col = state->scroll_col;

  tui_set_status(state, "Table refreshed (%zu rows)", state->total_rows);
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

  /* Build WHERE clause from filters */
  char *where_clause = build_filter_where(state);
  char *order_clause = build_order_clause(state);

  char *err = NULL;
  ResultSet *more;
  if (where_clause) {
    more = db_query_page_where(state->conn, table, new_offset, PAGE_SIZE,
                               where_clause, order_clause, false, &err);
  } else {
    more = db_query_page(state->conn, table, new_offset, PAGE_SIZE, order_clause,
                         false, &err);
  }
  free(where_clause);
  free(order_clause);
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

  /* Build WHERE clause from filters */
  char *where_clause = build_filter_where(state);
  char *order_clause = build_order_clause(state);

  char *err = NULL;
  ResultSet *data;
  if (where_clause) {
    data = db_query_page_where(state->conn, table, offset, PAGE_SIZE,
                               where_clause, order_clause, false, &err);
  } else {
    data = db_query_page(state->conn, table, offset, PAGE_SIZE, order_clause,
                         false, &err);
  }
  free(where_clause);
  free(order_clause);
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

  /* Update current tab's pointers to prevent dangling references */
  Tab *tab = TUI_TAB(state);
  if (tab) {
    tab->data = state->data;
    tab->col_widths = state->col_widths;
    tab->num_col_widths = state->num_col_widths;
    tab->total_rows = state->total_rows;
    tab->loaded_offset = state->loaded_offset;
    tab->loaded_count = state->loaded_count;
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

  /* Build WHERE clause from filters */
  char *where_clause = build_filter_where(state);
  char *order_clause = build_order_clause(state);

  char *err = NULL;
  ResultSet *more;
  if (where_clause) {
    more = db_query_page_where(state->conn, table, new_offset, load_count,
                               where_clause, order_clause, false, &err);
  } else {
    more = db_query_page(state->conn, table, new_offset, load_count, order_clause,
                         false, &err);
  }
  free(where_clause);
  free(order_clause);
  if (!more || more->num_rows == 0) {
    if (more)
      db_result_free(more);
    free(err);
    return false;
  }

  /* Prepend rows to existing data */
  size_t old_count = state->data->num_rows;
  size_t new_count = old_count + more->num_rows;

  /* Check for overflow and enforce maximum row limit (1M rows) */
  if (new_count < old_count || new_count > SIZE_MAX / sizeof(Row) ||
      new_count > 1000000) {
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

  /* Get current cursor/scroll from VmTable if available */
  size_t cursor_row = state->cursor_row;
  size_t scroll_row = state->scroll_row;
  VmTable *vm = get_vm_table(state);
  if (vm) {
    vm_table_get_cursor(vm, &cursor_row, NULL);
    vm_table_get_scroll(vm, &scroll_row, NULL);
  }

  /* Adjust cursor position (it's now offset by the prepended rows) */
  cursor_row += more->num_rows;
  scroll_row += more->num_rows;

  /* Update via viewmodel if available */
  if (vm) {
    size_t cursor_col, scroll_col;
    vm_table_get_cursor(vm, NULL, &cursor_col);
    vm_table_get_scroll(vm, NULL, &scroll_col);
    vm_table_set_cursor(vm, cursor_row, cursor_col);
    vm_table_set_scroll(vm, scroll_row, scroll_col);
  }

  /* Sync to compatibility layer */
  state->cursor_row = cursor_row;
  state->scroll_row = scroll_row;

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

  /* Get cursor position from VmTable if available */
  size_t cursor_row = state->cursor_row;
  size_t scroll_row = state->scroll_row;
  VmTable *vm = get_vm_table(state);
  if (vm) {
    vm_table_get_cursor(vm, &cursor_row, NULL);
    vm_table_get_scroll(vm, &scroll_row, NULL);
  }

  /* Calculate cursor's page within loaded data */
  size_t cursor_page = cursor_row / PAGE_SIZE;
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
  if (cursor_row >= trim_start) {
    cursor_row -= trim_start;
  } else {
    cursor_row = 0;
  }

  if (scroll_row >= trim_start) {
    scroll_row -= trim_start;
  } else {
    scroll_row = 0;
  }

  /* Update via viewmodel if available */
  if (vm) {
    size_t cursor_col, scroll_col;
    vm_table_get_cursor(vm, NULL, &cursor_col);
    vm_table_get_scroll(vm, NULL, &scroll_col);
    vm_table_set_cursor(vm, cursor_row, cursor_col);
    vm_table_set_scroll(vm, scroll_row, scroll_col);
  }

  /* Sync to compatibility layer */
  state->cursor_row = cursor_row;
  state->scroll_row = scroll_row;

  /* Update tracking */
  state->loaded_offset += trim_start;
  state->loaded_count = new_count;
}

/* Check if more rows need to be loaded based on cursor position */
void tui_check_load_more(TuiState *state) {
  if (!state || !state->data)
    return;

  /* Don't do synchronous load if background load is in progress */
  Tab *tab = TUI_TAB(state);
  if (tab && tab->bg_load_op != NULL)
    return;

  /* Get cursor position from VmTable if available */
  size_t cursor_row = state->cursor_row;
  VmTable *vm = get_vm_table(state);
  if (vm) {
    vm_table_get_cursor(vm, &cursor_row, NULL);
  }

  /* If cursor is within LOAD_THRESHOLD of the END, load more at end */
  size_t rows_from_end = state->data->num_rows > cursor_row
                             ? state->data->num_rows - cursor_row
                             : 0;

  if (rows_from_end < LOAD_THRESHOLD) {
    /* Check if there are more rows to load at end */
    size_t loaded_end = state->loaded_offset + state->loaded_count;
    if (loaded_end < state->total_rows) {
      tui_load_more_rows(state);
    }
  }

  /* If cursor is within LOAD_THRESHOLD of the BEGINNING, load previous rows */
  if (cursor_row < LOAD_THRESHOLD && state->loaded_offset > 0) {
    tui_load_prev_rows(state);
  }
}

/* ============================================================================
 * Blocking load with dialog (for fast scrolling past loaded data)
 * ============================================================================
 */

/* Merge new page result into existing data */
static bool merge_page_result(TuiState *state, ResultSet *new_data,
                              bool forward) {
  if (!state || !state->data || !new_data || new_data->num_rows == 0)
    return false;

  size_t old_count = state->data->num_rows;
  size_t new_count = old_count + new_data->num_rows;

  /* Check for overflow and enforce maximum row limit */
  if (new_count < old_count || new_count > SIZE_MAX / sizeof(Row) ||
      new_count > 1000000) {
    return false;
  }

  if (forward) {
    /* Append: extend existing rows array */
    Row *new_rows = realloc(state->data->rows, new_count * sizeof(Row));
    if (!new_rows)
      return false;

    state->data->rows = new_rows;

    /* Copy new rows at end */
    for (size_t i = 0; i < new_data->num_rows; i++) {
      state->data->rows[old_count + i] = new_data->rows[i];
      /* Clear source so free doesn't deallocate cells we moved */
      new_data->rows[i].cells = NULL;
      new_data->rows[i].num_cells = 0;
    }

    state->data->num_rows = new_count;
    state->loaded_count = new_count;
  } else {
    /* Prepend: allocate new array */
    Row *new_rows = malloc(new_count * sizeof(Row));
    if (!new_rows)
      return false;

    /* Copy new rows first */
    for (size_t i = 0; i < new_data->num_rows; i++) {
      new_rows[i] = new_data->rows[i];
      new_data->rows[i].cells = NULL;
      new_data->rows[i].num_cells = 0;
    }

    /* Then copy old rows */
    for (size_t i = 0; i < old_count; i++) {
      new_rows[new_data->num_rows + i] = state->data->rows[i];
    }

    free(state->data->rows);
    state->data->rows = new_rows;
    state->data->num_rows = new_count;

    /* Get current cursor/scroll from VmTable if available */
    size_t cursor_row = state->cursor_row;
    size_t scroll_row = state->scroll_row;
    VmTable *vm = get_vm_table(state);
    if (vm) {
      vm_table_get_cursor(vm, &cursor_row, NULL);
      vm_table_get_scroll(vm, &scroll_row, NULL);
    }

    /* Adjust cursor and scroll positions */
    cursor_row += new_data->num_rows;
    scroll_row += new_data->num_rows;

    /* Update via viewmodel if available */
    if (vm) {
      size_t cursor_col, scroll_col;
      vm_table_get_cursor(vm, NULL, &cursor_col);
      vm_table_get_scroll(vm, NULL, &scroll_col);
      vm_table_set_cursor(vm, cursor_row, cursor_col);
      vm_table_set_scroll(vm, scroll_row, scroll_col);
    }

    /* Sync to compatibility layer */
    state->cursor_row = cursor_row;
    state->scroll_row = scroll_row;

    /* Update offset */
    state->loaded_offset -= new_data->num_rows;
    state->loaded_count = new_count;
  }

  /* Update tab pointers */
  Tab *tab = TUI_TAB(state);
  if (tab) {
    tab->data = state->data;
    tab->loaded_offset = state->loaded_offset;
    tab->loaded_count = state->loaded_count;
  }

  return true;
}

/* Load rows at specific offset with blocking dialog (for goto/home/end) */
bool tui_load_rows_at_with_dialog(TuiState *state, size_t offset) {
  if (!state || !state->conn || !state->tables)
    return false;
  if (state->current_table >= state->num_tables)
    return false;

  /* Cancel any pending background load first */
  tui_cancel_background_load(state);

  const char *table = state->tables[state->current_table];

  /* Check if we're using approximate count */
  bool was_approximate = false;
  Tab *tab = TUI_TAB(state);
  if (tab) {
    was_approximate = tab->row_count_approximate;
  }

  /* Clamp offset */
  if (offset >= state->total_rows) {
    offset = state->total_rows > PAGE_SIZE ? state->total_rows - PAGE_SIZE : 0;
  }

  /* Build WHERE clause from filters */
  char *where_clause = build_filter_where(state);
  char *order_clause = build_order_clause(state);

  /* Setup async operation */
  AsyncOperation op;
  async_init(&op);
  op.conn = state->conn;
  op.table_name = str_dup(table);
  op.offset = offset;
  op.limit = PAGE_SIZE * PREFETCH_PAGES;
  op.order_by = order_clause; /* Takes ownership */
  op.desc = false;

  if (where_clause) {
    op.op_type = ASYNC_OP_QUERY_PAGE_WHERE;
    op.where_clause = str_dup(where_clause);
  } else {
    op.op_type = ASYNC_OP_QUERY_PAGE;
  }
  free(where_clause);

  if (!op.table_name || !async_start(&op)) {
    async_free(&op);
    return false;
  }

  /* Show blocking dialog */
  bool completed = tui_show_processing_dialog(state, &op, "Loading data...");

  bool success = false;
  if (completed && op.state == ASYNC_STATE_COMPLETED && op.result) {
    ResultSet *new_data = (ResultSet *)op.result;

    /* If we got 0 rows and were using approximate count, get exact count and
     * retry */
    if (new_data->num_rows == 0 && was_approximate && offset > 0) {
      db_result_free(new_data);
      async_free(&op);

      /* Get EXACT count with progress dialog (approximate was wrong) */
      AsyncOperation count_op;
      async_init(&count_op);
      count_op.op_type = ASYNC_OP_COUNT_ROWS;
      count_op.conn = state->conn;
      count_op.table_name = str_dup(table);
      count_op.use_approximate = false; /* Force exact count */

      if (!count_op.table_name || !async_start(&count_op)) {
        async_free(&count_op);
        tui_set_error(state, "Failed to start count operation");
        return false;
      }

      bool count_completed = tui_show_processing_dialog(
          state, &count_op, "Counting rows (exact)...");
      int64_t exact_count = -1;
      if (count_completed && count_op.state == ASYNC_STATE_COMPLETED) {
        exact_count = count_op.count;
      } else if (count_op.state == ASYNC_STATE_CANCELLED) {
        async_free(&count_op);
        tui_set_status(state, "Count cancelled");
        return false;
      }
      async_free(&count_op);

      if (exact_count > 0) {
        /* Update total_rows with exact count */
        state->total_rows = (size_t)exact_count;
        if (tab) {
          tab->total_rows = state->total_rows;
          tab->row_count_approximate = false;
        }

        /* Recalculate offset and retry */
        size_t new_offset = (size_t)exact_count > PAGE_SIZE
                                ? (size_t)exact_count - PAGE_SIZE
                                : 0;

        /* Refresh screen before next dialog */
        touchwin(stdscr);
        tui_refresh(state);

        return tui_load_rows_at_with_dialog(state, new_offset);
      }
      tui_set_error(state, "Could not determine row count");
      return false;
    }

    /* Apply schema column names */
    if (state->schema && new_data) {
      size_t min_cols = state->schema->num_columns;
      if (new_data->num_columns < min_cols) {
        min_cols = new_data->num_columns;
      }
      for (size_t i = 0; i < min_cols; i++) {
        if (state->schema->columns[i].name) {
          free(new_data->columns[i].name);
          new_data->columns[i].name = str_dup(state->schema->columns[i].name);
          new_data->columns[i].type = state->schema->columns[i].type;
        }
      }
    }

    /* Free old data and replace */
    if (state->data) {
      db_result_free(state->data);
    }
    state->data = new_data;
    state->loaded_offset = offset;
    state->loaded_count = new_data->num_rows;

    /* Update tab pointers */
    if (tab) {
      tab->data = state->data;
      tab->loaded_offset = state->loaded_offset;
      tab->loaded_count = state->loaded_count;
    }

    success = true;
  } else if (op.state == ASYNC_STATE_CANCELLED) {
    tui_set_status(state, "Load cancelled");
  } else if (op.state == ASYNC_STATE_ERROR) {
    tui_set_error(state, "Load failed: %s",
                  op.error ? op.error : "Unknown error");
  }

  async_free(&op);
  return success;
}

/* Load a page with blocking dialog (for fast scrolling past loaded data) */
bool tui_load_page_with_dialog(TuiState *state, bool forward) {
  if (!state || !state->conn || !state->tables)
    return false;
  if (state->current_table >= state->num_tables)
    return false;

  Tab *tab = TUI_TAB(state);
  if (!tab)
    return false;

  /* Check if a background load is already running in the same direction */
  if (tab->bg_load_op != NULL && tab->bg_load_forward == forward) {
    AsyncOperation *bg_op = (AsyncOperation *)tab->bg_load_op;

    /* Show progress dialog and wait for existing operation */
    bool completed =
        tui_show_processing_dialog(state, bg_op, "Loading data...");

    bool success = false;
    if (completed && bg_op->state == ASYNC_STATE_COMPLETED && bg_op->result) {
      ResultSet *new_data = (ResultSet *)bg_op->result;

      /* Apply schema column names */
      if (state->schema && new_data) {
        size_t min_cols = state->schema->num_columns;
        if (new_data->num_columns < min_cols) {
          min_cols = new_data->num_columns;
        }
        for (size_t i = 0; i < min_cols; i++) {
          if (state->schema->columns[i].name) {
            free(new_data->columns[i].name);
            new_data->columns[i].name = str_dup(state->schema->columns[i].name);
            new_data->columns[i].type = state->schema->columns[i].type;
          }
        }
      }

      /* Merge into existing data */
      success = merge_page_result(state, new_data, forward);
      if (success) {
        tui_trim_loaded_data(state);
        tui_set_status(state, "Loaded %zu/%zu rows", state->loaded_count,
                       state->total_rows);
      }

      db_result_free(new_data);
    } else if (bg_op->state == ASYNC_STATE_CANCELLED) {
      tui_set_status(state, "Load cancelled");
    } else if (bg_op->state == ASYNC_STATE_ERROR) {
      tui_set_error(state, "Load failed: %s",
                    bg_op->error ? bg_op->error : "Unknown error");
    }

    /* Clean up background operation */
    async_free(bg_op);
    free(bg_op);
    tab->bg_load_op = NULL;
    state->bg_loading_active = false;

    return success;
  }

  /* No compatible background load - cancel any existing and start new */
  tui_cancel_background_load(state);

  const char *table = state->tables[state->current_table];

  /* Calculate target offset */
  size_t target_offset;
  if (forward) {
    target_offset = state->loaded_offset + state->loaded_count;
    /* Check if there are more rows */
    if (target_offset >= state->total_rows)
      return false;
  } else {
    if (state->loaded_offset == 0)
      return false; /* Already at beginning */
    target_offset = state->loaded_offset >= PAGE_SIZE
                        ? state->loaded_offset - PAGE_SIZE
                        : 0;
  }

  /* Build WHERE clause from filters */
  char *where_clause = build_filter_where(state);
  char *order_clause = build_order_clause(state);

  /* Setup async operation */
  AsyncOperation op;
  async_init(&op);
  op.conn = state->conn;
  op.table_name = str_dup(table);
  op.offset = target_offset;
  op.limit = PAGE_SIZE * PREFETCH_PAGES;
  op.order_by = order_clause; /* Takes ownership */
  op.desc = false;

  if (where_clause) {
    op.op_type = ASYNC_OP_QUERY_PAGE_WHERE;
    op.where_clause = str_dup(where_clause);
  } else {
    op.op_type = ASYNC_OP_QUERY_PAGE;
  }
  free(where_clause);

  if (!op.table_name || !async_start(&op)) {
    async_free(&op);
    return false;
  }

  /* Show blocking dialog - same as table open */
  bool completed = tui_show_processing_dialog(state, &op, "Loading data...");

  bool success = false;
  if (completed && op.state == ASYNC_STATE_COMPLETED && op.result) {
    ResultSet *new_data = (ResultSet *)op.result;

    /* Apply schema column names */
    if (state->schema && new_data) {
      size_t min_cols = state->schema->num_columns;
      if (new_data->num_columns < min_cols) {
        min_cols = new_data->num_columns;
      }
      for (size_t i = 0; i < min_cols; i++) {
        if (state->schema->columns[i].name) {
          free(new_data->columns[i].name);
          new_data->columns[i].name = str_dup(state->schema->columns[i].name);
          new_data->columns[i].type = state->schema->columns[i].type;
        }
      }
    }

    /* Merge into existing data */
    success = merge_page_result(state, new_data, forward);
    if (success) {
      /* Trim old data to keep memory bounded */
      tui_trim_loaded_data(state);
      tui_set_status(state, "Loaded %zu/%zu rows", state->loaded_count,
                     state->total_rows);
    }

    /* Free the result set structure (cells were moved) */
    db_result_free(new_data);
  } else if (op.state == ASYNC_STATE_CANCELLED) {
    tui_set_status(state, "Load cancelled");
  } else if (op.state == ASYNC_STATE_ERROR) {
    tui_set_error(state, "Load failed: %s",
                  op.error ? op.error : "Unknown error");
  }

  async_free(&op);
  return success;
}

/* ============================================================================
 * Background prefetch (non-blocking)
 * ============================================================================
 */

/* Start background load (non-blocking) - returns true if started */
bool tui_start_background_load(TuiState *state, bool forward) {
  if (!state || !state->conn || !state->tables)
    return false;
  if (state->current_table >= state->num_tables)
    return false;

  Tab *tab = TUI_TAB(state);
  if (!tab)
    return false;

  /* Already have a background load in progress */
  if (tab->bg_load_op != NULL)
    return false;

  const char *table = state->tables[state->current_table];

  /* Calculate target offset */
  size_t target_offset;
  if (forward) {
    target_offset = state->loaded_offset + state->loaded_count;
    if (target_offset >= state->total_rows)
      return false; /* No more data */
  } else {
    if (state->loaded_offset == 0)
      return false; /* Already at beginning */
    target_offset = state->loaded_offset >= PAGE_SIZE
                        ? state->loaded_offset - PAGE_SIZE
                        : 0;
  }

  /* Build WHERE clause from filters */
  char *where_clause = build_filter_where(state);
  char *order_clause = build_order_clause(state);

  /* Allocate and setup async operation */
  AsyncOperation *op = malloc(sizeof(AsyncOperation));
  if (!op) {
    free(where_clause);
    free(order_clause);
    return false;
  }

  async_init(op);
  op->conn = state->conn;
  op->table_name = str_dup(table);
  op->offset = target_offset;
  op->limit = PAGE_SIZE * PREFETCH_PAGES;
  op->order_by = order_clause; /* Takes ownership */
  op->desc = false;

  if (where_clause) {
    op->op_type = ASYNC_OP_QUERY_PAGE_WHERE;
    op->where_clause = str_dup(where_clause);
  } else {
    op->op_type = ASYNC_OP_QUERY_PAGE;
  }
  free(where_clause);

  if (!op->table_name || !async_start(op)) {
    async_free(op);
    free(op);
    return false;
  }

  /* Store in tab */
  tab->bg_load_op = op;
  tab->bg_load_forward = forward;
  tab->bg_load_target_offset = target_offset;
  state->bg_loading_active = true;

  return true;
}

/* Poll background load, merge if complete - call from main loop */
bool tui_poll_background_load(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return false;

  AsyncOperation *op = (AsyncOperation *)tab->bg_load_op;

  if (!op)
    return false;

  AsyncState op_state = async_poll(op);

  if (op_state == ASYNC_STATE_RUNNING) {
    return true; /* Still running */
  }

  /* Operation completed (success, error, or cancelled) */
  bool merged = false;

  if (op_state == ASYNC_STATE_COMPLETED && op->result) {
    ResultSet *new_data = (ResultSet *)op->result;

    /* Apply schema column names */
    if (state->schema && new_data) {
      size_t min_cols = state->schema->num_columns;
      if (new_data->num_columns < min_cols) {
        min_cols = new_data->num_columns;
      }
      for (size_t i = 0; i < min_cols; i++) {
        if (state->schema->columns[i].name) {
          free(new_data->columns[i].name);
          new_data->columns[i].name = str_dup(state->schema->columns[i].name);
          new_data->columns[i].type = state->schema->columns[i].type;
        }
      }
    }

    /* Merge into existing data */
    merged = merge_page_result(state, new_data, tab->bg_load_forward);
    if (merged) {
      tui_trim_loaded_data(state);
    }

    db_result_free(new_data);
  }

  /* Clean up */
  async_free(op);
  free(op);
  tab->bg_load_op = NULL;
  state->bg_loading_active = false;

  return merged; /* Return true if we merged data (need redraw) */
}

/* Cancel pending background load */
void tui_cancel_background_load(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;

  AsyncOperation *op = (AsyncOperation *)tab->bg_load_op;

  if (!op)
    return;

  /* Request cancellation */
  async_cancel(op);

  /* Wait for operation to actually complete/cancel - important for connection
   * safety */
  /* PostgreSQL connections can't be used concurrently, so we must wait */
  async_wait(op, 500); /* Wait up to 500ms for query to cancel */

  /* If still running after wait, poll until done (shouldn't happen often) */
  while (async_poll(op) == ASYNC_STATE_RUNNING) {
    struct timespec ts = {0, 10000000L}; /* 10ms */
    nanosleep(&ts, NULL);
  }

  /* Free result if any - worker may have already freed on cancel, check under mutex */
  lace_mutex_lock(&op->mutex);
  if (op->result) {
    db_result_free((ResultSet *)op->result);
    op->result = NULL;
  }
  lace_mutex_unlock(&op->mutex);

  async_free(op);
  free(op);
  tab->bg_load_op = NULL;
  state->bg_loading_active = false;
}

/* Check if speculative prefetch should start */
void tui_check_speculative_prefetch(TuiState *state) {
  if (!state || !state->data)
    return;

  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;

  /* Skip if background load already in progress */
  if (tab->bg_load_op != NULL)
    return;

  /* Skip if we're in a special tab type */
  if (tab->type != TAB_TYPE_TABLE)
    return;

  /* Get cursor position from VmTable if available */
  size_t cursor_row = state->cursor_row;
  VmTable *vm = get_vm_table(state);
  if (vm) {
    vm_table_get_cursor(vm, &cursor_row, NULL);
  }

  /* Calculate distance from edges */
  size_t rows_from_end = state->data->num_rows > cursor_row
                             ? state->data->num_rows - cursor_row
                             : 0;
  size_t rows_from_start = cursor_row;

  /* Prefetch forward when within PREFETCH_THRESHOLD of end */
  size_t loaded_end = state->loaded_offset + state->loaded_count;
  if (rows_from_end < PREFETCH_THRESHOLD && loaded_end < state->total_rows) {
    tui_start_background_load(state, true);
    return;
  }

  /* Prefetch backward when within PREFETCH_THRESHOLD of start */
  if (rows_from_start < PREFETCH_THRESHOLD && state->loaded_offset > 0) {
    tui_start_background_load(state, false);
  }
}
