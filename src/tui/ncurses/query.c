/*
 * Lace
 * Query tab implementation
 *
 * During ViewModel migration, Tab is the source of truth for query state
 * (query_text, query_cursor, etc.). VmQuery is available for future native
 * GUI text editor integration.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../config/config.h"
#include "../../viewmodel/vm_query.h"
#include "tui_internal.h"
#include "views/editor_view.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Helper to get VmQuery, returns NULL if not valid */
static VmQuery *get_vm_query(TuiState *state) {
  if (!state || !state->vm_query)
    return NULL;
  if (!vm_query_valid(state->vm_query))
    return NULL;
  return state->vm_query;
}

#define QUERY_INITIAL_CAPACITY 1024
#define QUERY_GROWTH_FACTOR 2
#define QUERY_MAX_SIZE (64 * 1024 * 1024) /* 64 MB max query size */

/* Line info for cursor tracking */
typedef struct {
  size_t start; /* Byte offset of line start */
  size_t len;   /* Length of line (excluding newline) */
} QueryLineInfo;

/* Forward declarations */
static bool query_ensure_capacity(Tab *tab, size_t needed);
static void query_rebuild_line_cache(Tab *tab, QueryLineInfo **lines,
                                     size_t *num_lines);
static void query_cursor_to_line_col(Tab *tab, size_t *line, size_t *col);
static size_t query_line_col_to_cursor(Tab *tab, size_t line, size_t col,
                                       QueryLineInfo *lines, size_t num_lines);
static void query_insert_char(Tab *tab, char c);
static void query_delete_char(Tab *tab);
static void query_backspace(Tab *tab);
static char *query_find_at_cursor(const char *text, size_t cursor);
static void query_execute(TuiState *state, const char *sql);
static void query_calculate_result_widths(Tab *tab);

/* Pagination helpers */
static bool query_has_limit_offset(const char *sql);
static int64_t query_count_rows(TuiState *state, const char *base_sql);
static bool query_load_more_rows(TuiState *state, Tab *tab);
static bool query_load_prev_rows(TuiState *state, Tab *tab);
static void query_trim_loaded_data(TuiState *state, Tab *tab);
static void query_check_load_more(TuiState *state, Tab *tab);

/* Create a new query tab */
bool tab_create_query(TuiState *state) {
  if (!state || !state->app)
    return false;

  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws) {
    /* No workspace - need to create one first */
    ws = app_create_workspace(state->app);
    if (!ws)
      return false;
  }

  /* Get connection index - use current tab's connection if available */
  size_t connection_index = 0;
  Tab *current_tab = TUI_TAB(state);
  if (current_tab) {
    connection_index = current_tab->connection_index;
  }

  /* Save current tab first */
  if (ws->num_tabs > 0) {
    tab_save(state);
  }

  /* Find next query number by scanning existing tabs */
  int max_query_num = 0;
  for (size_t i = 0; i < ws->num_tabs; i++) {
    Tab *t = &ws->tabs[i];
    if (t->type == TAB_TYPE_QUERY && t->table_name) {
      int num = 0;
      if (sscanf(t->table_name, "Query %d", &num) == 1) {
        if (num > max_query_num)
          max_query_num = num;
      }
    }
  }

  /* Create new tab with connection reference */
  Tab *tab = workspace_create_query_tab(ws, connection_index);
  if (!tab)
    return false;

  /* Ensure UITabState capacity for new tab */
  tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                             ws->current_tab);

  /* Initialize UITabState for new query tab (source of truth) */
  UITabState *ui = TUI_TAB_UI(state);
  if (ui) {
    /* Inherit current sidebar state */
    ui->sidebar_visible = state->sidebar_visible;
    ui->sidebar_focused = false;
    ui->sidebar_highlight = state->sidebar_highlight;
    ui->sidebar_scroll = state->sidebar_scroll;
    ui->sidebar_filter_len = state->sidebar_filter_len;
    memcpy(ui->sidebar_filter, state->sidebar_filter,
           sizeof(ui->sidebar_filter));

    /* Query tab has no filters panel */
    ui->filters_visible = false;
    ui->filters_focused = false;

    /* Query starts with focus on editor, not results */
    ui->query_focus_results = false;
  }

  /* Update tab name (free the default "Query" name set by workspace_create_query_tab) */
  free(tab->table_name);
  tab->table_name = str_printf("Query %d", max_query_num + 1);
  if (!tab->table_name) {
    workspace_close_tab(ws, ws->current_tab);
    return false;
  }

  /* Initialize query buffer */
  tab->query_capacity = QUERY_INITIAL_CAPACITY;
  tab->query_text = calloc(tab->query_capacity, 1);
  if (!tab->query_text) {
    workspace_close_tab(ws, ws->current_tab);
    return false;
  }
  tab->query_len = 0;
  tab->query_cursor = 0;
  tab->query_scroll_line = 0;
  tab->query_scroll_col = 0;

  /* Clear convenience pointers (query mode doesn't use them) */
  state->data = NULL;
  state->schema = NULL;
  state->col_widths = NULL;
  state->num_col_widths = 0;

  /* Reset TUI state for new tab - all panels start unfocused, filters closed */
  state->sidebar_focused = false;
  state->filters_visible = false;
  state->filters_focused = false;
  state->filters_was_focused = false;
  state->filters_editing = false;
  state->filters_cursor_row = 0;
  state->filters_cursor_col = 0;
  state->filters_scroll = 0;

  tui_set_status(state, "Query tab opened. Ctrl+R to run, Ctrl+A to run all");
  return true;
}

/* Legacy wrapper for compatibility */
bool workspace_create_query(TuiState *state) { return tab_create_query(state); }

/* Ensure query buffer has enough capacity. Returns false on allocation failure.
 */
static bool query_ensure_capacity(Tab *tab, size_t needed) {
  if (needed <= tab->query_capacity)
    return true;

  /* Reject unreasonably large queries */
  if (needed > QUERY_MAX_SIZE)
    return false;

  size_t new_cap = tab->query_capacity;
  while (new_cap < needed) {
    /* Check for overflow before multiplying */
    if (new_cap > SIZE_MAX / QUERY_GROWTH_FACTOR) {
      new_cap = needed; /* Fall back to exact size */
      break;
    }
    new_cap *= QUERY_GROWTH_FACTOR;
  }

  char *new_buf = realloc(tab->query_text, new_cap);
  if (!new_buf) {
    return false; /* Allocation failed */
  }
  tab->query_text = new_buf;
  tab->query_capacity = new_cap;
  return true;
}

/* Rebuild line cache from query text */
static void query_rebuild_line_cache(Tab *tab, QueryLineInfo **lines,
                                     size_t *num_lines) {
  /* Count lines */
  size_t count = 1;
  for (size_t i = 0; i < tab->query_len; i++) {
    if (tab->query_text[i] == '\n')
      count++;
  }

  *lines = calloc(count, sizeof(QueryLineInfo));
  if (!*lines) {
    *num_lines = 0;
    return;
  }

  /* Build line info */
  size_t line_idx = 0;
  size_t line_start = 0;

  for (size_t i = 0; i <= tab->query_len; i++) {
    if (i == tab->query_len || tab->query_text[i] == '\n') {
      (*lines)[line_idx].start = line_start;
      (*lines)[line_idx].len = i - line_start;
      line_idx++;
      line_start = i + 1;
    }
  }

  *num_lines = count;
}

/* Convert cursor offset to line/column */
static void query_cursor_to_line_col(Tab *tab, size_t *line, size_t *col) {
  *line = 0;
  *col = 0;

  size_t line_start = 0;
  for (size_t i = 0; i < tab->query_cursor && i < tab->query_len; i++) {
    if (tab->query_text[i] == '\n') {
      (*line)++;
      line_start = i + 1;
    }
  }
  *col = tab->query_cursor - line_start;
}

