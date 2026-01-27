/*
 * Lace
 * Pagination and data loading
 *
 * Simplified pagination without blocking dialogs.
 * Uses status bar for loading feedback.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "async/async.h"
#include "config/config.h"
#include "../../liblace/include/constants.h"
#include "../../liblace/include/keyset.h"
#include "../../liblace/include/util/mem.h"
#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/* Calculate column widths based on data */
void tui_calculate_column_widths(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  ResultSet *data = tab ? tab->data : NULL;
  if (!data)
    return;

  if (!data->columns || data->num_columns == 0)
    return;

  free(tab->col_widths);
  tab->num_col_widths = data->num_columns;
  tab->col_widths = safe_calloc(tab->num_col_widths, sizeof(int));

  /* Start with column name widths */
  for (size_t i = 0; i < data->num_columns; i++) {
    const char *name = data->columns[i].name;
    int len = name ? (int)strlen(name) : 0;
    tab->col_widths[i] = len < MIN_COL_WIDTH ? MIN_COL_WIDTH : len;
  }

  /* Check data widths - sample first 100 rows */
  for (size_t row = 0; row < data->num_rows && row < 100; row++) {
    Row *r = &data->rows[row];
    if (!r->cells)
      continue;
    for (size_t col = 0; col < data->num_columns && col < r->num_cells; col++) {
      char *str = db_value_to_string(&r->cells[col]);
      if (str) {
        int len = (int)strlen(str);
        if (len > tab->col_widths[col]) {
          tab->col_widths[col] = len;
        }
        free(str);
      }
    }
  }

  /* Apply max width */
  for (size_t i = 0; i < tab->num_col_widths; i++) {
    if (tab->col_widths[i] > DEFAULT_COL_WIDTH) {
      tab->col_widths[i] = DEFAULT_COL_WIDTH;
    }
  }
}

int tui_get_column_width(TuiState *state, size_t col) {
  Tab *tab = TUI_TAB(state);
  if (!tab || !tab->col_widths || col >= tab->num_col_widths) {
    return DEFAULT_COL_WIDTH;
  }
  return tab->col_widths[col];
}

/* Build WHERE clause for current tab filters */
static char *build_filter_where(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->filters.num_filters == 0)
    return NULL;

  DbConnection *conn = TUI_CONN(state);
  if (!tab->schema || !conn)
    return NULL;

  char *err = NULL;
  char *where =
      filters_build_where(&tab->filters, tab->schema, conn->driver->name, &err);
  free(err);
  return where;
}

/* Build ORDER BY clause for current tab */
static char *build_order_clause(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->num_sort_entries == 0)
    return NULL;

  DbConnection *conn = TUI_CONN(state);
  if (!tab->schema || !conn)
    return NULL;

  bool use_backtick = conn->driver &&
                      (strcmp(conn->driver->name, "mysql") == 0 ||
                       strcmp(conn->driver->name, "mariadb") == 0);

  StringBuilder *sb = sb_new(128);
  if (!sb)
    return NULL;

  bool first = true;
  for (size_t i = 0; i < tab->num_sort_entries; i++) {
    SortEntry *entry = &tab->sort_entries[i];
    if (entry->column >= tab->schema->num_columns)
      continue;

    const char *col_name = tab->schema->columns[entry->column].name;
    if (!col_name)
      continue;

    char *escaped = use_backtick ? str_escape_identifier_backtick(col_name)
                                 : str_escape_identifier_dquote(col_name);
    if (!escaped) {
      sb_free(sb);
      return NULL;
    }

    if (!first)
      sb_append(sb, ", ");
    first = false;
    sb_printf(sb, "%s %s", escaped, entry->direction == SORT_ASC ? "ASC" : "DESC");
    free(escaped);
  }

  if (first) {
    sb_free(sb);
    return NULL;
  }

  return sb_to_string(sb);
}

/* Apply schema column names to result set */
static void apply_schema_columns(Tab *tab, ResultSet *data) {
  if (!tab->schema || !data)
    return;

  size_t min_cols = tab->schema->num_columns < data->num_columns
                        ? tab->schema->num_columns
                        : data->num_columns;

  for (size_t i = 0; i < min_cols; i++) {
    if (tab->schema->columns[i].name) {
      free(data->columns[i].name);
      data->columns[i].name = str_dup(tab->schema->columns[i].name);
      data->columns[i].type = tab->schema->columns[i].type;
    }
  }
}

