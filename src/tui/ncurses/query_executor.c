/*
 * Lace
 * Query tab execution and pagination functions
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "query_internal.h"
#include "../../util/mem.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Extract table name from a simple SELECT query (e.g., SELECT ... FROM table)
 * Returns allocated string or NULL if not a simple single-table query */
char *query_extract_table_name(const char *sql) {
  if (!sql)
    return NULL;

  const char *p = sql;
  while (*p && isspace((unsigned char)*p))
    p++;

  /* Must start with SELECT */
  if (strncasecmp(p, "SELECT", 6) != 0)
    return NULL;

  /* Find FROM keyword (case insensitive, skip strings) */
  const char *from_pos = NULL;
  bool in_string = false;
  char quote_char = 0;

  for (const char *s = p; *s; s++) {
    if (in_string) {
      if (*s == quote_char && (s == p || *(s - 1) != '\\')) {
        in_string = false;
      }
    } else {
      if (*s == '\'' || *s == '"') {
        in_string = true;
        quote_char = *s;
      } else if (isspace((unsigned char)*s) || *s == '(') {
        /* Check for FROM keyword */
        if (s[0] && strncasecmp(s + 1, "FROM", 4) == 0 &&
            (isspace((unsigned char)s[5]) || s[5] == '`' || s[5] == '"')) {
          from_pos = s + 5;
          break;
        }
      }
    }
  }

  if (!from_pos)
    return NULL;

  /* Skip whitespace after FROM */
  while (*from_pos && isspace((unsigned char)*from_pos))
    from_pos++;

  if (!*from_pos)
    return NULL;

  /* Extract table name (handle quoted identifiers) */
  const char *table_start = from_pos;
  const char *table_end = from_pos;
  char quote = 0;

  if (*from_pos == '`' || *from_pos == '"' || *from_pos == '[') {
    quote = (*from_pos == '[') ? ']' : *from_pos;
    table_start++;
    table_end = table_start;
    while (*table_end && *table_end != quote)
      table_end++;
  } else {
    while (*table_end && !isspace((unsigned char)*table_end) &&
           *table_end != ',' && *table_end != ';' && *table_end != ')')
      table_end++;
  }

  if (table_end == table_start)
    return NULL;

  /* Check if there's a comma (multiple tables = can't edit) */
  const char *check = table_end;
  if (quote)
    check++; /* Skip closing quote */
  while (*check && isspace((unsigned char)*check))
    check++;
  if (*check == ',')
    return NULL; /* JOIN or multiple tables */

  size_t len = table_end - table_start;
  char *table = safe_malloc(len + 1);
  memcpy(table, table_start, len);
  table[len] = '\0';
  return table;
}

/* Check if query has LIMIT or OFFSET clause (case-insensitive, outside strings)
 */
bool query_has_limit_offset(const char *sql) {
  if (!sql)
    return false;

  bool in_string = false;
  char quote_char = 0;
  const char *p = sql;

  while (*p) {
    if (in_string) {
      if (*p == quote_char && (p == sql || *(p - 1) != '\\')) {
        in_string = false;
      }
    } else {
      if (*p == '\'' || *p == '"') {
        in_string = true;
        quote_char = *p;
      } else if (isspace((unsigned char)*p) || p == sql) {
        /* Check for LIMIT or OFFSET keyword */
        const char *check = (p == sql) ? p : p + 1;
        if (strncasecmp(check, "LIMIT", 5) == 0 &&
            (check[5] == '\0' || isspace((unsigned char)check[5]) ||
             isdigit((unsigned char)check[5]))) {
          return true;
        }
        if (strncasecmp(check, "OFFSET", 6) == 0 &&
            (check[6] == '\0' || isspace((unsigned char)check[6]) ||
             isdigit((unsigned char)check[6]))) {
          return true;
        }
      }
    }
    p++;
  }
  return false;
}

/* Count total rows for a SELECT query using COUNT wrapper */
int64_t query_count_rows(TuiState *state, const char *base_sql) {
  if (!state || !state->conn || !base_sql)
    return -1;

  /* Wrap the base query in COUNT(*) */
  char *count_sql =
      str_printf("SELECT COUNT(*) FROM (%s) AS _count_wrapper", base_sql);
  if (!count_sql)
    return -1;

  char *err = NULL;
  ResultSet *result = db_query(state->conn, count_sql, &err);
  free(count_sql);

  if (!result || result->num_rows == 0 || result->num_columns == 0 ||
      !result->rows) {
    free(err);
    if (result)
      db_result_free(result);
    return -1;
  }

  /* Get count value from first row, first column */
  int64_t count = -1;
  if (result->rows[0].num_cells > 0) {
    DbValue *val = &result->rows[0].cells[0];
    if (val->type == DB_TYPE_INT) {
      count = val->int_val;
    } else if (val->type == DB_TYPE_FLOAT) {
      count = (int64_t)val->float_val;
    } else if (val->type == DB_TYPE_TEXT && val->text.data) {
      errno = 0;
      char *endptr;
      long long parsed = strtoll(val->text.data, &endptr, 10);
      if (errno == 0 && endptr != val->text.data) {
        count = parsed;
      }
    }
  }

  db_result_free(result);
  free(err);
  return count;
}

