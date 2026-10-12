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
#include "core/constants.h"
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
    int len = name ? (int)utf8_width(name, strlen(name)) : 0;
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
        int len = (int)utf8_width(str, strlen(str));
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

/* The width the column is drawn with: the viewmodel's (re-measured over
 * every loaded window); Tab.col_widths (the first rows at load) only
 * before the table is bound. Editing (inline or modal) and horizontal
 * scrolling must agree with what is on screen. */
int tui_get_column_width(TuiState *state, size_t col) {
  VmTable *vm = tui_vm_table(state);
  if (vm && vm->col_widths && col < vm->num_col_widths && vm->col_widths[col] > 0)
    return vm->col_widths[col];
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

static bool keyset_paging_on(TuiState *state) {
  return state->app && state->app->config &&
         state->app->config->general.pagination_strategy != LACE_PAGINATION_OFFSET;
}

/* The keyset ordering of the current tab, or NULL to page with OFFSET */
static LaceKeysetState *tab_keyset(TuiState *state) {
  DbConnection *conn = TUI_CONN(state);
  return conn ? tab_page_keyset(TUI_TAB(state), db_conn_driver_name(conn),
                                keyset_paging_on(state))
              : NULL;
}

/* ORDER BY for every page of the current tab (see tab_page_order_by) */
static char *build_order_clause(TuiState *state) {
  DbConnection *conn = TUI_CONN(state);
  return conn ? tab_page_order_by(TUI_TAB(state), db_conn_driver_name(conn),
                                  keyset_paging_on(state))
              : NULL;
}

/* "(a) AND (b)", or whichever is set; takes ownership of both */
static char *and_where(char *a, char *b) {
  if (!a)
    return b;
  if (!b)
    return a;
  char *both = str_printf("(%s) AND (%s)", a, b);
  free(a);
  free(b);
  return both;
}

/* Apply schema column names to result set */
static void apply_schema_columns(Tab *tab, ResultSet *data) {
  if (!tab->schema || !data)
    return;

  /* Match by name: the result already carries the real column names, and
   * positions differ when the table has generated or hidden columns
   * (renaming by position put another column's name, e.g. the key's, on
   * a generated column, and edits went to the wrong row) */
  for (size_t i = 0; i < data->num_columns; i++) {
    const char *name = data->columns[i].name;
    for (size_t k = 0; name && k < tab->schema->num_columns; k++) {
      if (tab->schema->columns[k].name &&
          strcmp(tab->schema->columns[k].name, name) == 0) {
        if (tab->schema->columns[k].type != DB_TYPE_NULL)
          data->columns[i].type = tab->schema->columns[k].type;
        break;
      }
    }
  }
}

/* ============================================================================
 * Initial Table Load (uses dialog - explicit user action)
 * ============================================================================ */

bool tui_load_table_data(TuiState *state, const char *table) {
  /* A new row being added belongs to the table shown until now: its values
   * are sized and typed for that schema (a mouse click on another table
   * reaches here without the add-row keys seeing it) */
  if (state && state->adding_row) {
    tui_cancel_add_row(state);
  }
  DbConnection *conn = TUI_CONN(state);
  if (!state || !conn || !table)
    return false;

  Tab *tab = TUI_TAB(state);
  if (!tab)
    return false;

  /* Clear previous data */
  free(tab->table_error);
  tab->table_error = NULL;

  /* Free old data */
  tab_data_replaced(tab);
  if (tab->data) {
    db_result_free(tab->data);
    tab->data = NULL;
  }
  if (tab->schema) {
    db_schema_free(tab->schema);
    tab->schema = NULL;
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
  data_op.limit = TUI_PAGE_SIZE * PREFETCH_PAGES;
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
/* Load one page on a worker behind the processing dialog (spinner; Esc
 * cancels), so a slow page never freezes the UI. Takes ownership of
 * where_clause and order_clause. */
static ResultSet *load_page_modal(TuiState *state, DbConnection *conn,
                                  const char *table, size_t offset, size_t limit,
                                  char *where_clause, char *order_clause,
                                  bool reverse, char **err, bool *cancelled) {
  *err = NULL;
  *cancelled = false;
  AsyncOperation op;
  async_init(&op);
  op.op_type = where_clause ? ASYNC_OP_QUERY_PAGE_WHERE : ASYNC_OP_QUERY_PAGE;
  op.conn = conn;
  op.table_name = str_dup(table);
  op.offset = offset;
  op.limit = limit;
  op.where_clause = where_clause; /* Owned by op now */
  op.order_by = order_clause;
  op.reverse_rows = reverse;
  ResultSet *rs = NULL;
  if (async_start(&op)) {
    bool completed = tui_show_processing_dialog(state, &op, "Loading rows...");
    if (completed && op.state == ASYNC_STATE_COMPLETED) {
      rs = (ResultSet *)op.result;
      op.result = NULL;
    } else if (op.state == ASYNC_STATE_ERROR) {
      *err = op.error ? str_dup(op.error) : str_dup("Query failed");
    } else if (op.state == ASYNC_STATE_CANCELLED) {
      *cancelled = true;
    }
  }
  async_free(&op);
  return rs;
}

bool tui_load_page_with_dialog(TuiState *state, bool forward) {
  Tab *tab = TUI_TAB(state);
  DbConnection *conn = TUI_CONN(state);
  if (!tab || !conn || !tab->table_name || !tab->data)
    return false;

  /* Cancel any background load first */
  tui_cancel_background_load(state);

  /* Calculate target offset (an estimated total may be short: try) */
  size_t target_offset;
  if (forward) {
    target_offset = tab->loaded_offset + tab->loaded_count;
    if (target_offset >= tab->total_rows && !tab->row_count_approximate)
      return false;
  } else {
    if (tab->loaded_offset == 0)
      return false;
    target_offset = tab->loaded_offset > TUI_PAGE_SIZE
                        ? tab->loaded_offset - TUI_PAGE_SIZE
                        : 0;
  }

  tui_set_status(state, "Loading...");
  tui_refresh(state);

  /* Build clauses: with keyset paging, the rows after the window's last
   * row (or before its first, read in reverse), whatever their offset;
   * otherwise LIMIT/OFFSET */
  char *where_clause = build_filter_where(state);
  char *order_clause = NULL;
  size_t load_count = forward ? TUI_PAGE_SIZE : (tab->loaded_offset - target_offset);
  size_t query_offset = target_offset;
  bool keyset = false;
  LaceKeysetState *ks = tab->keyset_edge_stale ? NULL : tab_keyset(state);
  char *edge = ks ? lace_keyset_build_where(
                        ks, tab->data, forward ? tab->data->num_rows - 1 : 0,
                        forward)
                  : NULL;
  if (edge) {
    where_clause = and_where(where_clause, edge);
    order_clause = lace_keyset_build_order(ks, forward);
    query_offset = 0;
    keyset = true;
  } else {
    order_clause = build_order_clause(state);
  }
  lace_keyset_free(ks);

  /* Load on a worker (spinner, Esc cancels) */
  char *err = NULL;
  bool cancelled = false;
  ResultSet *new_data =
      load_page_modal(state, conn, tab->table_name, query_offset, load_count,
                      where_clause, order_clause, keyset && !forward, &err,
                      &cancelled);

  /* The dialog ran the event loop: the tab may have changed underneath */
  tab = TUI_TAB(state);
  if (!tab || !tab->data) {
    db_result_free(new_data);
    free(err);
    return false;
  }

  if (!new_data || new_data->num_rows == 0) {
    if (new_data)
      db_result_free(new_data);
    if (cancelled) {
      tui_set_status(state, "Loading cancelled");
    } else if (err) {
      tui_set_error(state, "Load failed: %s", err);
    } else {
      tui_set_status(state, "No more data");
    }
    free(err);
    return false;
  }
  free(err);

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
    /* Rows past an estimated total, or a short page that shows the end */
    size_t end = tab->loaded_offset + new_count;
    if (end > tab->total_rows ||
        (keyset && new_data->num_rows < load_count))
      tab->total_rows = end;
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
    /* A keyset page that came back short reached the first row (rows were
     * deleted meanwhile, or offsets were estimated) */
    if (keyset && new_data->num_rows < load_count) {
      /* Positions moved: selections are stored as positions */
      tab_clear_selections(tab);
      tab->loaded_offset = 0;
    } else
      tab->loaded_offset = tab->loaded_offset - new_data->num_rows;
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
    offset = tab->total_rows > TUI_PAGE_SIZE ? tab->total_rows - TUI_PAGE_SIZE : 0;
  }

  char *where_clause = build_filter_where(state);
  char *order_clause = NULL;
  size_t limit = TUI_PAGE_SIZE;
  bool from_end = false;

  /* The last page: read the table's end in reverse order instead of
   * skipping every row before it */
  LaceKeysetState *ks = tab_keyset(state);
  if (ks && offset > 0 && offset + TUI_PAGE_SIZE >= tab->total_rows &&
      !tab->row_count_approximate) {
    limit = tab->total_rows - offset;
    order_clause = lace_keyset_build_order(ks, false);
    from_end = true;
  } else {
    order_clause = build_order_clause(state);
  }
  lace_keyset_free(ks);

  char *err = NULL;
  bool cancelled = false;
  ResultSet *data = load_page_modal(state, conn, tab->table_name,
                                    from_end ? 0 : offset, limit, where_clause,
                                    order_clause, from_end, &err, &cancelled);
  tab = TUI_TAB(state); /* The dialog ran the event loop */
  if (!data || !tab) {
    if (cancelled) {
      tui_set_status(state, "Loading cancelled");
    } else {
      tui_set_error(state, "Query failed: %s", err ? err : "Unknown error");
    }
    free(err);
    db_result_free(data);
    return false;
  }
  free(err);

  tab_data_replaced(tab);
  if (tab->data)
    db_result_free(tab->data);

  tab->data = data;
  if (from_end && data->num_rows < limit) {
    /* Fewer rows than the total said: these are all of them */
    offset = 0;
    tab->total_rows = data->num_rows;
  }
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

    size_t target_offset = (abs_row / TUI_PAGE_SIZE) * TUI_PAGE_SIZE;
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

    size_t visible = (size_t)tui_get_layout_info(state).visible_rows; /* >= 1 */
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

  size_t max_rows = MAX_LOADED_PAGES * TUI_PAGE_SIZE;
  if (tab->loaded_count <= max_rows)
    return;

  size_t cursor_row = tab->cursor_row;
  size_t scroll_row = tab->scroll_row;
  size_t cursor_page = cursor_row / TUI_PAGE_SIZE;
  size_t total_pages = (tab->loaded_count + TUI_PAGE_SIZE - 1) / TUI_PAGE_SIZE;

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

  size_t trim_start = keep_start * TUI_PAGE_SIZE;
  size_t trim_end = keep_end * TUI_PAGE_SIZE;
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
    target_offset = tab->loaded_offset > TUI_PAGE_SIZE
                        ? tab->loaded_offset - TUI_PAGE_SIZE
                        : 0;
  }
  /* Backward: exactly the rows before the window. A full page from 0 used
   * to overlap the window when it started less than a page in. */
  size_t fetch_limit = forward ? TUI_PAGE_SIZE : tab->loaded_offset - target_offset;

  /* Build clauses */
  char *where_clause = build_filter_where(state);
  char *order_clause = build_order_clause(state);

  /* Setup operation */
  AsyncOperation *op = safe_calloc(1, sizeof(AsyncOperation));
  async_init(op);
  op->conn = conn;
  op->table_name = str_dup(tab->table_name);
  op->offset = target_offset;
  op->limit = fetch_limit;
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
  tab->bg_load_generation = tab->load_generation;
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

    /* Rows fetched for an older view (sorted, filtered, reloaded since) or
     * that do not join the loaded window exactly must not be merged */
    bool joins = false;
    if (tab->data && tab->bg_load_generation == tab->load_generation &&
        new_data->num_columns == tab->data->num_columns) {
      if (tab->bg_load_forward) {
        joins = tab->bg_load_target_offset ==
                tab->loaded_offset + tab->data->num_rows;
      } else {
        joins = tab->bg_load_target_offset <= tab->loaded_offset &&
                tab->bg_load_target_offset + new_data->num_rows ==
                    tab->loaded_offset;
      }
    }

    if (joins && new_data->num_rows > 0) {
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

  /* Wait until the worker is done: it still uses op and op->conn, so
   * abandoning it after a timeout risked a use-after-free once the tab or
   * connection was closed */
  while (async_poll(op) == ASYNC_STATE_RUNNING) {
    async_wait(op, 50);
  }

  /* Free result if any (under the mutex: the worker may still touch it) */
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
