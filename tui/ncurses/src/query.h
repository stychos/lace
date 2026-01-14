/*
 * Lace ncurses frontend
 * Query tab - SQL editor with results
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FRONTEND_QUERY_H
#define LACE_FRONTEND_QUERY_H

#include "tui.h"

/* Initial query buffer capacity */
#define QUERY_INITIAL_CAPACITY 4096

/* ==========================================================================
 * Query Tab State
 * ========================================================================== */

typedef struct {
  /* Editor state */
  char *text;             /* Query text buffer */
  size_t text_len;        /* Current text length */
  size_t text_cap;        /* Buffer capacity */
  size_t cursor;          /* Cursor position */
  size_t scroll_line;     /* First visible line */
  size_t scroll_col;      /* Horizontal scroll */

  /* Results state */
  LaceResult *results;    /* Query results */
  char *error;            /* Error message */
  size_t result_row;      /* Result cursor row */
  size_t result_col;      /* Result cursor column */
  size_t result_scroll_row;
  size_t result_scroll_col;
  int *col_widths;        /* Column widths for display */

  /* Execution state */
  int64_t affected_rows;  /* Rows affected by non-SELECT */
  bool exec_success;      /* Last exec was successful */
  char *base_sql;         /* Base SQL for pagination */
  size_t total_rows;      /* Total rows (for paginated) */
  size_t loaded_offset;   /* Offset of loaded page */
  bool paginated;         /* Whether results are paginated */

  /* Focus state */
  bool focus_results;     /* Focus on results vs editor */

  /* Edit state for result cells */
  bool editing;           /* Editing a result cell */
  char edit_buffer[1024]; /* Edit buffer */
  size_t edit_pos;        /* Edit cursor position */
} QueryState;

/* ==========================================================================
 * Query Functions
 * ========================================================================== */

/*
 * Initialize query state.
 */
void query_init(QueryState *qs);

/*
 * Free query state resources.
 */
void query_free(QueryState *qs);

/*
 * Execute query text at cursor position.
 */
bool query_execute_at_cursor(TuiState *tui, QueryState *qs);

/*
 * Execute all queries in buffer.
 */
bool query_execute_all(TuiState *tui, QueryState *qs);

/*
 * Execute specific SQL string.
 */
bool query_execute(TuiState *tui, QueryState *qs, const char *sql);

/*
 * Draw the query tab.
 */
void query_draw(TuiState *tui, QueryState *qs, WINDOW *win);

/*
 * Handle query tab input.
 * Returns true if input was handled.
 */
bool query_handle_input(TuiState *tui, QueryState *qs, int ch);

/*
 * Insert character at cursor.
 */
void query_insert_char(QueryState *qs, char c);

/*
 * Delete character before cursor (backspace).
 */
void query_backspace(QueryState *qs);

/*
 * Delete character at cursor.
 */
void query_delete_char(QueryState *qs);

/*
 * Get line info for cursor navigation.
 */
void query_cursor_to_line_col(QueryState *qs, size_t *line, size_t *col);

/*
 * Move cursor to specific line/col.
 */
void query_set_cursor_line_col(QueryState *qs, size_t line, size_t col);

#endif /* LACE_FRONTEND_QUERY_H */