/* Execute a SQL query and store results */
void query_execute(TuiState *state, const char *sql) {
  if (!state || !sql || !*sql)
    return;

  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || tab->type != TAB_TYPE_QUERY || !ui)
    return;

  /* Free previous results */
  if (tab->query_results) {
    db_result_free(tab->query_results);
    tab->query_results = NULL;
  }
  free(tab->query_error);
  tab->query_error = NULL;
  tab->query_affected = 0;
  tab->query_exec_success = false;
  free(tab->query_result_col_widths);
  tab->query_result_col_widths = NULL;
  tab->query_result_num_cols = 0;
  free(tab->query_source_table);
  tab->query_source_table = NULL;
  db_schema_free(tab->query_source_schema);
  tab->query_source_schema = NULL;
  free(tab->query_base_sql);
  tab->query_base_sql = NULL;
  tab->query_total_rows = 0;
  tab->query_loaded_offset = 0;
  tab->query_loaded_count = 0;
  tab->query_paginated = false;

  /* Clear any row selections */
  tab_clear_selections(tab);

  /* Reset result cursor */
  tab->query_result_row = 0;
  tab->query_result_col = 0;
  tab->query_result_scroll_row = 0;
  tab->query_result_scroll_col = 0;

  if (!state->conn) {
    tab->query_error = str_dup("Not connected to database");
    return;
  }

  /* Detect query type (simple heuristic) */
  const char *p = sql;
  while (*p && isspace((unsigned char)*p))
    p++;

  bool is_select = (strncasecmp(p, "SELECT", 6) == 0);
  bool is_readonly = is_select || (strncasecmp(p, "SHOW", 4) == 0) ||
                     (strncasecmp(p, "DESCRIBE", 8) == 0) ||
                     (strncasecmp(p, "EXPLAIN", 7) == 0) ||
                     (strncasecmp(p, "PRAGMA", 6) == 0);

  char *err = NULL;

  if (is_readonly) {
    /* Check if pagination should be applied (SELECT only, no existing LIMIT) */
    bool should_paginate = is_select && !query_has_limit_offset(sql);

    if (should_paginate) {
      /* Store base SQL for pagination */
      tab->query_base_sql = str_dup(sql);

      /* Get total row count */
      int64_t total = query_count_rows(state, sql);
      if (total >= 0) {
        tab->query_total_rows = (size_t)total;
      } else {
        /* Fallback: can't count, disable pagination */
        tab->query_total_rows = 0;
      }

      /* Execute with LIMIT/OFFSET for first page using async operation */
      char *paginated_sql = str_printf("%s LIMIT %d OFFSET 0", sql, PAGE_SIZE);
      if (paginated_sql) {
        AsyncOperation op;
        async_init(&op);
        op.op_type = ASYNC_OP_QUERY;
        op.conn = state->conn;
        op.sql = paginated_sql; /* ownership transferred */

        if (async_start(&op)) {
          bool completed =
              tui_show_processing_dialog(state, &op, "Executing query...");
          if (completed && op.state == ASYNC_STATE_COMPLETED) {
            tab->query_results = (ResultSet *)op.result;
            if (tab->query_results) {
              tab->query_paginated = true;
              tab->query_loaded_offset = 0;
              tab->query_loaded_count = tab->query_results->num_rows;
            }
          } else if (op.state == ASYNC_STATE_ERROR) {
            err = op.error ? str_dup(op.error) : str_dup("Query failed");
          } else if (op.state == ASYNC_STATE_CANCELLED) {
            tui_set_status(state, "Query cancelled");
          }
        }
        op.sql = NULL; /* Prevent double free */
        async_free(&op);
        free(paginated_sql);
      }
    } else {
      /* Execute as-is (has LIMIT/OFFSET or not a SELECT) using async */
      AsyncOperation op;
      async_init(&op);
      op.op_type = ASYNC_OP_QUERY;
      op.conn = state->conn;
      op.sql = str_dup(sql);
      if (async_start(&op)) {
        bool completed =
            tui_show_processing_dialog(state, &op, "Executing query...");
        if (completed && op.state == ASYNC_STATE_COMPLETED) {
          tab->query_results = (ResultSet *)op.result;
        } else if (op.state == ASYNC_STATE_ERROR) {
          err = op.error ? str_dup(op.error) : str_dup("Query failed");
        } else if (op.state == ASYNC_STATE_CANCELLED) {
          tui_set_status(state, "Query cancelled");
        }
      }
      async_free(&op);
    }

    if (err) {
      tab->query_error = err;
    } else if (tab->query_results) {
      query_calculate_result_widths(tab);
      /* Try to extract table name for editing support */
      tab->query_source_table = query_extract_table_name(sql);
      /* Load table schema for primary key info (needed for SQLite/PostgreSQL)
       */
      if (tab->query_source_table) {
        char *schema_err = NULL;
        tab->query_source_schema = db_get_table_schema(
            state->conn, tab->query_source_table, &schema_err);
        free(schema_err); /* Ignore schema errors */
      }
      if (tab->query_paginated && tab->query_total_rows > 0) {
        tui_set_status(state, "Loaded %zu/%zu rows", tab->query_loaded_count,
                       tab->query_total_rows);
      } else {
        tui_set_status(state, "%zu rows returned",
                       tab->query_results->num_rows);
      }
      /* History is recorded automatically by database layer */
    }
  } else {
    /* Execute non-SELECT query using async operation */
    AsyncOperation op;
    async_init(&op);
    op.op_type = ASYNC_OP_EXEC;
    op.conn = state->conn;
    op.sql = str_dup(sql);

    if (op.sql && async_start(&op)) {
      bool completed =
          tui_show_processing_dialog(state, &op, "Executing statement...");
      if (completed && op.state == ASYNC_STATE_COMPLETED) {
        tab->query_affected = op.count;
        tab->query_exec_success = true;
        tui_set_status(state, "%lld rows affected", (long long)op.count);
        /* History is recorded automatically by database layer */
      } else if (op.state == ASYNC_STATE_ERROR) {
        err = op.error ? str_dup(op.error) : str_dup("Statement failed");
        tab->query_error = err;
      } else if (op.state == ASYNC_STATE_CANCELLED) {
        tui_set_status(state, "Statement cancelled");
      }
    }
    async_free(&op);
  }

  /* Focus results pane after execution (only if there are results) */
  if (!tab->query_error && tab->query_results &&
      tab->query_results->num_rows > 0) {
    ui->query_focus_results = true;
  }
}

