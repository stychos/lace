/*
 * Lace
 * TUI interface
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_H
#define LACE_TUI_H

#include "../../async/async.h"
#include "../../core/app_state.h"
#include "../../core/constants.h"
#include "../../db/db.h"
#include "../../viewmodel/vm_app.h"
#include "../../viewmodel/focus_manager.h"
#include "../../viewmodel/viewmodel.h"
#include "../../viewmodel/filters_viewmodel.h"
#include "../../viewmodel/query_viewmodel.h"
#include "../../viewmodel/sidebar_viewmodel.h"
#include "../../viewmodel/table_viewmodel.h"
#include "backend.h"
#include <ncurses.h>
#include <stdbool.h>

/* Color pairs for ncurses - mapped from UiColor enum values.
 * Using UiColor values ensures consistency across backends.
 * ncurses uses these as color pair indices with init_pair(). */
#define COLOR_HEADER UI_COLOR_HEADER
#define COLOR_SELECTED UI_COLOR_SELECTED
#define COLOR_STATUS UI_COLOR_STATUS
#define COLOR_ERROR UI_COLOR_ERROR
#define COLOR_BORDER UI_COLOR_BORDER
#define COLOR_TITLE UI_COLOR_TITLE
#define COLOR_NULL UI_COLOR_NULL
#define COLOR_NUMBER UI_COLOR_NUMBER
#define COLOR_EDIT UI_COLOR_EDIT
#define COLOR_ERROR_TEXT UI_COLOR_ERROR_TEXT
#define COLOR_PK UI_COLOR_PK

/* UI dimensions are in core/constants.h:
 * SIDEBAR_WIDTH, TAB_BAR_HEIGHT, MIN_TERM_ROWS, MIN_TERM_COLS */

/* ============================================================================
 * UITabState - Per-tab UI state (TUI-specific)
 *
 * This struct holds UI-specific state that persists across tab switches but
 * should NOT be in the core AppState. This separation allows core/ to remain
 * platform-independent while TUI maintains its own UI state.
 *
 * Indexed by [workspace_index][tab_index] in TuiState.tab_ui array.
 *
 * MIGRATION NOTE: This struct is transitioning to a widget-based architecture.
 * New code should use the widget pointers. Legacy fields will be removed
 * after all code is migrated to use widgets.
 * ============================================================================
 */
typedef struct UITabState {
  /* =========================================================================
   * Widget-based state (NEW - use these for new code)
   * Widgets own their cursor, scroll, selection, edit state.
   * =========================================================================
   */
  TableWidget *table_widget;     /* Table data widget (NULL for query tabs) */
  SidebarWidget *sidebar_widget; /* Sidebar widget (shared across tabs) */
  FiltersWidget *filters_widget; /* Filters panel widget */
  QueryWidget *query_widget;     /* Query editor widget (NULL for table tabs) */

  /* Focus management via FocusManager */
  FocusManager focus_mgr;        /* Centralized focus/event routing */

  /* =========================================================================
   * Legacy fields (DEPRECATED - will be removed after migration)
   * Kept for backwards compatibility during gradual migration.
   * =========================================================================
   */

  /* Filter panel UI state (LEGACY - migrate to filters_widget) */
  bool filters_visible;
  bool filters_focused;
  bool filters_editing;
  bool filters_was_focused; /* Was filters focused before entering sidebar */
  size_t filters_cursor_row;
  size_t filters_cursor_col;
  size_t filters_scroll;

  /* Sidebar state per-tab (LEGACY - migrate to sidebar_widget) */
  bool sidebar_visible;
  bool sidebar_focused;
  size_t sidebar_highlight;
  size_t sidebar_scroll;
  size_t sidebar_last_position;
  char sidebar_filter[64];
  size_t sidebar_filter_len;

  /* Query tab UI state (LEGACY - migrate to query_widget) */
  bool query_focus_results;
  bool query_result_editing;
  char *query_result_edit_buf;
  size_t query_result_edit_pos;
} UITabState;

/* TUI state - contains UI-specific state and reference to core AppState */
struct TuiState {
  /* Core application state (platform-independent) */
  AppState *app;