/* Convert line/column to cursor offset */
static size_t query_line_col_to_cursor(Tab *tab __attribute__((unused)),
                                       size_t line, size_t col,
                                       QueryLineInfo *lines, size_t num_lines) {
  if (!lines || num_lines == 0)
    return 0;

  if (line >= num_lines)
    line = num_lines - 1;

  size_t cursor = lines[line].start + col;
  if (col > lines[line].len)
    cursor = lines[line].start + lines[line].len;

  return cursor;
}

/* Insert character at cursor */
static void query_insert_char(Tab *tab, char c) {
  if (!query_ensure_capacity(tab, tab->query_len + 2)) {
    return; /* Allocation failed, silently ignore insert */
  }

  /* Shift text after cursor */
  memmove(tab->query_text + tab->query_cursor + 1,
          tab->query_text + tab->query_cursor,
          tab->query_len - tab->query_cursor);

  tab->query_text[tab->query_cursor] = c;
  tab->query_len++;
  tab->query_cursor++;
  tab->query_text[tab->query_len] = '\0';
}

/* Delete character at cursor (forward delete) */
static void query_delete_char(Tab *tab) {
  if (tab->query_cursor >= tab->query_len)
    return;

  memmove(tab->query_text + tab->query_cursor,
          tab->query_text + tab->query_cursor + 1,
          tab->query_len - tab->query_cursor);
  tab->query_len--;
  tab->query_text[tab->query_len] = '\0';
}

/* Delete character before cursor (backspace) */
static void query_backspace(Tab *tab) {
  if (tab->query_cursor == 0)
    return;

  tab->query_cursor--;
  query_delete_char(tab);
}

/* Find the byte boundaries of the query at cursor position.
 * Returns true if a valid query range was found.
 * out_start and out_end are set to the byte positions (not trimmed).
 * If cursor is in empty space after a query, falls back to the last query. */
static bool query_find_bounds_at_cursor(const char *text, size_t cursor,
                                        size_t *out_start, size_t *out_end) {
  if (!text || !*text) {
    *out_start = 0;
    *out_end = 0;
    return false;
  }

  size_t len = strlen(text);
  if (cursor > len)
    cursor = len;

  /* Scan from beginning to cursor to find query start (after last ';') */
  size_t last_semi = 0;
  size_t prev_semi = 0; /* Track the semicolon before last_semi */
  bool in_string = false;
  char quote_char = 0;

  for (size_t i = 0; i < cursor; i++) {
    char c = text[i];
    if (in_string) {
      if (c == quote_char && (i == 0 || text[i - 1] != '\\')) {
        in_string = false;
      }
    } else {
      if (c == '\'' || c == '"') {
        in_string = true;
        quote_char = c;
      } else if (c == ';') {
        prev_semi = last_semi;
        last_semi = i + 1;
      }
    }
  }

  size_t start = last_semi;

  /* Scan forward from cursor to find query end (next ';' or end of text) */
  size_t end = len;
  in_string = false;
  quote_char = 0;

  for (size_t i = cursor; i < len; i++) {
    char c = text[i];
    if (in_string) {
      if (c == quote_char && (i == 0 || text[i - 1] != '\\')) {
        in_string = false;
      }
    } else {
      if (c == '\'' || c == '"') {
        in_string = true;
        quote_char = c;
      } else if (c == ';') {
        end = i + 1; /* Include the semicolon */
        break;
      }
    }
  }

  /* Check if the current range is empty/whitespace only */
  bool is_empty = true;
  for (size_t i = start; i < end && is_empty; i++) {
    char c = text[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ';') {
      is_empty = false;
    }
  }

  /* If empty and we have a previous query, use that instead */
  if (is_empty && last_semi > 0) {
    start = prev_semi;
    end = last_semi; /* Include the semicolon of the previous query */
  }

  *out_start = start;
  *out_end = end;
  return start < end;
}

/* Find the query at cursor position (caller must free result) */
static char *query_find_at_cursor(const char *text, size_t cursor) {
  if (!text || !*text)
    return str_dup("");

  size_t len = strlen(text);
  if (cursor > len)
    cursor = len;

  /* Scan backward to find query start (after ';' or at beginning) */
  size_t start = cursor;
  bool in_string = false;
  char quote_char = 0;

  /* First pass: find start by scanning from beginning to cursor */
  size_t last_semi = 0;
  in_string = false;
  for (size_t i = 0; i < cursor; i++) {
    char c = text[i];
    if (in_string) {
      if (c == quote_char && (i == 0 || text[i - 1] != '\\')) {
        in_string = false;
      }
    } else {
      if (c == '\'' || c == '"') {
        in_string = true;
        quote_char = c;
      } else if (c == ';') {
        last_semi = i + 1;
      }
    }
  }
  start = last_semi;

  /* Scan forward to find query end */
  size_t end = cursor;
  in_string = false;
  for (size_t i = cursor; i < len; i++) {
    char c = text[i];
    if (in_string) {
      if (c == quote_char && (i == 0 || text[i - 1] != '\\')) {
        in_string = false;
      }
    } else {
      if (c == '\'' || c == '"') {
        in_string = true;
        quote_char = c;
      } else if (c == ';') {
        end = i;
        break;
      }
    }
    end = i + 1;
  }

  /* Extract query */
  size_t query_len = end - start;
  char *query = malloc(query_len + 1);
  if (!query)
    return NULL;

  memcpy(query, text + start, query_len);
  query[query_len] = '\0';

  /* Trim whitespace */
  char *trimmed = query;
  while (*trimmed && isspace((unsigned char)*trimmed))
    trimmed++;

  size_t trim_len = strlen(trimmed);
  while (trim_len > 0 && isspace((unsigned char)trimmed[trim_len - 1]))
    trim_len--;

  /* If empty, try to find the last available query */
  if (trim_len == 0 && last_semi > 0) {
    free(query);

    /* Find the query before last_semi */
    size_t prev_end = last_semi - 1; /* Position of the ';' */
    size_t prev_start = 0;

    /* Scan from beginning to find the start of the previous query */
    in_string = false;
    size_t prev_semi = 0;
    for (size_t i = 0; i < prev_end; i++) {
      char c = text[i];
      if (in_string) {
        if (c == quote_char && (i == 0 || text[i - 1] != '\\')) {
          in_string = false;
        }
      } else {
        if (c == '\'' || c == '"') {
          in_string = true;
          quote_char = c;
        } else if (c == ';') {
          prev_semi = i + 1;
        }
      }
    }
    prev_start = prev_semi;

    /* Extract the previous query */
    query_len = prev_end - prev_start;
    query = malloc(query_len + 1);
    if (!query)
      return NULL;

    memcpy(query, text + prev_start, query_len);
    query[query_len] = '\0';

    /* Trim again */
    trimmed = query;
    while (*trimmed && isspace((unsigned char)*trimmed))
      trimmed++;

    trim_len = strlen(trimmed);
    while (trim_len > 0 && isspace((unsigned char)trimmed[trim_len - 1]))
      trim_len--;
  }

  char *result = malloc(trim_len + 1);
  if (result) {
    memcpy(result, trimmed, trim_len);
    result[trim_len] = '\0';
  }

  free(query);
  return result;
}

/* Extract table name from a simple SELECT query (e.g., SELECT ... FROM table)
 * Returns allocated string or NULL if not a simple single-table query */
static char *query_extract_table_name(const char *sql) {
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
  char *table = malloc(len + 1);
  if (table) {
    memcpy(table, table_start, len);
    table[len] = '\0';
  }
  return table;
}