/* Calculate column widths for query results */
void query_calculate_result_widths(Tab *tab) {
  if (!tab->query_results || tab->query_results->num_columns == 0)
    return;

  size_t num_cols = tab->query_results->num_columns;

  tab->query_result_col_widths = safe_calloc(num_cols, sizeof(int));
  tab->query_result_num_cols = num_cols;

  /* Start with column name widths */
  for (size_t c = 0; c < num_cols; c++) {
    const char *name = tab->query_results->columns[c].name;
    int w = name ? (int)strlen(name) : 4;
    if (w < MIN_COL_WIDTH)
      w = MIN_COL_WIDTH;
    tab->query_result_col_widths[c] = w;
  }

  /* Check data values */
  for (size_t r = 0; r < tab->query_results->num_rows && r < 100; r++) {
    Row *row = &tab->query_results->rows[r];
    for (size_t c = 0; c < num_cols && c < row->num_cells; c++) {
      char *str = db_value_to_string(&row->cells[c]);
      if (str) {
        int w = (int)strlen(str);
        if (w > tab->query_result_col_widths[c]) {
          tab->query_result_col_widths[c] = w;
        }
        free(str);
      }
    }
  }

  /* Clamp to max width */
  for (size_t c = 0; c < num_cols; c++) {
    if (tab->query_result_col_widths[c] > MAX_COL_WIDTH) {
      tab->query_result_col_widths[c] = MAX_COL_WIDTH;
    }
  }
}

/* Load more rows at end of current query results */
bool query_load_more_rows(TuiState *state, Tab *tab) {
  if (!state || !state->conn || !tab || !tab->query_paginated ||
      !tab->query_base_sql)
    return false;
  if (!tab->query_results)
    return false;

  size_t new_offset = tab->query_loaded_offset + tab->query_loaded_count;

  /* Check if there are more rows to load */
  if (tab->query_total_rows > 0 && new_offset >= tab->query_total_rows)
    return false;

  char *paginated_sql = str_printf("%s LIMIT %d OFFSET %zu",
                                   tab->query_base_sql, PAGE_SIZE, new_offset);
  if (!paginated_sql)
    return false;

  char *err = NULL;
  ResultSet *more = db_query(state->conn, paginated_sql, &err);
  free(paginated_sql);

  if (!more || more->num_rows == 0) {
    if (more)
      db_result_free(more);
    free(err);
    return false;
  }

  /* Extend existing rows array */
  size_t old_count = tab->query_results->num_rows;
  size_t new_count = old_count + more->num_rows;

  /* Check for overflow and enforce maximum row limit (1M rows) */
  if (new_count < old_count || new_count > SIZE_MAX / sizeof(Row) ||
      new_count > 1000000) {
    db_result_free(more);
    return false;
  }

  tab->query_results->rows = safe_reallocarray(tab->query_results->rows, new_count, sizeof(Row));

  /* Copy new rows */
  for (size_t i = 0; i < more->num_rows; i++) {
    tab->query_results->rows[old_count + i] = more->rows[i];
    /* Clear source so free doesn't deallocate the cells we moved */
    more->rows[i].cells = NULL;
    more->rows[i].num_cells = 0;
  }

  tab->query_results->num_rows = new_count;
  tab->query_loaded_count = new_count;

  db_result_free(more);

  /* Trim old data to keep memory bounded */
  query_trim_loaded_data(state, tab);

  tui_set_status(state, "Loaded %zu/%zu rows", tab->query_loaded_count,
                 tab->query_total_rows);
  return true;
}