  /* =========================================================================
   * ViewModels - Platform-independent view state
   * These provide a clean interface between TUI and core state.
   * VmSidebar and VmQuery removed - use widgets instead (Phase 9a/9b).
   * =========================================================================
   */
  VmApp *vm_app;     /* App-level viewmodel */
  VmTable *vm_table; /* Current table viewmodel */

  /* Render backend context (for gradual migration from direct ncurses) */
  RenderContext *render_ctx;

  /* ncurses windows (legacy - will be replaced by RenderBackend regions) */
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
  bool filters_was_focused;     /* Was filters focused before sidebar? */
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

  /* Internal clipboard buffer (fallback when external clipboard unavailable) */
  char *clipboard_buffer;

  /* Add row mode (temporary row being created) */
  bool adding_row;              /* True when in add-row mode */
  DbValue *new_row_values;      /* Array of values for new row (num_columns) */
  bool *new_row_placeholders;   /* Track which cells show placeholder
                                   (auto/default) */
  bool *new_row_auto_increment; /* Track which columns are auto_increment */
  bool *new_row_edited;         /* Track which cells user has edited */
  size_t new_row_num_cols;      /* Number of columns */
  size_t new_row_cursor_col;    /* Currently focused column in new row */
  char *new_row_edit_buffer;    /* Edit buffer for current cell */
  size_t new_row_edit_len;      /* Length of edit buffer content */
  size_t new_row_edit_pos;      /* Cursor position in edit buffer */
  bool new_row_cell_editing; /* True when editing a cell within the new row */

  /* Running flag */
  bool running;

  /* Background loading indicator */
  bool bg_loading_active;

  /* =========================================================================
   * Cached connection/tables for sidebar (performance optimization)
   * These are synced in tui_sync_from_app() when tab changes.
   * =========================================================================
   */

  /* Cached from current tab's connection */
  DbConnection *conn;
  char **tables;
  size_t num_tables;

  /* Cached from app->workspaces (for initialization/cleanup) */
  Workspace *workspaces;
  size_t num_workspaces;
  size_t current_workspace;

  /* Cached from app */
  size_t page_size;

  /* =========================================================================
   * Per-tab UI state (TUI-specific, indexed by [workspace][tab])
   * This replaces the UI fields that were previously stored in core Tab struct.
   * Dynamic 2D array: tab_ui[workspace_index][tab_index]
   * =========================================================================
   */
  UITabState **tab_ui;       /* Array of per-workspace UITabState arrays */
  size_t *tab_ui_capacity;   /* Capacity per workspace */
  size_t tab_ui_ws_capacity; /* Capacity for workspace dimension */
};
typedef struct TuiState TuiState;

/* Sync view cache from AppState (call after app state changes) */
void tui_sync_from_app(TuiState *state);

/* Sync current workspace from view cache (call before workspace switch) */
void tui_sync_to_workspace(TuiState *state);

/* ============================================================================
 * Convenience macros for accessing app state hierarchy
 *
 * New architecture:
 *   - Connections are a flat pool at AppState level
 *   - Workspaces are independent at AppState level
 *   - Each Tab has a connection_index field referencing which connection it
 * uses
 * ============================================================================
 */

/* Get current workspace from TuiState */
#define TUI_WORKSPACE(state) app_current_workspace((state)->app)

/* Get current tab from TuiState */
#define TUI_TAB(state) app_current_tab((state)->app)

/* Get connection for current tab */
#define TUI_TAB_CONNECTION(state) app_current_tab_connection((state)->app)

/* Get current table data (from current tab) */
#define TUI_DATA(state) (TUI_TAB(state) ? TUI_TAB(state)->data : NULL)

/* Get current schema (from current tab) */
#define TUI_SCHEMA(state) (TUI_TAB(state) ? TUI_TAB(state)->schema : NULL)

/* Connection shortcuts for current tab (with null safety) */
#define TUI_CONN(state)                                                        \
  (TUI_TAB_CONNECTION(state) ? TUI_TAB_CONNECTION(state)->conn : NULL)
#define TUI_TABLES(state)                                                      \
  (TUI_TAB_CONNECTION(state) ? TUI_TAB_CONNECTION(state)->tables : NULL)
