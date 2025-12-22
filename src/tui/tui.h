/*
 * lace - Database Viewer and Manager
 * TUI interface
 */

#ifndef LACE_TUI_H
#define LACE_TUI_H

#include "../async/async.h"
#include "../core/app_state.h"
#include "../db/db.h"
#include <ncurses.h>
#include <stdbool.h>

/* Color pairs */
#define COLOR_HEADER 1
#define COLOR_SELECTED 2
#define COLOR_STATUS 3
#define COLOR_ERROR 4
#define COLOR_BORDER 5
#define COLOR_TITLE 6
#define COLOR_NULL 7
#define COLOR_NUMBER 8
#define COLOR_EDIT 9

/* Sidebar width */
#define SIDEBAR_WIDTH 20

/* Tab bar height */
#define TAB_BAR_HEIGHT 1

/* Minimum terminal dimensions */
#define MIN_TERM_ROWS 10
#define MIN_TERM_COLS 40

/* TUI state - contains UI-specific state and reference to core AppState */
typedef struct {
  /* Core application state (platform-independent) */
  AppState *app;

  /* ncurses windows */
  WINDOW *main_win;
  WINDOW *status_win;
  WINDOW *header_win;
  WINDOW *sidebar_win;
  WINDOW *tab_win;

  /* Terminal dimensions */
  int term_rows;
  int term_cols;
  int content_rows;
  int content_cols;

  /* Editing mode (inline cell editing) */
  bool editing;
  char *edit_buffer;
  size_t edit_pos;

  /* Visibility toggles */
  bool header_visible;
  bool status_visible;

  /* Sidebar UI state */
  bool sidebar_visible;
  size_t sidebar_highlight;
  size_t sidebar_scroll;
  bool sidebar_focused;
  bool sidebar_filter_active;
  char sidebar_filter[64];
  size_t sidebar_filter_len;

  /* Sidebar name scroll animation */
  size_t sidebar_name_scroll;
  int sidebar_name_scroll_dir;
  int sidebar_name_scroll_delay;
  size_t sidebar_last_highlight;

  /* Track state before sidebar focus for restoration */
  bool filters_was_focused;    /* Was filters focused before sidebar? */
  size_t sidebar_last_position; /* Sidebar highlight before leaving */

  /* Filters panel state (synced from current workspace) */
  bool filters_visible;
  bool filters_focused;
  size_t filters_cursor_row;
  size_t filters_cursor_col;
  size_t filters_scroll;
  bool filters_editing;
  char filters_edit_buffer[256];
  size_t filters_edit_len;
  size_t filters_edit_pos;

  /* Status message */
  char *status_msg;
  bool status_is_error;

  /* Running flag */
  bool running;

  /* =========================================================================
   * COMPATIBILITY LAYER - View cache synced from AppState/Workspace
   * These will be removed in future refactoring phases.
   * Access through state->app or TUI_WORKSPACE() for new code.
   * =========================================================================
   */

  /* Cached from app->conn */
  DbConnection *conn;

  /* Cached from app->tables */
  char **tables;
  size_t num_tables;

  /* Cached from app->workspaces */
  Workspace *workspaces;
  size_t num_workspaces;
  size_t current_workspace;

  /* Cached from current workspace */
  size_t current_table;
  ResultSet *data;
  TableSchema *schema;
  size_t cursor_row;
  size_t cursor_col;
  size_t scroll_row;
  size_t scroll_col;
  size_t total_rows;
  size_t loaded_offset;
  size_t loaded_count;
  int *col_widths;
  size_t num_col_widths;
  size_t page_size;
} TuiState;

/* Sync view cache from AppState (call after app state changes) */
void tui_sync_from_app(TuiState *state);

/* Sync current workspace from view cache (call before workspace switch) */
void tui_sync_to_workspace(TuiState *state);

/* ============================================================================
 * Convenience macros for accessing app state
 * ============================================================================
 */

/* Get current workspace from TuiState */
#define TUI_WORKSPACE(state) app_current_workspace((state)->app)

/* Get current table data */
#define TUI_DATA(state) (TUI_WORKSPACE(state) ? TUI_WORKSPACE(state)->data : NULL)

/* Get current schema */
#define TUI_SCHEMA(state) (TUI_WORKSPACE(state) ? TUI_WORKSPACE(state)->schema : NULL)

/* App state shortcuts */
#define TUI_CONN(state) ((state)->app->conn)
#define TUI_TABLES(state) ((state)->app->tables)
#define TUI_NUM_TABLES(state) ((state)->app->num_tables)
#define TUI_NUM_WORKSPACES(state) ((state)->app->num_workspaces)
#define TUI_CURRENT_WS_IDX(state) ((state)->app->current_workspace)