/* ============================================================================
 * Initial Table Load (uses dialog - explicit user action)
 * ============================================================================ */

bool tui_load_table_data(TuiState *state, const char *table) {
  DbConnection *conn = TUI_CONN(state);
  if (!state || !conn || !table)
    return false;

  Tab *tab = TUI_TAB(state);
  if (!tab)
    return false;

  /* Clear previous data */
  free(tab->table_error);
  tab->table_error = NULL;

  if (tab->data) {
    db_result_free(tab->data);
    tab->data = NULL;
  }
  if (tab->schema) {
    db_schema_free(tab->schema);
    tab->schema = NULL;
  }
  if (tab->keyset) {
    lace_keyset_free(tab->keyset);
    tab->keyset = NULL;
  }

  /* Load schema */
  AsyncOperation schema_op;
  async_init(&schema_op);
  schema_op.op_type = ASYNC_OP_GET_SCHEMA;
  schema_op.conn = conn;
  schema_op.table_name = str_dup(table);

  if (schema_op.table_name && async_start(&schema_op)) {
    bool completed = tui_show_processing_dialog(state, &schema_op, "Loading schema...");
    if (completed && schema_op.state == ASYNC_STATE_COMPLETED) {
      tab->schema = (TableSchema *)schema_op.result;
    } else if (schema_op.state == ASYNC_STATE_CANCELLED) {
      async_free(&schema_op);
      tui_set_status(state, "Cancelled");
      return false;
    }
  }
  async_free(&schema_op);

  /* Build WHERE clause from filters */
  char *where_clause = build_filter_where(state);

  /* Get row count */
  AsyncOperation count_op;
  async_init(&count_op);
  count_op.conn = conn;
  count_op.table_name = str_dup(table);

  int64_t count = 0;
  bool is_approximate = false;

  if (where_clause) {
    count_op.op_type = ASYNC_OP_COUNT_ROWS_WHERE;
    count_op.where_clause = str_dup(where_clause);
  } else {
    count_op.op_type = ASYNC_OP_COUNT_ROWS;
    count_op.use_approximate = true;
  }

  if (count_op.table_name && async_start(&count_op)) {
    bool completed = tui_show_processing_dialog(state, &count_op, "Counting rows...");
    if (completed && count_op.state == ASYNC_STATE_COMPLETED) {
      count = count_op.count;
      is_approximate = count_op.is_approximate;
    } else if (count_op.state == ASYNC_STATE_CANCELLED) {
      async_free(&count_op);
      free(where_clause);
      tui_set_status(state, "Cancelled");
      return false;
    }
  }
  async_free(&count_op);

  tab->total_rows = count >= 0 ? (size_t)count : 0;
  tab->loaded_offset = 0;
  tab->row_count_approximate = is_approximate;

  if (!where_clause) {
    tab->unfiltered_total_rows = tab->total_rows;
  }

  /* Load first page */
  AsyncOperation data_op;
  async_init(&data_op);
  data_op.conn = conn;
  data_op.table_name = str_dup(table);
  data_op.offset = 0;
  data_op.limit = PAGE_SIZE * PREFETCH_PAGES;
  data_op.order_by = build_order_clause(state);

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

  bool completed = tui_show_processing_dialog(state, &data_op, "Loading data...");

  if (!completed || data_op.state == ASYNC_STATE_CANCELLED) {
    async_free(&data_op);
    tui_set_status(state, "Cancelled");
    return false;
  }

  if (data_op.state == ASYNC_STATE_ERROR) {
    const char *err = data_op.error ? data_op.error : "Unknown error";
    tui_set_error(state, "Query failed: %s", err);
    tab->table_error = str_dup(err);
    async_free(&data_op);
    return false;
  }

  tab->data = (ResultSet *)data_op.result;
  async_free(&data_op);

  if (!tab->data) {
    tui_set_error(state, "No data returned");
    return false;
  }

  tab->loaded_count = tab->data->num_rows;
  apply_schema_columns(tab, tab->data);

  /* Reset cursor */
  tab->cursor_row = 0;
  tab->cursor_col = 0;
  tab->scroll_row = 0;
  tab->scroll_col = 0;

  tui_calculate_column_widths(state);

  free(state->status_msg);
  state->status_msg = NULL;
  state->status_is_error = false;

  /* Bind VmTable */
  if (tab->type == TAB_TYPE_TABLE) {
    if (!state->vm_table) {
      state->vm_table = table_vm_create(state->app, tab);
    } else {
      table_vm_bind(state->vm_table, tab);
    }
  }

  return true;
}