#define TUI_NUM_TABLES(state)                                                  \
  (TUI_TAB_CONNECTION(state) ? TUI_TAB_CONNECTION(state)->num_tables : 0)

/* Workspace/Tab count shortcuts */
#define TUI_NUM_WORKSPACES(state)                                              \
  ((state)->app ? (state)->app->num_workspaces : 0)
#define TUI_CURRENT_WS_IDX(state)                                              \
  ((state)->app ? (state)->app->current_workspace : 0)
#define TUI_NUM_TABS(state)                                                    \
  (TUI_WORKSPACE(state) ? TUI_WORKSPACE(state)->num_tabs : 0)
#define TUI_CURRENT_TAB_IDX(state)                                             \
  (TUI_WORKSPACE(state) ? TUI_WORKSPACE(state)->current_tab : 0)

/* Get current tab's UI state (with bounds checking) */
static inline UITabState *tui_current_tab_ui(TuiState *state) {
  if (!state || !state->app || !state->tab_ui)
    return NULL;
  size_t ws = state->app->current_workspace;
  Workspace *workspace = app_current_workspace(state->app);
  if (!workspace)
    return NULL;
  if (ws >= state->tab_ui_ws_capacity || !state->tab_ui[ws])
    return NULL;
  size_t tab = workspace->current_tab;
  if (tab >= state->tab_ui_capacity[ws])
    return NULL;
  return &state->tab_ui[ws][tab];
}

/* Get UI state for a specific workspace/tab index */
static inline UITabState *tui_get_tab_ui(TuiState *state, size_t ws_idx,
                                         size_t tab_idx) {
  if (!state || !state->tab_ui)
    return NULL;
  if (ws_idx >= state->tab_ui_ws_capacity || !state->tab_ui[ws_idx])
    return NULL;
  if (tab_idx >= state->tab_ui_capacity[ws_idx])
    return NULL;
  return &state->tab_ui[ws_idx][tab_idx];
}

/* Ensure UITabState capacity for a workspace/tab - call before accessing */
bool tui_ensure_tab_ui_capacity(TuiState *state, size_t ws_idx, size_t tab_idx);

/* Macro shortcut for current tab UI state */
#define TUI_TAB_UI(state) tui_current_tab_ui(state)

/* Get TableWidget for current tab (source of truth for cursor/scroll) */
#define TUI_TABLE_WIDGET(state)                                                \
  (TUI_TAB_UI(state) ? TUI_TAB_UI(state)->table_widget : NULL)

/* Get SidebarWidget for current tab */
#define TUI_SIDEBAR_WIDGET(state)                                              \
  (TUI_TAB_UI(state) ? TUI_TAB_UI(state)->sidebar_widget : NULL)

/* Get FiltersWidget for current tab */
#define TUI_FILTERS_WIDGET(state)                                              \
  (TUI_TAB_UI(state) ? TUI_TAB_UI(state)->filters_widget : NULL)

/* Get QueryWidget for current tab */
#define TUI_QUERY_WIDGET(state)                                                \
  (TUI_TAB_UI(state) ? TUI_TAB_UI(state)->query_widget : NULL)

/* ============================================================================
 * Widget state accessors (read from widgets as source of truth)
 * These provide null-safe access to widget state.
 * ============================================================================
 */

/* TableWidget accessors */
static inline size_t tui_cursor_row(TuiState *state) {
  TableWidget *w = TUI_TABLE_WIDGET(state);
  return w ? w->base.state.cursor_row : 0;
}
static inline size_t tui_cursor_col(TuiState *state) {
  TableWidget *w = TUI_TABLE_WIDGET(state);
  return w ? w->base.state.cursor_col : 0;
}

