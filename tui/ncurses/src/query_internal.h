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

/* Convert a line and display column (tabs 4 wide, UTF-8 aware) to a cursor
 * offset, landing on a character boundary */
size_t query_line_col_to_cursor(Tab *tab, size_t line, size_t col,
                                QueryLineInfo *lines, size_t num_lines);

/* Display width of the first n bytes of an editor line (tabs 4 wide) */
size_t query_display_width(const char *text, size_t n);

/* Display column of the cursor within its line */
size_t query_cursor_display_col(Tab *tab);

/* Insert character at cursor */
void query_insert_char(Tab *tab, char c);

/* Delete character at cursor (forward delete) */
void query_delete_char(Tab *tab);

/* Delete character before cursor (backspace) */
void query_backspace(Tab *tab);

/* Byte range of the statement at the cursor (its ';' included), split like
 * "run all" splits; in blank space after a ';' it is the previous statement.
 * Ctrl+R runs exactly what this range highlights. */
bool query_find_bounds_at_cursor(const char *text, size_t cursor,
                                 const char *driver, size_t *out_start,
                                 size_t *out_end);

/* Find the query at cursor position (caller must free result) */
char *query_find_at_cursor(const char *text, size_t cursor, const char *driver);

/* What the SQL scanners must skip, per dialect */
typedef struct {
  bool mysql;    /* backslash escapes in strings, # comments, `ident` */
  bool sqlite;   /* `ident` and [ident] */
  bool postgres; /* $tag$ ... $tag$ bodies */
} SqlDialect;

SqlDialect query_sql_dialect(const char *driver);
SqlDialect query_sql_dialect_of(LaceDriver driver);

/* If s starts a string literal, quoted identifier, comment or PostgreSQL
 * dollar-quoted body, return its length (to the end of text if unclosed) and
 * set *is_comment; otherwise return 0. Every SQL scanner in the query tab
 * uses this, so they agree on what is code and what is not. */
size_t query_sql_skip(const char *s, SqlDialect d, bool *is_comment);

/* Length of the SQL statement at p up to its terminating ';' (or end).
 * Dialect-aware: quotes, comments, and PostgreSQL $$ bodies. */
size_t query_statement_length(const char *p, const char *driver);

/* Lexical shape of one statement (strings, comments, $tag$ bodies skipped):
 * its first keyword (upper case), whether it has a LIMIT, OFFSET or FETCH at
 * the top level (outside parentheses), and where it really ends (before any
 * trailing ';', whitespace or comment). */
typedef struct {
  char first_keyword[16];
  char verb[8];          /* First top-level SELECT/INSERT/UPDATE/DELETE/MERGE/
                            REPLACE/VALUES/TABLE/UPSERT (after any WITH list) */
  bool top_level_into;   /* SELECT ... INTO (new table, OUTFILE, variables) */
  bool top_level_limit;
  bool nested_write;     /* Not read-only by lace_sql_read_only(): a writing
                            verb at any depth (a data-modifying WITH) or INTO */
  size_t end;
} QuerySqlShape;

/* True for a statement that only reads and can take LIMIT/OFFSET: its verb
 * is SELECT, VALUES or TABLE (also behind a WITH list), it has no
 * top-level INTO, and nothing in it writes (WITH u AS (UPDATE ...) SELECT
 * would repeat the UPDATE for every page and count). Anything else must run exactly once, as written. */
bool query_sql_pageable(const QuerySqlShape *shape);

/* A page must wrap the statement in "SELECT * FROM (...) AS lace_page"
 * instead of appending LIMIT: it has its own top-level LIMIT/OFFSET/FETCH,
 * or it is a VALUES list (SQLite takes no LIMIT after VALUES) */
bool query_page_needs_wrap(const QuerySqlShape *shape);
void query_sql_shape(const char *sql, const char *driver, QuerySqlShape *out);

/* What Ctrl+R, Ctrl+A and Ctrl+T run from the editor */
typedef enum {
  QUERY_RUN_AT_CURSOR,   /* The statement at the cursor */
  QUERY_RUN_ALL,         /* Every statement; errors do not stop the run */
  QUERY_RUN_TRANSACTION, /* Every statement inside BEGIN; the first error or
                            a cancel rolls back, otherwise COMMIT */
} QueryRunMode;

/* Run the current query tab's text in the given mode, reporting the outcome
 * in the status bar. A cancel stops the run. */
void query_run_script(TuiState *state, QueryRunMode mode);

/* Collect the background row count of the current query tab when done */
void query_poll_count(TuiState *state);

/* ============================================================================
 * query_executor.c - Query execution and pagination
 * ============================================================================
 */

/* Extract table name from a simple SELECT query */
char *query_extract_table_name(const char *sql, LaceDriver driver);

/* Check if query has LIMIT or OFFSET clause */

/* Count total rows for a SELECT query using COUNT wrapper */
/* SQL for one page of a paginated query tab */
char *query_page_sql(const Tab *tab, size_t limit, size_t offset);

/* Run a row query on a worker behind the processing dialog (spinner, Esc
 * cancels). Returns rows, or NULL with *err set or *cancelled true. */
ResultSet *query_run_modal(TuiState *state, const char *sql,
                           const char *message, char **err, bool *cancelled);

/* query_run_modal for SQL the TUI derived from the user's statement (pages,
 * counts). In a PostgreSQL transaction it runs under a savepoint, so a
 * failure or cancel does not abort the user's transaction; in a failed one
 * it refuses. */
ResultSet *query_run_derived(TuiState *state, const char *sql,
                             const char *message, char **err,
                             bool *cancelled);

/* "Loaded N/M rows" (or N/M+ while counting) in the status bar */
void query_status_loaded(TuiState *state, Tab *tab);

/* Make query_total_rows exact for a paginated tab (counts in the foreground,
 * cancellable, if the background count has not finished). */
bool query_ensure_total(TuiState *state, Tab *tab);

/* Execute a SQL query and store results */
void query_execute(TuiState *state, const char *sql);

/* True if the last query_execute() call was cancelled by the user */
bool query_last_execute_cancelled(void);

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

/* Rows of the SQL editor in a main window win_rows high (30%, at least 3);
 * the separator is the next row and the results start below it. Result
 * rows begin 3 lines into the results area (title, header, rule). */
int query_editor_height(int win_rows);
#define QUERY_RESULTS_HEADER_ROWS 3

/* Result rows that fit below the editor in the current main window (>= 1) */
int query_visible_result_rows(TuiState *state);

/* Start editing a cell in query results (inline or modal based on content) */
void query_result_start_edit(TuiState *state, Tab *tab);

/* Start modal editing for query results (always uses modal) */
void query_result_start_modal_edit(TuiState *state, Tab *tab);

/* Confirm edit and update database */
void query_result_confirm_edit(TuiState *state, Tab *tab);

/* Cancel editing in query results */
void query_result_cancel_edit(TuiState *state, Tab *tab);




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
