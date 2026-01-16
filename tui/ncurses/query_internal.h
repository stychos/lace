/*
 * Lace
 * Query tab internal declarations
 *
 * Shared types and declarations for query tab modules.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_QUERY_INTERNAL_H
#define LACE_QUERY_INTERNAL_H

#include "tui_internal.h"

#define QUERY_INITIAL_CAPACITY 1024
#define QUERY_MAX_SIZE (64 * 1024 * 1024) /* 64 MB max query size */

/* Line info for cursor tracking */
typedef struct {
  size_t start; /* Byte offset of line start */
  size_t len;   /* Length of line (excluding newline) */
} QueryLineInfo;

/* Primary key info for query result database operations */
typedef struct {
  const char **col_names;
  DbValue *values;
  size_t count;
} QueryPkInfo;

/* ============================================================================
 * query_editor.c - Text editing functions
 * ============================================================================
 */

/* Ensure query buffer has enough capacity. Returns false on allocation failure.
 */
bool query_ensure_capacity(Tab *tab, size_t needed);

/* Rebuild line cache from query text */
void query_rebuild_line_cache(Tab *tab, QueryLineInfo **lines,
                              size_t *num_lines);

/* Convert cursor offset to line/column */
void query_cursor_to_line_col(Tab *tab, size_t *line, size_t *col);

/* Convert line/column to cursor offset */
size_t query_line_col_to_cursor(Tab *tab, size_t line, size_t col,
                                QueryLineInfo *lines, size_t num_lines);

/* Insert character at cursor */
void query_insert_char(Tab *tab, char c);

/* Delete character at cursor (forward delete) */
void query_delete_char(Tab *tab);

/* Delete character before cursor (backspace) */
void query_backspace(Tab *tab);

/* Find the byte boundaries of the query at cursor position.
 * Returns true if a valid query range was found. */
bool query_find_bounds_at_cursor(const char *text, size_t cursor,
                                 size_t *out_start, size_t *out_end);

/* Find the query at cursor position (caller must free result) */
char *query_find_at_cursor(const char *text, size_t cursor);

/* ============================================================================
 * query_executor.c - Query execution and pagination
 * ============================================================================
 */

/* Extract table name from a simple SELECT query */
char *query_extract_table_name(const char *sql);

/* Check if query has LIMIT or OFFSET clause */
bool query_has_limit_offset(const char *sql);

/* Count total rows for a SELECT query using COUNT wrapper */
int64_t query_count_rows(TuiState *state, const char *base_sql);

/* Execute a SQL query and store results */
void query_execute(TuiState *state, const char *sql);

/* Calculate column widths for query results */
void query_calculate_result_widths(Tab *tab);

/* Load more rows at end of current query results */
bool query_load_more_rows(TuiState *state, Tab *tab);

/* Load previous rows (prepend to current query results) */
bool query_load_prev_rows(TuiState *state, Tab *tab);

/* Trim loaded query data to keep memory bounded */
void query_trim_loaded_data(TuiState *state, Tab *tab);

/* Check if more rows need to be loaded based on cursor position */
void query_check_load_more(TuiState *state, Tab *tab);

/* ============================================================================
 * query_results.c - Result grid editing functions
 * ============================================================================
 */

/* Get column width for query results */
int query_get_col_width(Tab *tab, size_t col);

/* Start editing a cell in query results (inline or modal based on content) */
void query_result_start_edit(TuiState *state, Tab *tab);

/* Start modal editing for query results (always uses modal) */
void query_result_start_modal_edit(TuiState *state, Tab *tab);

/* Confirm edit and update database */
void query_result_confirm_edit(TuiState *state, Tab *tab);

/* Cancel editing in query results */
void query_result_cancel_edit(TuiState *state, Tab *tab);

/* Find column index in result set by name */
size_t query_find_column_by_name(Tab *tab, const char *name);

/* Find primary key columns in query results */
size_t query_find_pk_columns(Tab *tab, size_t *pk_indices, size_t max_pks);

/* Build PK info from query result row. Returns false on error. */
bool query_pk_info_build(QueryPkInfo *pk, Tab *tab, size_t row_idx);

/* Free PK info resources */
void query_pk_info_free(QueryPkInfo *pk);

/* Set query result cell directly to NULL or empty string */
void query_result_set_cell_direct(TuiState *state, Tab *tab, bool set_null);

/* Copy query result cell to clipboard */
void query_result_cell_copy(TuiState *state, Tab *tab);

/* Paste clipboard content to query result cell */
void query_result_cell_paste(TuiState *state, Tab *tab);

/* Delete row(s) from query results - bulk if selections exist */
void query_result_delete_row(TuiState *state, Tab *tab);

/* Handle edit input for query results */
bool query_result_handle_edit_input(TuiState *state, Tab *tab,
                                    const UiEvent *event);

#endif /* LACE_QUERY_INTERNAL_H */