/* SidebarWidget accessors (read from widget, fallback to TuiState for legacy) */
static inline bool tui_sidebar_visible(TuiState *state) {
  SidebarWidget *w = TUI_SIDEBAR_WIDGET(state);
  if (w) return w->base.state.visible;
  return state ? state->sidebar_visible : false;
}
static inline bool tui_sidebar_focused(TuiState *state) {
  SidebarWidget *w = TUI_SIDEBAR_WIDGET(state);
  if (w) return w->base.state.focused;
  return state ? state->sidebar_focused : false;
}
static inline size_t tui_sidebar_highlight(TuiState *state) {
  SidebarWidget *w = TUI_SIDEBAR_WIDGET(state);
  if (w) return w->base.state.cursor_row;
  return state ? state->sidebar_highlight : 0;
}
static inline size_t tui_sidebar_scroll(TuiState *state) {
  SidebarWidget *w = TUI_SIDEBAR_WIDGET(state);
  if (w) return w->base.state.scroll_row;
  return state ? state->sidebar_scroll : 0;
}

/* FiltersWidget accessors (read from widget, fallback to UITabState for legacy) */
static inline bool tui_filters_visible(TuiState *state) {
  FiltersWidget *w = TUI_FILTERS_WIDGET(state);
  if (w) return w->base.state.visible;
  UITabState *ui = tui_current_tab_ui(state);
  return ui ? ui->filters_visible : false;
}
static inline bool tui_filters_focused(TuiState *state) {
  FiltersWidget *w = TUI_FILTERS_WIDGET(state);
  if (w) return w->base.state.focused;
  UITabState *ui = tui_current_tab_ui(state);
  return ui ? ui->filters_focused : false;
}

/* ============================================================================
 * Widget state setters (write to widgets AND legacy fields for compatibility)
 * These ensure both widget and legacy fields stay in sync during migration.
 * ============================================================================
 */

/* SidebarWidget setters - update widget, TuiState, AND UITabState */
static inline void tui_set_sidebar_visible(TuiState *state, bool visible) {
  SidebarWidget *w = TUI_SIDEBAR_WIDGET(state);
  if (w) w->base.state.visible = visible;
  if (state) state->sidebar_visible = visible;
  UITabState *ui = tui_current_tab_ui(state);
  if (ui) ui->sidebar_visible = visible;
}
static inline void tui_set_sidebar_focused(TuiState *state, bool focused) {
  SidebarWidget *w = TUI_SIDEBAR_WIDGET(state);
  if (w) w->base.state.focused = focused;
  if (state) state->sidebar_focused = focused;
  UITabState *ui = tui_current_tab_ui(state);
  if (ui) ui->sidebar_focused = focused;
}
static inline void tui_set_sidebar_highlight(TuiState *state, size_t highlight) {
  SidebarWidget *w = TUI_SIDEBAR_WIDGET(state);
  if (w) w->base.state.cursor_row = highlight;
  if (state) state->sidebar_highlight = highlight;
  UITabState *ui = tui_current_tab_ui(state);
  if (ui) ui->sidebar_highlight = highlight;
}
static inline void tui_set_sidebar_scroll(TuiState *state, size_t scroll) {
  SidebarWidget *w = TUI_SIDEBAR_WIDGET(state);
  if (w) w->base.state.scroll_row = scroll;
  if (state) state->sidebar_scroll = scroll;
  UITabState *ui = tui_current_tab_ui(state);
  if (ui) ui->sidebar_scroll = scroll;
}

/* FiltersWidget setters */
static inline void tui_set_filters_visible(TuiState *state, bool visible) {
  FiltersWidget *w = TUI_FILTERS_WIDGET(state);
  if (w) w->base.state.visible = visible;
  if (state) state->filters_visible = visible;
  UITabState *ui = tui_current_tab_ui(state);
  if (ui) ui->filters_visible = visible;
}
static inline void tui_set_filters_focused(TuiState *state, bool focused) {
  FiltersWidget *w = TUI_FILTERS_WIDGET(state);
  if (w) w->base.state.focused = focused;
  if (state) state->filters_focused = focused;
  UITabState *ui = tui_current_tab_ui(state);
  if (ui) ui->filters_focused = focused;
}

/* ============================================================================
 * Tab field accessor macros - USE THESE for cursor/scroll/data access
 *
 * These macros provide direct access to Tab fields, avoiding the need for
 * duplicated fields in TuiState. They expand to lvalues, so they work for
 * both read and write operations.
 *
 * IMPORTANT: These require TUI_TAB(state) to be non-NULL. Always check
 * for a valid tab before using these, e.g.:
 *   Tab *tab = TUI_TAB(state);
 *   if (!tab) return;
 *   tab->cursor_row = 0;  // Direct access is fine after null check
 *
 * Or use the null-safe read-only functions above for reads.
 * ============================================================================
 */