/* Workspace field shortcuts (with null safety) */
static inline size_t tui_cursor_row(TuiState *state) {
  Workspace *ws = TUI_WORKSPACE(state);
  return ws ? ws->cursor_row : 0;
}
static inline size_t tui_cursor_col(TuiState *state) {
  Workspace *ws = TUI_WORKSPACE(state);
  return ws ? ws->cursor_col : 0;
}
static inline bool tui_filters_visible(TuiState *state) {
  Workspace *ws = TUI_WORKSPACE(state);
  return ws ? ws->filters_visible : false;
}
static inline bool tui_filters_focused(TuiState *state) {
  Workspace *ws = TUI_WORKSPACE(state);
  return ws ? ws->filters_focused : false;
}

/* ============================================================================
 * TUI lifecycle
 * ============================================================================
 */

/* Initialize TUI */
bool tui_init(TuiState *state, AppState *app);

/* Cleanup TUI */
void tui_cleanup(TuiState *state);

/* Connect to database */
bool tui_connect(TuiState *state, const char *connstr);

/* Disconnect */
void tui_disconnect(TuiState *state);

/* Main loop */
void tui_run(TuiState *state);

/* Refresh display */
void tui_refresh(TuiState *state);

/* ============================================================================
 * Drawing functions
 * ============================================================================
 */

void tui_draw_header(TuiState *state);
void tui_draw_table(TuiState *state);
void tui_draw_status(TuiState *state);
void tui_draw_sidebar(TuiState *state);
void tui_draw_tabs(TuiState *state);

/* ============================================================================
 * Data loading
 * ============================================================================
 */

bool tui_load_tables(TuiState *state);
bool tui_load_table_data(TuiState *state, const char *table);
bool tui_load_schema(TuiState *state, const char *table);

/* ============================================================================
 * Navigation
 * ============================================================================
 */

void tui_move_cursor(TuiState *state, int row_delta, int col_delta);
void tui_page_up(TuiState *state);
void tui_page_down(TuiState *state);
void tui_home(TuiState *state);
void tui_end(TuiState *state);

/* ============================================================================
 * Actions
 * ============================================================================
 */

void tui_next_table(TuiState *state);
void tui_prev_table(TuiState *state);
void tui_show_schema(TuiState *state);
void tui_show_connect_dialog(TuiState *state);
void tui_show_table_selector(TuiState *state);
void tui_show_help(TuiState *state);

/* ============================================================================
 * Status messages
 * ============================================================================
 */

void tui_set_status(TuiState *state, const char *fmt, ...);
void tui_set_error(TuiState *state, const char *fmt, ...);

/* ============================================================================
 * Utility
 * ============================================================================
 */

int tui_get_column_width(TuiState *state, size_t col);
void tui_calculate_column_widths(TuiState *state);

/* ============================================================================
 * Query tab
 * ============================================================================
 */

bool workspace_create_query(TuiState *state);
void tui_draw_query(TuiState *state);
bool tui_handle_query_input(TuiState *state, int ch);
void tui_query_start_result_edit(TuiState *state);
void tui_query_confirm_result_edit(TuiState *state);
void tui_query_scroll_results(TuiState *state, int delta);
bool query_load_rows_at(TuiState *state, Workspace *ws, size_t offset);

/* ============================================================================
 * Filters UI
 * ============================================================================
 */

void tui_draw_filters_panel(TuiState *state);
bool tui_handle_filters_input(TuiState *state, int ch);
void tui_apply_filters(TuiState *state);
int tui_get_filters_panel_height(TuiState *state);

/* ============================================================================
 * Async operations with progress dialog
 * ============================================================================
 */

/* Show processing dialog with spinner and cancel support */
bool tui_show_processing_dialog(TuiState *state, AsyncOperation *op,
                                const char *message);

/* Extended version with custom delay (0 = show immediately) */
bool tui_show_processing_dialog_ex(TuiState *state, AsyncOperation *op,
                                   const char *message, int delay_ms);

/* Connect to database with progress dialog */
DbConnection *tui_connect_with_progress(TuiState *state, const char *connstr);

/* Load table list with progress dialog */
bool tui_load_tables_with_progress(TuiState *state);

/* Count rows with progress dialog (uses approximate count if available) */
int64_t tui_count_rows_with_progress(TuiState *state, const char *table,
                                     bool *is_approximate);

/* Load table schema with progress dialog */
TableSchema *tui_get_schema_with_progress(TuiState *state, const char *table);

/* Query page with progress dialog */
ResultSet *tui_query_page_with_progress(TuiState *state, const char *table,
                                        size_t offset, size_t limit,
                                        const char *order_by, bool desc);

#endif /* LACE_TUI_H */