/* Check if query has LIMIT or OFFSET clause (case-insensitive, outside strings)
 */
static bool query_has_limit_offset(const char *sql) {
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
static int64_t query_count_rows(TuiState *state, const char *base_sql) {
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
static void query_execute(TuiState *state, const char *sql) {
  if (!state || !sql || !*sql)
    return;

  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || tab->type != TAB_TYPE_QUERY || !ui)
    return;

  /* Use VmQuery for connection if available (future cross-platform support) */
  VmQuery *vm = get_vm_query(state);
  DbConnection *conn = vm ? vm_query_connection(vm) : state->conn;
  (void)conn; /* Will be used when we fully migrate to viewmodel */

  /* Free previous results */
  if (tab->query_results) {
    db_result_free(tab->query_results);
    tab->query_results = NULL;
  }
  free(tab->query_error);
  tab->query_error = NULL;
  tab->query_affected = 0;
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

      if (op.sql && async_start(&op)) {
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
        tui_set_status(state, "%lld rows affected", (long long)op.count);
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
static void query_calculate_result_widths(Tab *tab) {
  if (!tab->query_results || tab->query_results->num_columns == 0)
    return;

  size_t num_cols = tab->query_results->num_columns;
  tab->query_result_col_widths = calloc(num_cols, sizeof(int));
  tab->query_result_num_cols = num_cols;

  if (!tab->query_result_col_widths)
    return;

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
static bool query_load_more_rows(TuiState *state, Tab *tab) {
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

  Row *new_rows = realloc(tab->query_results->rows, new_count * sizeof(Row));
  if (!new_rows) {
    db_result_free(more);
    return false;
  }

  tab->query_results->rows = new_rows;

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
static bool query_load_prev_rows(TuiState *state, Tab *tab) {
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
static void query_trim_loaded_data(TuiState *state, Tab *tab) {
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
  Row *new_rows = realloc(tab->query_results->rows, new_count * sizeof(Row));
  if (new_rows) {
    tab->query_results->rows = new_rows;
  }
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
static void query_check_load_more(TuiState *state, Tab *tab) {
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

/* Load query result rows at specific offset (replaces current data) */
bool query_load_rows_at(TuiState *state, Tab *tab, size_t offset) {
  if (!state || !state->conn || !tab || !tab->query_paginated ||
      !tab->query_base_sql)
    return false;

  /* Clamp offset */
  if (offset >= tab->query_total_rows) {
    offset = tab->query_total_rows > PAGE_SIZE
                 ? tab->query_total_rows - PAGE_SIZE
                 : 0;
  }

  char *paginated_sql = str_printf("%s LIMIT %d OFFSET %zu",
                                   tab->query_base_sql, PAGE_SIZE, offset);
  if (!paginated_sql)
    return false;

  char *err = NULL;
  ResultSet *data = db_query(state->conn, paginated_sql, &err);
  free(paginated_sql);

  if (!data) {
    tui_set_error(state, "Query failed: %s", err ? err : "Unknown error");
    free(err);
    return false;
  }

  /* Free old results */
  if (tab->query_results) {
    db_result_free(tab->query_results);
  }

  tab->query_results = data;
  tab->query_loaded_offset = offset;
  tab->query_loaded_count = data->num_rows;

  /* Recalculate column widths */
  free(tab->query_result_col_widths);
  tab->query_result_col_widths = NULL;
  if (data->num_columns > 0) {
    tab->query_result_col_widths = calloc(data->num_columns, sizeof(int));
    if (tab->query_result_col_widths) {
      for (size_t c = 0; c < data->num_columns; c++) {
        int w = data->columns[c].name ? (int)strlen(data->columns[c].name) : 0;
        if (w < 8)
          w = 8;
        for (size_t r = 0; r < data->num_rows && r < 100; r++) {
          if (r < data->num_rows && c < data->rows[r].num_cells) {
            DbValue *v = &data->rows[r].cells[c];
            int vw = 0;
            if (v->type == DB_TYPE_TEXT && v->text.data) {
              vw = (int)strlen(v->text.data);
            } else if (v->type == DB_TYPE_INT) {
              vw = 12;
            } else if (v->type == DB_TYPE_FLOAT) {
              vw = 15;
            }
            if (vw > w)
              w = vw;
          }
        }
        if (w > 50)
          w = 50;
        tab->query_result_col_widths[c] = w;
      }
    }
  }

  tui_set_status(state, "Loaded %zu/%zu rows", tab->query_loaded_count,
                 tab->query_total_rows);
  return true;
}

/* Draw the query tab */
void tui_draw_query(TuiState *state) {
  if (!state || !state->main_win)
    return;

  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || tab->type != TAB_TYPE_QUERY || !ui)
    return;

  werase(state->main_win);

  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);

  /* Split view: editor on top, results on bottom */
  int editor_height =
      (win_rows - 1) * 3 / 10; /* 30% for editor, -1 for separator */
  if (editor_height < 3)
    editor_height = 3; /* Minimum 3 lines for editor */
  int results_start = editor_height + 1;
  (void)(win_rows -
         results_start); /* results_height calculated but not stored */

  /* Build line cache */
  QueryLineInfo *lines = NULL;
  size_t num_lines = 0;
  query_rebuild_line_cache(tab, &lines, &num_lines);

  /* Get cursor line/col */
  size_t cursor_line, cursor_col;
  query_cursor_to_line_col(tab, &cursor_line, &cursor_col);

  /* Adjust scroll to keep cursor visible */
  if (cursor_line < tab->query_scroll_line) {
    tab->query_scroll_line = cursor_line;
  } else if (cursor_line >=
             tab->query_scroll_line + (size_t)(editor_height - 1)) {
    tab->query_scroll_line = cursor_line - editor_height + 2;
  }

  /* Find current query bounds for highlighting */
  size_t query_start = 0, query_end = 0;
  bool has_query_bounds =
      query_find_bounds_at_cursor(tab->query_text, tab->query_cursor,
                                  &query_start, &query_end);

  /* Draw editor area */
  if (!ui->query_focus_results) {
    wattron(state->main_win, A_BOLD);
  }
  mvwprintw(state->main_win, 0, 1,
            "SQL Query (^R: run, ^A: all, ^T: transaction, ^W: switch)");
  if (!ui->query_focus_results) {
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
    bool line_in_query = has_query_bounds &&
                         line_end > query_start && line_start < query_end;

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
    for (size_t i = 0; i < li->len && x < win_cols - 1; i++) {
      char c = tab->query_text[li->start + i];
      if (c == '\t') {
        /* Tab as spaces */
        for (int t = 0; t < 4 && x < win_cols - 1; t++) {
          mvwaddch(state->main_win, y, x++, ' ');
        }
      } else if (c >= 32 && c < 127) {
        mvwaddch(state->main_win, y, x++, c);
      }
    }

    if (is_dimmed) {
      wattroff(state->main_win, A_DIM);
    }

    /* Draw cursor if on this line and editor focused */
    if (!ui->query_focus_results && line_idx == cursor_line) {
      int cursor_x = 4 + (int)cursor_col;
      if (cursor_x < win_cols) {
        char cursor_char = ' ';
        if (cursor_col < li->len) {
          cursor_char = tab->query_text[li->start + cursor_col];
          if (cursor_char < 32 || cursor_char >= 127)
            cursor_char = ' ';
        }
        wattron(state->main_win, A_REVERSE);
        mvwaddch(state->main_win, y, cursor_x, cursor_char);
        wattroff(state->main_win, A_REVERSE);
      }
    }
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
                             .is_focused = ui->query_focus_results,
                             .is_editing = ui->query_result_editing,
                             .edit_buffer = ui->query_result_edit_buf,
                             .edit_pos = ui->query_result_edit_pos,
                             .show_header_line = false};

    tui_draw_result_grid(state, &params);
  } else if (tab->query_affected > 0) {
    /* Show affected rows */
    mvwprintw(state->main_win, results_start + 1, 1, "%lld rows affected",
              (long long)tab->query_affected);
  } else {
    /* No results yet */
    wattron(state->main_win, A_DIM);
    mvwprintw(state->main_win, results_start + 1, 1,
              "Enter SQL and press Ctrl+R to execute");
    wattroff(state->main_win, A_DIM);
  }

  wrefresh(state->main_win);
}

/* Forward declaration */
static void query_result_confirm_edit(TuiState *state, Tab *tab);

/* Get column width for query results */
static int query_get_col_width(Tab *tab, size_t col) {
  if (tab->query_result_col_widths && col < tab->query_result_num_cols) {
    return tab->query_result_col_widths[col];
  }
  return DEFAULT_COL_WIDTH;
}

/* Start editing a cell in query results (inline or modal based on content) */
static void query_result_start_edit(TuiState *state, Tab *tab) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !tab->query_results || !ui || ui->query_result_editing)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;
  if (tab->query_result_col >= tab->query_results->num_columns)
    return;

  Row *row = &tab->query_results->rows[tab->query_result_row];
  if (!row->cells || tab->query_result_col >= row->num_cells)
    return;

  DbValue *val = &row->cells[tab->query_result_col];

  /* Convert value to string */
  char *content = NULL;
  if (val->is_null) {
    content = str_dup("");
  } else {
    content = db_value_to_string(val);
    if (!content)
      content = str_dup("");
  }

  /* Check if content is truncated (exceeds column width) */
  int col_width = query_get_col_width(tab, tab->query_result_col);
  size_t content_len = content ? strlen(content) : 0;
  bool is_truncated = content_len > (size_t)col_width;

  /* Also check if content has newlines (always use modal for multi-line) */
  bool has_newlines = content && strchr(content, '\n') != NULL;

  if (is_truncated || has_newlines) {
    /* Use modal editor for truncated or multi-line content */
    const char *col_name =
        tab->query_results->columns[tab->query_result_col].name;
    char *title = str_printf("Edit: %s", col_name);

    EditorResult result =
        editor_view_show(state, title ? title : "Edit Cell", content, false);
    free(title);

    if (result.saved) {
      /* Update via the confirm flow */
      free(ui->query_result_edit_buf);
      ui->query_result_edit_buf = result.set_null ? NULL : result.content;
      ui->query_result_editing = true;
      query_result_confirm_edit(state, tab);
    } else {
      free(result.content);
    }

    free(content);
  } else {
    /* Use inline editing for short content */
    free(ui->query_result_edit_buf);
    ui->query_result_edit_buf = content;
    ui->query_result_edit_pos =
        ui->query_result_edit_buf ? strlen(ui->query_result_edit_buf) : 0;
    ui->query_result_editing = true;
    curs_set(1);
  }
}

/* Start modal editing for query results (always uses modal) */
static void query_result_start_modal_edit(TuiState *state, Tab *tab) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !tab->query_results || !ui || ui->query_result_editing)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;
  if (tab->query_result_col >= tab->query_results->num_columns)
    return;

  Row *row = &tab->query_results->rows[tab->query_result_row];
  if (!row->cells || tab->query_result_col >= row->num_cells)
    return;

  DbValue *val = &row->cells[tab->query_result_col];

  /* Convert value to string */
  char *content = NULL;
  if (val->is_null) {
    content = str_dup("");
  } else {
    content = db_value_to_string(val);
    if (!content)
      content = str_dup("");
  }

  /* Always use modal editor */
  const char *col_name =
      tab->query_results->columns[tab->query_result_col].name;
  char *title = str_printf("Edit: %s", col_name);

  EditorResult result =
      editor_view_show(state, title ? title : "Edit Cell", content, false);
  free(title);

  if (result.saved) {
    /* Update via the confirm flow */
    free(ui->query_result_edit_buf);
    ui->query_result_edit_buf = result.set_null ? NULL : result.content;
    ui->query_result_editing = true;
    query_result_confirm_edit(state, tab);
  } else {
    free(result.content);
  }

  free(content);
}

/* Public wrapper for starting edit from mouse handler */
void tui_query_start_result_edit(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !ui)
    return;
  if (tab->type != TAB_TYPE_QUERY || !ui->query_focus_results)
    return;
  query_result_start_edit(state, tab);
}