/* ============================================================================
 * Synchronous Page Loading (no dialog, just status bar)
 * ============================================================================ */

/* Load a page synchronously without dialog - for navigation past boundaries */
bool tui_load_page_with_dialog(TuiState *state, bool forward) {
  Tab *tab = TUI_TAB(state);
  DbConnection *conn = TUI_CONN(state);
  if (!tab || !conn || !tab->table_name || !tab->data)
    return false;

  /* Cancel any background load first */
  tui_cancel_background_load(state);

  /* Calculate target offset */
  size_t target_offset;
  if (forward) {
    target_offset = tab->loaded_offset + tab->loaded_count;
    if (target_offset >= tab->total_rows)
      return false;
  } else {
    if (tab->loaded_offset == 0)
      return false;
    target_offset = tab->loaded_offset > PAGE_SIZE
                        ? tab->loaded_offset - PAGE_SIZE
                        : 0;
  }

  tui_set_status(state, "Loading...");
  tui_refresh(state);

  /* Build clauses */
  char *where_clause = build_filter_where(state);
  char *order_clause = build_order_clause(state);

  /* Execute query synchronously */
  char *err = NULL;
  ResultSet *new_data;
  size_t load_count = forward ? PAGE_SIZE : (tab->loaded_offset - target_offset);

  if (where_clause) {
    new_data = db_query_page_where(conn, tab->table_name, target_offset,
                                   load_count, where_clause, order_clause, false, &err);
  } else {
    new_data = db_query_page(conn, tab->table_name, target_offset,
                             load_count, order_clause, false, &err);
  }
  free(where_clause);
  free(order_clause);

  if (!new_data || new_data->num_rows == 0) {
    if (new_data)
      db_result_free(new_data);
    free(err);
    tui_set_status(state, "No more data");
    return false;
  }

  /* Merge new data */
  size_t old_count = tab->data->num_rows;
  size_t new_count = old_count + new_data->num_rows;

  if (new_count > 1000000) {
    db_result_free(new_data);
    tui_set_error(state, "Too much data loaded");
    return false;
  }

  if (forward) {
    /* Append */
    tab->data->rows = safe_reallocarray(tab->data->rows, new_count, sizeof(Row));
    for (size_t i = 0; i < new_data->num_rows; i++) {
      tab->data->rows[old_count + i] = new_data->rows[i];
      new_data->rows[i].cells = NULL;
      new_data->rows[i].num_cells = 0;
    }
    tab->data->num_rows = new_count;
    tab->loaded_count = new_count;
  } else {
    /* Prepend */
    Row *merged = safe_reallocarray(NULL, new_count, sizeof(Row));
    for (size_t i = 0; i < new_data->num_rows; i++) {
      merged[i] = new_data->rows[i];
      new_data->rows[i].cells = NULL;
      new_data->rows[i].num_cells = 0;
    }
    for (size_t i = 0; i < old_count; i++) {
      merged[new_data->num_rows + i] = tab->data->rows[i];
    }
    free(tab->data->rows);
    tab->data->rows = merged;
    tab->data->num_rows = new_count;

    /* Adjust cursor and scroll */
    tab->cursor_row += new_data->num_rows;
    tab->scroll_row += new_data->num_rows;
    tab->loaded_offset = target_offset;
    tab->loaded_count = new_count;

    VmTable *vm = tui_vm_table(state);
    if (vm) {
      table_vm_set_cursor(vm, tab->cursor_row, tab->cursor_col);
      table_vm_set_scroll(vm, tab->scroll_row, tab->scroll_col);
    }
  }

  db_result_free(new_data);
  tui_trim_loaded_data(state);
  tui_set_status(state, "Loaded %zu/%zu rows", tab->loaded_count, tab->total_rows);

  return true;
}