/* Load previous rows (prepend to current query results) */
bool query_load_prev_rows(TuiState *state, Tab *tab) {
  if (!state || !state->conn || !tab || !tab->query_paginated ||
      !tab->query_base_sql)
    return false;
  if (!tab->query_results)
    return false;
  if (tab->query_loaded_offset == 0)
    return false; /* Already at beginning */

  /* Calculate how many rows to load before current offset */
  size_t load_count = PAGE_SIZE;
  size_t new_offset = 0;
  if (tab->query_loaded_offset > load_count) {
    new_offset = tab->query_loaded_offset - load_count;
  } else {
    load_count = tab->query_loaded_offset;
    new_offset = 0;
  }

  char *paginated_sql = str_printf("%s LIMIT %zu OFFSET %zu",
                                   tab->query_base_sql, load_count, new_offset);
  if (!paginated_sql)
    return false;

  char *err = NULL;
  ResultSet *more = db_query(state->conn, paginated_sql, &err);
  free(paginated_sql);

  if (!more || more->num_rows == 0) {
    if (more)
      db_result_free(more);
    free(err);
    return false;
  }

  /* Prepend rows to existing data */
  size_t old_count = tab->query_results->num_rows;
  size_t new_count = old_count + more->num_rows;

  /* Check for overflow */
  if (new_count < old_count || new_count > SIZE_MAX / sizeof(Row)) {
    db_result_free(more);
    return false;
  }

  Row *new_rows = safe_reallocarray(NULL, new_count, sizeof(Row));

  /* Copy new rows first (prepend) */
  for (size_t i = 0; i < more->num_rows; i++) {
    new_rows[i] = more->rows[i];
    /* Clear source so free doesn't deallocate the cells we moved */
    more->rows[i].cells = NULL;
    more->rows[i].num_cells = 0;
  }

  /* Then copy old rows */
  for (size_t i = 0; i < old_count; i++) {
    new_rows[more->num_rows + i] = tab->query_results->rows[i];
  }

  /* Free old array (but not the cells which we moved) */
  free(tab->query_results->rows);
  tab->query_results->rows = new_rows;
  tab->query_results->num_rows = new_count;

  /* Adjust cursor position (it's now offset by the prepended rows) */
  tab->query_result_row += more->num_rows;
  tab->query_result_scroll_row += more->num_rows;

  /* Update tracking */
  tab->query_loaded_offset = new_offset;
  tab->query_loaded_count = new_count;

  db_result_free(more);

  /* Trim old data to keep memory bounded */
  query_trim_loaded_data(state, tab);

  tui_set_status(state, "Loaded %zu/%zu rows", tab->query_loaded_count,
                 tab->query_total_rows);
  return true;
}

/* Trim loaded query data to keep memory bounded */
void query_trim_loaded_data(TuiState *state, Tab *tab) {
  (void)state; /* Currently unused */

  if (!tab || !tab->query_results || tab->query_results->num_rows == 0)
    return;

  size_t max_rows = MAX_LOADED_PAGES * PAGE_SIZE;
  if (tab->query_loaded_count <= max_rows)
    return;

  /* Calculate cursor's page within loaded data */
  size_t cursor_page = tab->query_result_row / PAGE_SIZE;
  size_t total_pages = (tab->query_loaded_count + PAGE_SIZE - 1) / PAGE_SIZE;

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
  size_t trim_start = keep_start_page * PAGE_SIZE;
  size_t trim_end = keep_end_page * PAGE_SIZE;
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
    if (tab->query_total_rows > 0 && loaded_end < tab->query_total_rows) {
      query_load_more_rows(state, tab);
    }
  }

  /* If cursor is within LOAD_THRESHOLD of the BEGINNING, load previous rows */
  if (tab->query_result_row < LOAD_THRESHOLD && tab->query_loaded_offset > 0) {
    query_load_prev_rows(state, tab);
  }
}