/* Public wrapper for confirming edit from mouse handler */
void tui_query_confirm_result_edit(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !ui)
    return;
  if (tab->type != TAB_TYPE_QUERY || !ui->query_result_editing)
    return;
  query_result_confirm_edit(state, tab);
}

/* Public wrapper for scrolling query results (used by mouse handler) */
void tui_query_scroll_results(TuiState *state, int delta) {
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;
  if (tab->type != TAB_TYPE_QUERY || !tab->query_results)
    return;
  if (tab->query_results->num_rows == 0)
    return;

  /* Calculate visible rows for scroll adjustment using actual main window */
  int main_rows, main_cols;
  getmaxyx(state->main_win, main_rows, main_cols);
  (void)main_cols;
  int win_rows = main_rows;
  int editor_height = (win_rows - 1) * 3 / 10;
  if (editor_height < 3)
    editor_height = 3;
  int visible = win_rows - editor_height - 4;
  if (visible < 1)
    visible = 1;

  if (delta < 0) {
    /* Scroll up */
    size_t amount = (size_t)(-delta);
    if (tab->query_result_row >= amount) {
      tab->query_result_row -= amount;
    } else {
      tab->query_result_row = 0;
    }
  } else if (delta > 0) {
    /* Scroll down */
    tab->query_result_row += (size_t)delta;
    if (tab->query_result_row >= tab->query_results->num_rows) {
      tab->query_result_row = tab->query_results->num_rows > 0
                                  ? tab->query_results->num_rows - 1
                                  : 0;
    }
  }

  /* Adjust scroll position */
  if (tab->query_result_row < tab->query_result_scroll_row) {
    tab->query_result_scroll_row = tab->query_result_row;
  } else if (tab->query_result_row >=
             tab->query_result_scroll_row + (size_t)visible) {
    tab->query_result_scroll_row = tab->query_result_row - visible + 1;
  }

  /* Check if more rows need to be loaded */
  query_check_load_more(state, tab);
}

/* Cancel editing in query results */
static void query_result_cancel_edit(TuiState *state, Tab *tab) {
  if (!tab)
    return;

  UITabState *ui = TUI_TAB_UI(state);
  if (!ui)
    return;

  free(ui->query_result_edit_buf);
  ui->query_result_edit_buf = NULL;
  ui->query_result_edit_pos = 0;
  ui->query_result_editing = false;
  curs_set(0);
}

/* Find column index in result set by name */
static size_t query_find_column_by_name(Tab *tab, const char *name) {
  if (!tab || !tab->query_results || !name)
    return (size_t)-1;

  for (size_t i = 0; i < tab->query_results->num_columns; i++) {
    if (tab->query_results->columns[i].name &&
        strcmp(tab->query_results->columns[i].name, name) == 0) {
      return i;
    }
  }
  return (size_t)-1;
}

/* Find primary key columns in query results
 * Uses schema if available (for SQLite/PostgreSQL), falls back to result
 * metadata */