/* Load rows at specific offset (replaces current data) */
bool tui_load_rows_at(TuiState *state, size_t offset) {
  Tab *tab = TUI_TAB(state);
  DbConnection *conn = TUI_CONN(state);
  if (!tab || !conn || !tab->table_name)
    return false;

  if (offset >= tab->total_rows && tab->total_rows > 0) {
    offset = tab->total_rows > PAGE_SIZE ? tab->total_rows - PAGE_SIZE : 0;
  }

  char *where_clause = build_filter_where(state);
  char *order_clause = build_order_clause(state);

  char *err = NULL;
  ResultSet *data;
  if (where_clause) {
    data = db_query_page_where(conn, tab->table_name, offset, PAGE_SIZE,
                               where_clause, order_clause, false, &err);
  } else {
    data = db_query_page(conn, tab->table_name, offset, PAGE_SIZE,
                         order_clause, false, &err);
  }
  free(where_clause);
  free(order_clause);

  if (!data) {
    tui_set_error(state, "Query failed: %s", err ? err : "Unknown error");
    free(err);
    return false;
  }

  if (tab->data)
    db_result_free(tab->data);

  tab->data = data;
  tab->loaded_offset = offset;
  tab->loaded_count = data->num_rows;
  apply_schema_columns(tab, tab->data);

  return true;
}

/* Load rows at offset with status bar feedback */
bool tui_load_rows_at_with_dialog(TuiState *state, size_t offset) {
  tui_cancel_background_load(state);
  tui_set_status(state, "Loading...");
  tui_refresh(state);

  bool result = tui_load_rows_at(state, offset);

  if (result) {
    Tab *tab = TUI_TAB(state);
    tui_set_status(state, "Loaded %zu/%zu rows", tab->loaded_count, tab->total_rows);
  }
  return result;
}

/* ============================================================================
 * Refresh and Utility
 * ============================================================================ */

bool tui_refresh_table(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE || !tab->table_name)
    return false;

  DbConnection *conn = TUI_CONN(state);
  if (!conn)
    return false;

  tui_cancel_background_load(state);

  /* Save position */
  size_t saved_cursor_row = tab->cursor_row;
  size_t saved_cursor_col = tab->cursor_col;
  size_t saved_scroll_row = tab->scroll_row;
  size_t saved_scroll_col = tab->scroll_col;
  size_t abs_row = tab->loaded_offset + saved_cursor_row;

  if (!tui_load_table_data(state, tab->table_name))
    return false;

  /* Restore position */
  ResultSet *data = tab->data;
  if (data && data->num_rows > 0) {
    if (abs_row >= tab->total_rows && tab->total_rows > 0)
      abs_row = tab->total_rows - 1;

    size_t target_offset = (abs_row / PAGE_SIZE) * PAGE_SIZE;
    if (target_offset > 0 && target_offset != tab->loaded_offset) {
      tui_load_rows_at_with_dialog(state, target_offset);
      data = tab->data;
    }

    size_t local_cursor = abs_row >= tab->loaded_offset
                              ? abs_row - tab->loaded_offset
                              : 0;
    if (data && local_cursor >= data->num_rows)
      local_cursor = data->num_rows > 0 ? data->num_rows - 1 : 0;

    tab->cursor_row = local_cursor;
    tab->cursor_col = data && saved_cursor_col < data->num_columns
                          ? saved_cursor_col
                          : 0;

    size_t visible = state->content_rows > 0 ? (size_t)state->content_rows : 1;
    if (saved_cursor_row >= saved_scroll_row) {
      size_t screen_off = saved_cursor_row - saved_scroll_row;
      tab->scroll_row = local_cursor >= screen_off ? local_cursor - screen_off : 0;
    } else {
      tab->scroll_row = local_cursor;
    }

    size_t max_scroll = data && data->num_rows > visible ? data->num_rows - visible : 0;
    if (tab->scroll_row > max_scroll)
      tab->scroll_row = max_scroll;

    tab->scroll_col = saved_scroll_col;
  }

  tui_set_status(state, "Refreshed (%zu rows)", tab->total_rows);
  return true;
}