/* Cursor position - direct access to Tab fields */
#define TAB_CURSOR_ROW(tab) ((tab)->cursor_row)
#define TAB_CURSOR_COL(tab) ((tab)->cursor_col)
#define TAB_SCROLL_ROW(tab) ((tab)->scroll_row)
#define TAB_SCROLL_COL(tab) ((tab)->scroll_col)

/* Pagination state */
#define TAB_TOTAL_ROWS(tab) ((tab)->total_rows)
#define TAB_LOADED_OFFSET(tab) ((tab)->loaded_offset)
#define TAB_LOADED_COUNT(tab) ((tab)->loaded_count)

/* Column widths */
#define TAB_COL_WIDTHS(tab) ((tab)->col_widths)
#define TAB_NUM_COL_WIDTHS(tab) ((tab)->num_col_widths)

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

/* Recreate windows after layout change (sidebar visibility, etc.) */
void tui_recreate_windows(TuiState *state);

/* Restore tab state after switch or session restore */
void tab_restore(TuiState *state);

/* ============================================================================
 * Drawing functions
 * ============================================================================
 */

void tui_draw_header(TuiState *state);
void tui_draw_table(TuiState *state);
void tui_draw_connection_tab(TuiState *state);
void tui_draw_status(TuiState *state);
void tui_draw_sidebar(TuiState *state);
void tui_draw_tabs(TuiState *state);

/* ============================================================================
 * Data loading
 * ============================================================================
 */

bool tui_load_tables(TuiState *state);
bool tui_load_table_data(TuiState *state, const char *table);
bool tui_refresh_table(TuiState *state);
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
void tui_column_first(TuiState *state);
void tui_column_last(TuiState *state);

/* ============================================================================
 * Actions
 * ============================================================================
 */

void tui_next_table(TuiState *state);
void tui_prev_table(TuiState *state);
void tui_show_schema(TuiState *state);
void tui_show_connect_dialog(TuiState *state);
void tui_show_history_dialog(TuiState *state);
void tui_show_table_selector(TuiState *state);
void tui_show_config(TuiState *state);

/* Password input dialog (masks input with asterisks).
 * Returns malloc'd password string, or NULL if cancelled.
 * Caller must use str_secure_free() on result. */
char *tui_show_password_dialog(TuiState *state, const char *title,
                               const char *label, const char *error_msg);

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
bool tui_handle_query_input(TuiState *state, const UiEvent *event);
void tui_query_start_result_edit(TuiState *state);
void tui_query_confirm_result_edit(TuiState *state);
void tui_query_scroll_results(TuiState *state, int delta);
bool query_load_rows_at(TuiState *state, Tab *tab, size_t offset);

/* ============================================================================
 * Filters UI
 * ============================================================================
 */

void tui_draw_filters_panel(TuiState *state);
bool tui_handle_filters_input(TuiState *state, const UiEvent *event);
bool tui_handle_filters_click(TuiState *state, int rel_x, int rel_y);
void tui_apply_filters(TuiState *state);
int tui_get_filters_panel_height(TuiState *state);

/* ============================================================================
 * Add Row Mode
 * ============================================================================
 */

/* Start add-row mode - creates temporary row for editing */
bool tui_start_add_row(TuiState *state);

/* Cancel add-row mode (Escape) - discards temporary row */
void tui_cancel_add_row(TuiState *state);

/* Persist new row to database */
bool tui_confirm_add_row(TuiState *state);

/* Handle input while in add-row mode */
bool tui_handle_add_row_input(TuiState *state, const UiEvent *event);

/* Start editing a cell in the new row */
void tui_add_row_start_cell_edit(TuiState *state, size_t col);

/* Confirm cell edit in new row */
void tui_add_row_confirm_cell(TuiState *state);

/* Cancel cell edit in new row */
void tui_add_row_cancel_cell(TuiState *state);

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