static size_t query_find_pk_columns(Tab *tab, size_t *pk_indices,
                                    size_t max_pks) {
  if (!tab || !tab->query_results || !pk_indices || max_pks == 0)
    return 0;

  size_t count = 0;

  /* First try using the loaded schema (more reliable for SQLite/PostgreSQL) */
  if (tab->query_source_schema) {
    for (size_t i = 0;
         i < tab->query_source_schema->num_columns && count < max_pks; i++) {
      if (tab->query_source_schema->columns[i].primary_key) {
        /* Find this column in the result set */
        const char *pk_name = tab->query_source_schema->columns[i].name;
        size_t result_idx = query_find_column_by_name(tab, pk_name);
        if (result_idx != (size_t)-1) {
          pk_indices[count++] = result_idx;
        }
      }
    }
    if (count > 0) {
      return count; /* Found PKs via schema */
    }
  }

  /* Fall back to result set metadata (works for MySQL) */
  for (size_t i = 0; i < tab->query_results->num_columns && count < max_pks;
       i++) {
    if (tab->query_results->columns[i].primary_key) {
      pk_indices[count++] = i;
    }
  }
  return count;
}

/* Primary key info for query result database operations */
typedef struct {
  const char **col_names;
  DbValue *values;
  size_t count;
} QueryPkInfo;

/* Build PK info from query result row. Returns false on error. */
static bool query_pk_info_build(QueryPkInfo *pk, Tab *tab, size_t row_idx) {
  if (!pk || !tab || !tab->query_results ||
      row_idx >= tab->query_results->num_rows)
    return false;

  size_t pk_indices[16];
  size_t num_pk = query_find_pk_columns(tab, pk_indices, 16);
  if (num_pk == 0)
    return false;

  Row *row = &tab->query_results->rows[row_idx];
  for (size_t i = 0; i < num_pk; i++) {
    if (pk_indices[i] >= tab->query_results->num_columns ||
        pk_indices[i] >= row->num_cells) {
      return false;
    }
  }

  pk->col_names = malloc(sizeof(char *) * num_pk);
  pk->values = malloc(sizeof(DbValue) * num_pk);
  if (!pk->col_names || !pk->values) {
    free(pk->col_names);
    free(pk->values);
    pk->col_names = NULL;
    pk->values = NULL;
    return false;
  }

  for (size_t i = 0; i < num_pk; i++) {
    pk->col_names[i] = tab->query_results->columns[pk_indices[i]].name;
    pk->values[i] = db_value_copy(&row->cells[pk_indices[i]]);
  }
  pk->count = num_pk;
  return true;
}

static void query_pk_info_free(QueryPkInfo *pk) {
  if (!pk)
    return;
  free(pk->col_names);
  if (pk->values) {
    for (size_t i = 0; i < pk->count; i++) {
      db_value_free(&pk->values[i]);
    }
    free(pk->values);
  }
  pk->col_names = NULL;
  pk->values = NULL;
  pk->count = 0;
}

/* Confirm edit and update database */
static void query_result_confirm_edit(TuiState *state, Tab *tab) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !ui || !ui->query_result_editing || !tab->query_results)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;
  if (tab->query_result_col >= tab->query_results->num_columns)
    return;

  Row *row = &tab->query_results->rows[tab->query_result_row];
  if (tab->query_result_col >= row->num_cells) {
    query_result_cancel_edit(state, tab);
    return;
  }

  /* Create new value from edit buffer */
  DbValue new_val;
  if (ui->query_result_edit_buf == NULL) {
    new_val = db_value_null();
  } else {
    new_val = db_value_text(ui->query_result_edit_buf);
  }

  /* Try to update database if we have table name and primary keys */
  bool db_updated = false;
  bool db_error = false;
  bool can_update_db = false;

  if (state->conn && tab->query_source_table) {
    QueryPkInfo pk = {0};
    if (query_pk_info_build(&pk, tab, tab->query_result_row)) {
      can_update_db = true;

      const char *col_name =
          tab->query_results->columns[tab->query_result_col].name;

      char *err = NULL;
      db_updated =
          db_update_cell(state->conn, tab->query_source_table, pk.col_names,
                         pk.values, pk.count, col_name, &new_val, &err);

      if (!db_updated) {
        db_error = true;
        tui_set_error(state, "Update failed: %s", err ? err : "unknown error");
        free(err);
      }

      query_pk_info_free(&pk);
    }
  }

  /* If database update failed, don't update local cell */
  if (db_error) {
    db_value_free(&new_val);
    query_result_cancel_edit(state, tab);
    return;
  }

  /* Update the local cell value */
  DbValue *cell = &row->cells[tab->query_result_col];
  db_value_free(cell);
  *cell = new_val;

  if (db_updated) {
    tui_set_status(state, "Cell updated");
  } else if (!state->conn) {
    tui_set_status(state, "Cell updated (not connected)");
  } else if (!tab->query_source_table) {
    tui_set_status(state, "Cell updated (local only - complex query)");
  } else if (!can_update_db) {
    tui_set_status(state, "Cell updated (local only - no primary key)");
  }

  query_result_cancel_edit(state, tab);
}

/* Set query result cell directly to NULL or empty string */
static void query_result_set_cell_direct(TuiState *state, Tab *tab,
                                         bool set_null) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !tab->query_results || !ui || ui->query_result_editing)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;
  if (tab->query_result_col >= tab->query_results->num_columns)
    return;

  /* Set up the edit buffer and trigger confirm */
  free(ui->query_result_edit_buf);
  if (set_null) {
    ui->query_result_edit_buf = NULL;
  } else {
    ui->query_result_edit_buf = str_dup("");
  }
  ui->query_result_editing = true;
  query_result_confirm_edit(state, tab);
}