void tui_trim_loaded_data(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || !tab->data || tab->data->num_rows == 0)
    return;

  size_t max_rows = MAX_LOADED_PAGES * PAGE_SIZE;
  if (tab->loaded_count <= max_rows)
    return;

  size_t cursor_row = tab->cursor_row;
  size_t scroll_row = tab->scroll_row;
  size_t cursor_page = cursor_row / PAGE_SIZE;
  size_t total_pages = (tab->loaded_count + PAGE_SIZE - 1) / PAGE_SIZE;

  size_t keep_start = cursor_page > TRIM_DISTANCE_PAGES
                          ? cursor_page - TRIM_DISTANCE_PAGES
                          : 0;
  size_t keep_end = cursor_page + TRIM_DISTANCE_PAGES + 1 < total_pages
                        ? cursor_page + TRIM_DISTANCE_PAGES + 1
                        : total_pages;

  if (keep_end - keep_start > MAX_LOADED_PAGES) {
    size_t excess = (keep_end - keep_start) - MAX_LOADED_PAGES;
    if (cursor_page - keep_start > keep_end - cursor_page - 1)
      keep_start += excess;
    else
      keep_end -= excess;
  }

  size_t trim_start = keep_start * PAGE_SIZE;
  size_t trim_end = keep_end * PAGE_SIZE;
  if (trim_end > tab->loaded_count)
    trim_end = tab->loaded_count;

  if (trim_start == 0 && trim_end >= tab->loaded_count)
    return;

  /* Free trimmed rows */
  for (size_t i = 0; i < trim_start; i++) {
    Row *row = &tab->data->rows[i];
    for (size_t j = 0; j < row->num_cells; j++)
      db_value_free(&row->cells[j]);
    free(row->cells);
  }
  for (size_t i = trim_end; i < tab->loaded_count; i++) {
    Row *row = &tab->data->rows[i];
    for (size_t j = 0; j < row->num_cells; j++)
      db_value_free(&row->cells[j]);
    free(row->cells);
  }

  size_t new_count = trim_end - trim_start;
  if (trim_start > 0)
    memmove(tab->data->rows, tab->data->rows + trim_start, new_count * sizeof(Row));

  tab->data->rows = safe_reallocarray(tab->data->rows, new_count, sizeof(Row));
  tab->data->num_rows = new_count;

  if (cursor_row >= trim_start)
    cursor_row -= trim_start;
  else
    cursor_row = 0;

  if (scroll_row >= trim_start)
    scroll_row -= trim_start;
  else
    scroll_row = 0;

  tab->cursor_row = cursor_row;
  tab->scroll_row = scroll_row;
  tab->loaded_offset += trim_start;
  tab->loaded_count = new_count;

  VmTable *vm = tui_vm_table(state);
  if (vm) {
    table_vm_set_cursor(vm, cursor_row, tab->cursor_col);
    table_vm_set_scroll(vm, scroll_row, tab->scroll_col);
  }
}

/* ============================================================================
 * Background Prefetch (simple version)
 * ============================================================================ */

bool tui_start_background_load(TuiState *state, bool forward) {
  Tab *tab = TUI_TAB(state);
  DbConnection *conn = TUI_CONN(state);
  if (!tab || !conn || !tab->table_name || !tab->data)
    return false;

  /* Already have a background load */
  if (tab->bg_load_op)
    return false;

  /* Check boundaries */
  size_t target_offset;
  if (forward) {
    target_offset = tab->loaded_offset + tab->loaded_count;
    if (target_offset >= tab->total_rows)
      return false;
  } else {
    if (tab->loaded_offset == 0)
      return false;
    target_offset = tab->loaded_offset > PAGE_SIZE
                        ? tab->loaded_offset - PAGE_SIZE
                        : 0;
  }

  /* Build clauses */
  char *where_clause = build_filter_where(state);
  char *order_clause = build_order_clause(state);

  /* Setup operation */
  AsyncOperation *op = safe_calloc(1, sizeof(AsyncOperation));
  async_init(op);
  op->conn = conn;
  op->table_name = str_dup(tab->table_name);
  op->offset = target_offset;
  op->limit = PAGE_SIZE;
  op->order_by = order_clause;

  if (where_clause) {
    op->op_type = ASYNC_OP_QUERY_PAGE_WHERE;
    op->where_clause = where_clause;
  } else {
    op->op_type = ASYNC_OP_QUERY_PAGE;
  }

  if (!async_start(op)) {
    async_free(op);
    free(op);
    return false;
  }

  tab->bg_load_op = op;
  tab->bg_load_forward = forward;
  tab->bg_load_target_offset = target_offset;
  state->bg_loading_active = true;

  tui_set_status(state, "Loading...");
  return true;
}

BgPollResult tui_poll_background_load(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return BG_POLL_NONE;

  AsyncOperation *op = (AsyncOperation *)tab->bg_load_op;
  if (!op)
    return BG_POLL_NONE;

  AsyncState op_state = async_poll(op);

  if (op_state == ASYNC_STATE_RUNNING)
    return BG_POLL_RUNNING;

  BgPollResult result = BG_POLL_NONE;

  if (op_state == ASYNC_STATE_COMPLETED && op->result) {
    ResultSet *new_data = (ResultSet *)op->result;
    apply_schema_columns(tab, new_data);

    if (new_data->num_rows > 0 && tab->data) {
      size_t old_count = tab->data->num_rows;
      size_t new_count = old_count + new_data->num_rows;

      if (new_count <= 1000000) {
        if (tab->bg_load_forward) {
          /* Append */
          tab->data->rows = safe_reallocarray(tab->data->rows, new_count, sizeof(Row));
          for (size_t i = 0; i < new_data->num_rows; i++) {
            tab->data->rows[old_count + i] = new_data->rows[i];
            new_data->rows[i].cells = NULL;
            new_data->rows[i].num_cells = 0;
          }
          tab->data->num_rows = new_count;
          tab->loaded_count = new_count;
        } else {
          /* Prepend */
          Row *merged = safe_reallocarray(NULL, new_count, sizeof(Row));
          for (size_t i = 0; i < new_data->num_rows; i++) {
            merged[i] = new_data->rows[i];
            new_data->rows[i].cells = NULL;
            new_data->rows[i].num_cells = 0;
          }
          for (size_t i = 0; i < old_count; i++) {
            merged[new_data->num_rows + i] = tab->data->rows[i];
          }
          free(tab->data->rows);
          tab->data->rows = merged;
          tab->data->num_rows = new_count;

          tab->cursor_row += new_data->num_rows;
          tab->scroll_row += new_data->num_rows;
          tab->loaded_offset = tab->bg_load_target_offset;
          tab->loaded_count = new_count;

          VmTable *vm = tui_vm_table(state);
          if (vm) {
            table_vm_set_cursor(vm, tab->cursor_row, tab->cursor_col);
            table_vm_set_scroll(vm, tab->scroll_row, tab->scroll_col);
          }
        }

        tui_trim_loaded_data(state);
        tui_set_status(state, "%zu/%zu rows", tab->loaded_count, tab->total_rows);
        result = BG_POLL_MERGED;
      }
    }

    db_result_free(new_data);
  } else if (op_state == ASYNC_STATE_ERROR) {
    tui_set_status(state, "Load error");
    result = BG_POLL_ERROR;
  }

  /* Cleanup */
  async_free(op);
  free(op);
  tab->bg_load_op = NULL;
  state->bg_loading_active = false;

  return result;
}

void tui_cancel_background_load(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || !tab->bg_load_op)
    return;

  AsyncOperation *op = (AsyncOperation *)tab->bg_load_op;

  /* Request cancellation - this sends cancel to daemon */
  async_cancel(op);

  /* Wait for completion with timeout - can't free while worker is using op */
  int waits = 0;
  while (async_poll(op) == ASYNC_STATE_RUNNING && waits < 100) {
    async_wait(op, 50);
    waits++;
  }

  /* If still running after 5 seconds, something is wrong */
  if (async_poll(op) == ASYNC_STATE_RUNNING) {
    /* Leave op allocated - worker will eventually finish and leak memory,
     * but better than crashing. Clear our reference to avoid reuse. */
    tab->bg_load_op = NULL;
    state->bg_loading_active = false;
    tui_set_status(state, "Warning: background query not responding");
    return;
  }

  /* Free result if any */
  if (op->result) {
    db_result_free((ResultSet *)op->result);
    op->result = NULL;
  }

  async_free(op);
  free(op);
  tab->bg_load_op = NULL;
  state->bg_loading_active = false;
}

void tui_check_speculative_prefetch(TuiState *state) {
  /* Background prefetch disabled - causes stalls when canceling.
   * All loading is now synchronous via tui_load_page_with_dialog. */
  (void)state;
}

/* Legacy functions for compatibility */
bool tui_load_more_rows(TuiState *state) {
  return tui_load_page_with_dialog(state, true);
}

bool tui_load_prev_rows(TuiState *state) {
  return tui_load_page_with_dialog(state, false);
}

void tui_check_load_more(TuiState *state) {
  tui_check_speculative_prefetch(state);
}