/* Delete row from query results */
static void query_result_delete_row(TuiState *state, Tab *tab) {
  if (!tab || !tab->query_results || !state->conn)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;

  if (!tab->query_source_table) {
    tui_set_error(state, "Cannot delete: no source table");
    return;
  }

  QueryPkInfo pk = {0};
  if (!query_pk_info_build(&pk, tab, tab->query_result_row)) {
    tui_set_error(state, "Cannot delete: no primary key found");
    return;
  }

  Row *row = &tab->query_results->rows[tab->query_result_row];

  /* Highlight the row being deleted with danger background */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);

  int editor_height = (win_rows - 1) * 3 / 10;
  if (editor_height < 3)
    editor_height = 3;
  int results_start = editor_height + 1;
  int row_y = results_start + 3 +
              (int)(tab->query_result_row - tab->query_result_scroll_row);

  int sidebar_width = state->sidebar_visible ? SIDEBAR_WIDTH : 0;
  wattron(state->main_win, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
  int x = 1;
  for (size_t col = tab->query_result_scroll_col;
       col < tab->query_results->num_columns && col < row->num_cells; col++) {
    int col_width =
        tab->query_result_col_widths ? tab->query_result_col_widths[col] : 15;
    if (x + col_width + 3 > win_cols - sidebar_width)
      break;

    DbValue *val = &row->cells[col];
    if (val->is_null) {
      mvwprintw(state->main_win, row_y, x, "%-*s", col_width, "NULL");
    } else {
      char *str = db_value_to_string(val);
      if (str) {
        char *safe = tui_sanitize_for_display(str);
        mvwprintw(state->main_win, row_y, x, "%-*.*s", col_width, col_width,
                  safe ? safe : str);
        free(safe);
        free(str);
      }
    }
    x += col_width + 1;
    mvwaddch(state->main_win, row_y, x - 1, ACS_VLINE);
  }
  wattroff(state->main_win, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
  wrefresh(state->main_win);

  if (!tui_show_confirm_dialog(state, "Delete this row?")) {
    tui_set_status(state, "Delete cancelled");
    query_pk_info_free(&pk);
    return;
  }

  char *err = NULL;
  bool success = db_delete_row(state->conn, tab->query_source_table,
                               pk.col_names, pk.values, pk.count, &err);
  query_pk_info_free(&pk);

  if (success) {
    tui_set_status(state, "Row deleted");

    /* Remove row from local results */
    for (size_t j = 0; j < row->num_cells; j++) {
      db_value_free(&row->cells[j]);
    }
    free(row->cells);

    for (size_t i = tab->query_result_row; i < tab->query_results->num_rows - 1;
         i++) {
      tab->query_results->rows[i] = tab->query_results->rows[i + 1];
    }
    tab->query_results->num_rows--;
    tab->query_loaded_count--;
    if (tab->query_total_rows > 0)
      tab->query_total_rows--;

    if (tab->query_result_row >= tab->query_results->num_rows &&
        tab->query_results->num_rows > 0)
      tab->query_result_row = tab->query_results->num_rows - 1;

    if (tab->query_result_scroll_row > 0 &&
        tab->query_result_scroll_row >= tab->query_results->num_rows)
      tab->query_result_scroll_row = tab->query_results->num_rows > 0
                                         ? tab->query_results->num_rows - 1
                                         : 0;
  } else {
    tui_set_error(state, "Delete failed: %s", err ? err : "unknown error");
    free(err);
  }
}

/* Handle edit input for query results */
static bool query_result_handle_edit_input(TuiState *state, Tab *tab,
                                           const UiEvent *event) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!ui || !ui->query_result_editing)
    return false;
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  size_t len =
      ui->query_result_edit_buf ? strlen(ui->query_result_edit_buf) : 0;
  int key_char = render_event_get_char(event);

  /* Escape - cancel */
  if (render_event_is_special(event, UI_KEY_ESCAPE)) {
    query_result_cancel_edit(state, tab);
    return true;
  }

  /* Enter - confirm */
  if (render_event_is_special(event, UI_KEY_ENTER)) {
    query_result_confirm_edit(state, tab);
    return true;
  }

  /* Left arrow */
  if (render_event_is_special(event, UI_KEY_LEFT)) {
    if (ui->query_result_edit_pos > 0) {
      ui->query_result_edit_pos--;
    }
    return true;
  }

  /* Right arrow */
  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    if (ui->query_result_edit_pos < len) {
      ui->query_result_edit_pos++;
    }
    return true;
  }

  /* Home */
  if (render_event_is_special(event, UI_KEY_HOME)) {
    ui->query_result_edit_pos = 0;
    return true;
  }

  /* End */
  if (render_event_is_special(event, UI_KEY_END)) {
    ui->query_result_edit_pos = len;
    return true;
  }

  /* Backspace */
  if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
    if (ui->query_result_edit_pos > 0 && ui->query_result_edit_buf) {
      memmove(ui->query_result_edit_buf + ui->query_result_edit_pos - 1,
              ui->query_result_edit_buf + ui->query_result_edit_pos,
              len - ui->query_result_edit_pos + 1);
      ui->query_result_edit_pos--;
    }
    return true;
  }

  /* Delete */
  if (render_event_is_special(event, UI_KEY_DELETE)) {
    if (ui->query_result_edit_pos < len && ui->query_result_edit_buf) {
      memmove(ui->query_result_edit_buf + ui->query_result_edit_pos,
              ui->query_result_edit_buf + ui->query_result_edit_pos + 1,
              len - ui->query_result_edit_pos);
    }
    return true;
  }

  /* Ctrl+U - clear line */
  if (render_event_is_ctrl(event, 'U')) {
    if (ui->query_result_edit_buf) {
      ui->query_result_edit_buf[0] = '\0';
      ui->query_result_edit_pos = 0;
    }
    return true;
  }

  /* Ctrl+N - set to NULL */
  if (render_event_is_ctrl(event, 'N')) {
    free(ui->query_result_edit_buf);
    ui->query_result_edit_buf = NULL;
    ui->query_result_edit_pos = 0;
    query_result_confirm_edit(state, tab);
    return true;
  }

  /* Printable character - insert */
  if (render_event_is_char(event) && key_char >= 32 && key_char < 127) {
    size_t new_len = len + 2;
    char *new_buf = realloc(ui->query_result_edit_buf, new_len);
    if (new_buf) {
      ui->query_result_edit_buf = new_buf;
      if (len == 0) {
        ui->query_result_edit_buf[0] = (char)key_char;
        ui->query_result_edit_buf[1] = '\0';
        ui->query_result_edit_pos = 1;
      } else {
        memmove(ui->query_result_edit_buf + ui->query_result_edit_pos + 1,
                ui->query_result_edit_buf + ui->query_result_edit_pos,
                len - ui->query_result_edit_pos + 1);
        ui->query_result_edit_buf[ui->query_result_edit_pos] = (char)key_char;
        ui->query_result_edit_pos++;
      }
    }
    return true;
  }

  /* Consume all other keys when editing */
  return true;
}

/* Handle query tab input */
bool tui_handle_query_input(TuiState *state, const UiEvent *event) {
  if (!state)
    return false;
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || tab->type != TAB_TYPE_QUERY || !ui)
    return false;

  int key_char = render_event_get_char(event);
  const Config *cfg = state->app ? state->app->config : NULL;

  /* Handle edit mode first if active */
  if (ui->query_result_editing) {
    return query_result_handle_edit_input(state, tab, event);
  }

  /* Ctrl+W or Esc toggles focus between editor and results */
  if (hotkey_matches(cfg, event, HOTKEY_QUERY_SWITCH_FOCUS)) {
    ui->query_focus_results = !ui->query_focus_results;
    return true;
  }

  /* Handle results navigation when focused on results */
  if (ui->query_focus_results) {
    if (!tab->query_results)
      return false;

    /* Enter - start editing */
    if (hotkey_matches(cfg, event, HOTKEY_EDIT_INLINE)) {
      query_result_start_edit(state, tab);
      return true;
    }

    /* e or F4 - start modal editing */
    if (hotkey_matches(cfg, event, HOTKEY_EDIT_MODAL)) {
      query_result_start_modal_edit(state, tab);
      return true;
    }

    /* Ctrl+N or n - set cell to NULL */
    if (hotkey_matches(cfg, event, HOTKEY_SET_NULL)) {
      query_result_set_cell_direct(state, tab, true);
      return true;
    }

    /* Ctrl+D or d - set cell to empty string */
    if (hotkey_matches(cfg, event, HOTKEY_SET_EMPTY)) {
      query_result_set_cell_direct(state, tab, false);
      return true;
    }

    /* x or Delete - delete row */
    if (hotkey_matches(cfg, event, HOTKEY_DELETE_ROW)) {
      query_result_delete_row(state, tab);
      return true;
    }

    /* r/R or Ctrl+R - refresh query results */
    if (hotkey_matches(cfg, event, HOTKEY_REFRESH) ||
        hotkey_matches(cfg, event, HOTKEY_EXECUTE_QUERY)) {
      /* Re-execute the base SQL if available, otherwise find query at cursor.
       * IMPORTANT: Must copy the SQL before calling query_execute because
       * query_execute frees tab->query_base_sql before re-using it. */
      char *sql_copy = NULL;
      if (tab->query_base_sql && *tab->query_base_sql) {
        sql_copy = str_dup(tab->query_base_sql);
      } else if (tab->query_text && *tab->query_text) {
        /* Fall back to finding query at cursor */
        sql_copy = query_find_at_cursor(tab->query_text, tab->query_cursor);
      }
      if (sql_copy && *sql_copy) {
        query_execute(state, sql_copy);
      }
      free(sql_copy);
      return true;
    }

    /* Up / k - move cursor up */
    if (hotkey_matches(cfg, event, HOTKEY_MOVE_UP)) {
      if (tab->query_result_row > 0) {
        tab->query_result_row--;
        /* Adjust scroll */
        if (tab->query_result_row < tab->query_result_scroll_row) {
          tab->query_result_scroll_row = tab->query_result_row;
        }
        query_check_load_more(state, tab);
      } else {
        /* At top row - switch focus to editor */
        ui->query_focus_results = false;
      }
      return true;
    }

    /* Down / j - move cursor down */
    if (hotkey_matches(cfg, event, HOTKEY_MOVE_DOWN)) {
      if (tab->query_results->num_rows > 0 &&
          tab->query_result_row < tab->query_results->num_rows - 1) {
        tab->query_result_row++;
        /* Adjust scroll based on actual main window height */
        int down_rows, down_cols;
        getmaxyx(state->main_win, down_rows, down_cols);
        (void)down_cols;
        int editor_height = (down_rows - 1) * 3 / 10;
        if (editor_height < 3)
          editor_height = 3;
        int visible = down_rows - editor_height - 4;
        if (visible < 1)
          visible = 1;
        if (tab->query_result_row >=
            tab->query_result_scroll_row + (size_t)visible) {
          tab->query_result_scroll_row = tab->query_result_row - visible + 1;
        }
        query_check_load_more(state, tab);
      }
      return true;
    }

    /* Left / h - move cursor left */
    if (hotkey_matches(cfg, event, HOTKEY_MOVE_LEFT)) {
      if (tab->query_result_col > 0) {
        tab->query_result_col--;
        /* Adjust horizontal scroll */
        if (tab->query_result_col < tab->query_result_scroll_col) {
          tab->query_result_scroll_col = tab->query_result_col;
        }
      } else if (state->sidebar_visible) {
        /* At left-most column - move focus to sidebar */
        state->sidebar_focused = true;
        /* Restore last sidebar position */
        state->sidebar_highlight = state->sidebar_last_position;
      }
      return true;
    }

    /* Right / l - move cursor right */
    if (hotkey_matches(cfg, event, HOTKEY_MOVE_RIGHT)) {
      if (tab->query_result_col < tab->query_results->num_columns - 1) {
        tab->query_result_col++;
        /* Adjust horizontal scroll to keep cursor visible using actual main
         * window */
        int right_rows, right_cols;
        getmaxyx(state->main_win, right_rows, right_cols);
        (void)right_rows;
        int avail_width = right_cols;
        int x = 1;
        size_t last_visible = tab->query_result_scroll_col;
        for (size_t col = tab->query_result_scroll_col;
             col < tab->query_results->num_columns; col++) {
          int w = tab->query_result_col_widths
                      ? tab->query_result_col_widths[col]
                      : 15;
          if (x + w + 3 > avail_width)
            break;
          x += w + 1;
          last_visible = col;
        }
        if (tab->query_result_col > last_visible) {
          tab->query_result_scroll_col = tab->query_result_col;
          /* Adjust to show as many columns as possible */
          x = 1;
          while (tab->query_result_scroll_col > 0) {
            int w =
                tab->query_result_col_widths
                    ? tab->query_result_col_widths[tab->query_result_scroll_col]
                    : 15;
            if (x + w + 3 > avail_width)
              break;
            x += w + 1;
            if (tab->query_result_scroll_col == tab->query_result_col)
              break;
            tab->query_result_scroll_col--;
          }
        }
      }
      return true;
    }

    /* Home */
    if (hotkey_matches(cfg, event, HOTKEY_FIRST_COL)) {
      tab->query_result_col = 0;
      tab->query_result_scroll_col = 0;
      return true;
    }

    /* End */
    if (hotkey_matches(cfg, event, HOTKEY_LAST_COL)) {
      if (tab->query_results->num_columns > 0) {
        tab->query_result_col = tab->query_results->num_columns - 1;
        /* Adjust horizontal scroll to show last column using actual main window
         */
        int end_rows, end_cols;
        getmaxyx(state->main_win, end_rows, end_cols);
        (void)end_rows;
        int avail_width = end_cols;
        tab->query_result_scroll_col = tab->query_result_col;
        /* Adjust to show as many columns as possible */
        int x = 1;
        while (tab->query_result_scroll_col > 0) {
          int w =
              tab->query_result_col_widths
                  ? tab->query_result_col_widths[tab->query_result_scroll_col]
                  : 15;
          if (x + w + 3 > avail_width)
            break;
          x += w + 1;
          if (tab->query_result_scroll_col == tab->query_result_col)
            break;
          tab->query_result_scroll_col--;
        }
      }
      return true;
    }

    /* Page Up */
    if (hotkey_matches(cfg, event, HOTKEY_PAGE_UP)) {
      /* Calculate visible rows in results area using actual main window */
      int ppage_rows, ppage_cols;
      getmaxyx(state->main_win, ppage_rows, ppage_cols);
      (void)ppage_cols;
      int editor_height = (ppage_rows - 1) * 3 / 10;
      if (editor_height < 3)
        editor_height = 3;
      int visible =
          ppage_rows - editor_height - 4; /* results area minus headers */
      if (visible < 1)
        visible = 1;

      if (tab->query_result_row > (size_t)visible) {
        tab->query_result_row -= visible;
      } else {
        tab->query_result_row = 0;
      }
      /* Adjust scroll to keep cursor visible */
      if (tab->query_result_row < tab->query_result_scroll_row) {
        tab->query_result_scroll_row = tab->query_result_row;
      }
      query_check_load_more(state, tab);
      return true;
    }

    /* Page Down */
    if (hotkey_matches(cfg, event, HOTKEY_PAGE_DOWN)) {
      /* Calculate visible rows in results area using actual main window */
      int npage_rows, npage_cols;
      getmaxyx(state->main_win, npage_rows, npage_cols);
      (void)npage_cols;
      int editor_height = (npage_rows - 1) * 3 / 10;
      if (editor_height < 3)
        editor_height = 3;
      int visible = npage_rows - editor_height - 4;
      if (visible < 1)
        visible = 1;

      tab->query_result_row += visible;
      if (tab->query_result_row >= tab->query_results->num_rows) {
        tab->query_result_row = tab->query_results->num_rows > 0
                                    ? tab->query_results->num_rows - 1
                                    : 0;
      }
      /* Adjust scroll to keep cursor visible */
      if (tab->query_result_row >=
          tab->query_result_scroll_row + (size_t)visible) {
        tab->query_result_scroll_row = tab->query_result_row - visible + 1;
      }
      query_check_load_more(state, tab);
      return true;
    }

    /* g or a - go to first row */
    if (hotkey_matches(cfg, event, HOTKEY_FIRST_ROW)) {
      tab->query_result_row = 0;
      tab->query_result_scroll_row = 0;
      query_check_load_more(state, tab);
      return true;
    }

    /* G or z - go to last row */
    if (hotkey_matches(cfg, event, HOTKEY_LAST_ROW)) {
      if (tab->query_results->num_rows > 0) {
        tab->query_result_row = tab->query_results->num_rows - 1;
      }
      query_check_load_more(state, tab);
      return true;
    }

    return false;
  }

  /* Handle editor input */

  /* Ctrl+R - run query under cursor */
  if (hotkey_matches(cfg, event, HOTKEY_EXECUTE_QUERY)) {
    char *query = query_find_at_cursor(tab->query_text, tab->query_cursor);
    if (query && *query) {
      query_execute(state, query);
    } else {
      tui_set_error(state, "No query at cursor");
    }
    free(query);
    return true;
  }

  /* Ctrl+A - run all queries */
  if (hotkey_matches(cfg, event, HOTKEY_EXECUTE_ALL)) {
    if (!tab->query_text || !*tab->query_text) {
      tui_set_error(state, "No queries to execute");
      return true;
    }

    /* Execute all queries separated by semicolons */
    char *text = str_dup(tab->query_text);
    char *p = text;
    int count = 0;
    int errors = 0;

    while (p && *p) {
      /* Skip whitespace */
      while (*p && isspace((unsigned char)*p))
        p++;
      if (!*p)
        break;

      /* Find end of query */
      char *end = p;
      bool in_string = false;
      char quote_char = 0;
      while (*end) {
        if (in_string) {
          if (*end == quote_char && (end == p || *(end - 1) != '\\')) {
            in_string = false;
          }
        } else {
          if (*end == '\'' || *end == '"') {
            in_string = true;
            quote_char = *end;
          } else if (*end == ';') {
            break;
          }
        }
        end++;
      }

      /* Extract query */
      char saved = *end;
      *end = '\0';

      /* Trim */
      char *q = p;
      while (*q && isspace((unsigned char)*q))
        q++;
      size_t qlen = strlen(q);
      while (qlen > 0 && isspace((unsigned char)q[qlen - 1]))
        qlen--;

      if (qlen > 0) {
        char *query = malloc(qlen + 1);
        if (query) {
          memcpy(query, q, qlen);
          query[qlen] = '\0';
          query_execute(state, query);
          count++;
          if (tab->query_error) {
            errors++;
          }
          free(query);
        }
      }

      *end = saved;
      p = *end ? end + 1 : end;
    }

    free(text);

    if (errors > 0) {
      tui_set_error(state, "Executed %d queries, %d errors", count, errors);
    } else {
      tui_set_status(state, "Executed %d queries", count);
    }
    return true;
  }

  /* Ctrl+T - run all queries in a transaction */
  if (hotkey_matches(cfg, event, HOTKEY_EXECUTE_TRANSACTION)) {
    if (!tab->query_text || !*tab->query_text) {
      tui_set_error(state, "No queries to execute");
      return true;
    }

    if (!state->conn) {
      tui_set_error(state, "Not connected to database");
      return true;
    }

    /* Start transaction */
    char *err = NULL;
    db_exec(state->conn, "BEGIN", &err);
    if (err) {
      tui_set_error(state, "Failed to start transaction: %s", err);
      free(err);
      return true;
    }

    /* Execute all queries separated by semicolons */
    char *text = str_dup(tab->query_text);
    char *p = text;
    int count = 0;
    bool had_error = false;

    while (p && *p && !had_error) {
      /* Skip whitespace */
      while (*p && isspace((unsigned char)*p))
        p++;
      if (!*p)
        break;

      /* Find end of query */
      char *end = p;
      bool in_string = false;
      char quote_char = 0;
      while (*end) {
        if (in_string) {
          if (*end == quote_char && (end == p || *(end - 1) != '\\')) {
            in_string = false;
          }
        } else {
          if (*end == '\'' || *end == '"') {
            in_string = true;
            quote_char = *end;
          } else if (*end == ';') {
            break;
          }
        }
        end++;
      }

      /* Extract query */
      char saved = *end;
      *end = '\0';

      /* Trim */
      char *q = p;
      while (*q && isspace((unsigned char)*q))
        q++;
      size_t qlen = strlen(q);
      while (qlen > 0 && isspace((unsigned char)q[qlen - 1]))
        qlen--;

      if (qlen > 0) {
        char *query = malloc(qlen + 1);
        if (query) {
          memcpy(query, q, qlen);
          query[qlen] = '\0';
          query_execute(state, query);
          count++;
          if (tab->query_error) {
            had_error = true;
          }
          free(query);
        }
      }

      *end = saved;
      p = *end ? end + 1 : end;
    }

    free(text);

    /* Commit or rollback */
    err = NULL;
    if (had_error) {
      db_exec(state->conn, "ROLLBACK", &err);
      free(err);
      tui_set_error(state, "Transaction rolled back after error in query %d",
                    count);
    } else {
      db_exec(state->conn, "COMMIT", &err);
      if (err) {
        tui_set_error(state, "Commit failed: %s", err);
        free(err);
      } else {
        tui_set_status(state, "Transaction committed (%d queries)", count);
      }
    }
    return true;
  }

  /* Up arrow - move cursor up one line */
  if (render_event_is_special(event, UI_KEY_UP)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    if (line > 0) {
      tab->query_cursor =
          query_line_col_to_cursor(tab, line - 1, col, lines, num_lines);
    }

    free(lines);
    return true;
  }

  /* Down arrow - move cursor down one line */
  if (render_event_is_special(event, UI_KEY_DOWN)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    if (line < num_lines - 1) {
      tab->query_cursor =
          query_line_col_to_cursor(tab, line + 1, col, lines, num_lines);
    } else if (tab->query_results && tab->query_results->num_rows > 0) {
      /* On last line - move focus to results panel */
      ui->query_focus_results = true;
    }

    free(lines);
    return true;
  }

  /* Left arrow */
  if (render_event_is_special(event, UI_KEY_LEFT)) {
    if (tab->query_cursor > 0) {
      tab->query_cursor--;
    } else if (state->sidebar_visible) {
      /* At top-left position - move focus to sidebar */
      state->sidebar_focused = true;
      /* Restore last sidebar position */
      state->sidebar_highlight = state->sidebar_last_position;
    }
    return true;
  }

  /* Right arrow */
  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    if (tab->query_cursor < tab->query_len) {
      tab->query_cursor++;
    } else if (tab->query_results && tab->query_results->num_rows > 0) {
      /* At end of text - move focus to results panel */
      ui->query_focus_results = true;
    }
    return true;
  }

  /* Home - move to start of line */
  if (render_event_is_special(event, UI_KEY_HOME)) {
    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    if (lines && line < num_lines) {
      tab->query_cursor = lines[line].start;
    }
    free(lines);
    return true;
  }

  /* End or Ctrl+E - move to end of line */
  if (render_event_is_special(event, UI_KEY_END) ||
      render_event_is_ctrl(event, 'E')) {
    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    if (lines && line < num_lines) {
      tab->query_cursor = lines[line].start + lines[line].len;
    }
    free(lines);
    return true;
  }

  /* Page Up */
  if (render_event_is_special(event, UI_KEY_PAGEUP)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    if (line > 10) {
      tab->query_cursor =
          query_line_col_to_cursor(tab, line - 10, col, lines, num_lines);
    } else {
      tab->query_cursor =
          query_line_col_to_cursor(tab, 0, col, lines, num_lines);
    }

    free(lines);
    return true;
  }

  /* Page Down */
  if (render_event_is_special(event, UI_KEY_PAGEDOWN)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    tab->query_cursor =
        query_line_col_to_cursor(tab, line + 10, col, lines, num_lines);

    free(lines);
    return true;
  }

  /* Backspace */
  if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
    query_backspace(tab);
    return true;
  }

  /* Delete */
  if (render_event_is_special(event, UI_KEY_DELETE)) {
    query_delete_char(tab);
    return true;
  }

  /* Enter - insert newline */
  if (render_event_is_special(event, UI_KEY_ENTER)) {
    query_insert_char(tab, '\n');
    return true;
  }

  /* Ctrl+U - clear line */
  if (render_event_is_ctrl(event, 'U')) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    if (lines && line < num_lines) {
      /* Delete from line start to cursor */
      size_t start = lines[line].start;
      size_t count = tab->query_cursor - start;
      if (count > 0) {
        memmove(tab->query_text + start, tab->query_text + tab->query_cursor,
                tab->query_len - tab->query_cursor + 1);
        tab->query_len -= count;
        tab->query_cursor = start;
      }
    }
    free(lines);
    return true;
  }

  /* Printable character - insert */
  if (render_event_is_char(event) && key_char >= 32 && key_char < 127) {
    query_insert_char(tab, (char)key_char);
    return true;
  }

  return false;
}
