/*
 * Lace
 * TUI interface
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_H
#define LACE_TUI_H

#include "async/async.h"
#include "config/connections.h"
#include "config/recent.h"
#include "core/app_state.h"
#include "core/constants.h"
#include "db_compat.h"
#include "viewmodel/table_viewmodel.h"
#include "core/ui_types.h"
#include "views/text_input.h"
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
 * ============================================================================
 */
typedef struct UITabState {
  /* Filter panel visibility per-tab */
  bool filters_visible;
  bool filters_focused;

  /* Query tab UI state. query_unfocused: neither editor nor results has
   * focus, so keys reach global hotkeys (tab switching etc.); set whenever
   * the user arrives at the tab. Enter/Tab focus the editor. */
  bool query_unfocused;
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
   * =========================================================================
   */
  VmTable *vm_table; /* Current table viewmodel */

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
  int edit_cursor_y;  /* Screen Y position of edit cursor */
  int edit_cursor_x;  /* Screen X position of edit cursor */

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
  unsigned sidebar_collapsed; /* bit per folded sidebar section (VIEWS 1, FUNCTIONS 2, TRIGGERS 3) */

  /* Sidebar name scroll animation */
  size_t sidebar_name_scroll;
  int sidebar_name_scroll_dir;
  int sidebar_name_scroll_delay;
  size_t sidebar_last_highlight;
  uint64_t sidebar_view_uid; /* the tab the sidebar's highlight last followed */
  bool terminal_dark;        /* the terminal's background at startup, for color_scheme "system" */

  /* The log: every status message, newest last (TUI_LOG_MAX kept) */
  struct TuiLogLine {
    char *line; /* "HH:MM:SS  text" */
    bool error;
  } *log;
  size_t num_log, log_cap;

  /* Track state before sidebar focus for restoration */
  bool filters_was_focused;     /* Was filters focused before sidebar? */
  size_t sidebar_last_position; /* Sidebar highlight before leaving */

  /* Filters panel editing state (global; visibility and focus are per tab,
   * in UITabState: tui_filters_visible/tui_filters_focused) */
  size_t filters_cursor_row;
  size_t filters_cursor_col;
  size_t filters_scroll;
  bool filters_editing;
  char filters_edit_buffer[256]; /* Edited through filters_edit */
  TextInput filters_edit;
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
  size_t new_row_edit_pos;      /* Cursor position in edit buffer */
  bool new_row_cell_editing; /* True when editing a cell within the new row */

  /* Running flag */
  bool running;
  uint64_t shown_tab_uid; /* Tab drawn last; detects switching to a tab */

  /* Background loading indicator */
  bool bg_loading_active;

  /* =========================================================================
   * Cached connection/tables for sidebar (performance optimization)
   * These are synced in tui_sync_from_app() when tab changes.
   * =========================================================================
   */

  /* The table list the sidebar position belongs to. Only compared, never
   * dereferenced: the connection and its tables are always read through the
   * current tab (TUI_CONN, TUI_TABLES). */
  const void *sidebar_tables_key;

  /* =========================================================================
   * Per-tab UI state (TUI-specific, indexed by [workspace][tab])
   * This replaces the UI fields that were previously stored in core Tab struct.
   * Dynamic 2D array: tab_ui[workspace_index][tab_index]
   * =========================================================================
   */
  /* The saved connections and Recent, as the connection forms list and
   * change them: loaded once (tui_connmgr, tui_recent), freed at cleanup */
  ConnectionManager *connmgr;
  char *connmgr_error; /* connections.json did not load: read only, why */
  RecentList *recent;

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

/* ============================================================================
 * State accessors - null-safe access to state fields
 * ============================================================================
 */

/* Cursor/scroll accessors (read from Tab via VmTable) */
static inline size_t tui_cursor_row(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  return tab ? tab->cursor_row : 0;
}
static inline size_t tui_cursor_col(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  return tab ? tab->cursor_col : 0;
}

/* Sidebar accessors */
static inline bool tui_sidebar_visible(TuiState *state) {
  return state ? state->sidebar_visible : false;
}
static inline bool tui_sidebar_focused(TuiState *state) {
  return state ? state->sidebar_focused : false;
}
static inline size_t tui_sidebar_highlight(TuiState *state) {
  return state ? state->sidebar_highlight : 0;
}
static inline size_t tui_sidebar_scroll(TuiState *state) {
  return state ? state->sidebar_scroll : 0;
}

/* Filters accessors */
static inline bool tui_filters_visible(TuiState *state) {
  UITabState *ui = tui_current_tab_ui(state);
  return ui ? ui->filters_visible : false;
}
static inline bool tui_filters_focused(TuiState *state) {
  UITabState *ui = tui_current_tab_ui(state);
  return ui ? ui->filters_focused : false;
}

/* ============================================================================
 * State setters - update state fields
 * ============================================================================
 */

/* Sidebar setters - sidebar is global, not per-tab */
static inline void tui_set_sidebar_visible(TuiState *state, bool visible) {
  if (state) state->sidebar_visible = visible;
}
static inline void tui_set_sidebar_focused(TuiState *state, bool focused) {
  if (state) state->sidebar_focused = focused;
}
static inline void tui_set_sidebar_highlight(TuiState *state, size_t highlight) {
  if (state) state->sidebar_highlight = highlight;
}
static inline void tui_set_sidebar_scroll(TuiState *state, size_t scroll) {
  if (state) state->sidebar_scroll = scroll;
}

/* Filters setters - visibility is per-tab, editing state is global */
static inline void tui_set_filters_visible(TuiState *state, bool visible) {
  UITabState *ui = tui_current_tab_ui(state);
  if (ui) ui->filters_visible = visible;
}
static inline void tui_set_filters_focused(TuiState *state, bool focused) {
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
/* One input event through every handler, as the main loop does (the
 * palette runs an action through its key) */
void tui_dispatch_event(TuiState *state, UiEvent *event);
/* Run an action that opens a dialog (go to row, workspaces, a new
 * connection tab, history, configuration, the palette) without its key;
 * false for any other action */
bool tui_run_dialog_action(TuiState *state, HotkeyAction a);

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

/* What a connection string points at, for people: "sqlite: /path" or
 * "driver://user@host:port/db  via ssh user@host"; never a password. NULL
 * if it does not parse. Caller frees. */
char *tui_connstr_display(const char *connstr);
void tui_draw_table(TuiState *state);
void tui_draw_connection_tab(TuiState *state);
void tui_draw_status(TuiState *state);
void tui_draw_sidebar(TuiState *state);
void tui_draw_tabs(TuiState *state);

/* ============================================================================
 * Data loading
 * ============================================================================
 */

bool tui_load_tables(TuiState *state, Connection *conn_obj);
bool tui_load_table_data(TuiState *state, const char *table);

/* subviews.c: the sub-views of a table tab */
int tui_subview_strip_rows(TuiState *state); /* 1 on a table tab: the strip's row at the top of main_win */
void tui_draw_subview_strip(TuiState *state);
const char *tui_subview_label(const Tab *tab, TabSubView sv); /* "Data", "Structure" or "Definition", ... */
void tui_draw_subview(TuiState *state);      /* the current list (not Data) under the strip */
void tui_set_subview(TuiState *state, TabSubView sv);
bool tui_handle_subview_input(TuiState *state, const UiEvent *event); /* the switch keys; a list's keys */
bool tui_subview_click(TuiState *state, int rel_x, int rel_y);       /* the strip, a list row */
void tui_subview_refresh(TuiState *state); /* drop what the lists read, load the schema again */
void tui_follow_fk(TuiState *state);       /* the referenced table, filtered by the cursor row's key */
void tui_referenced_by(TuiState *state);   /* a pick list of the tables referencing this one */
/* A new tab of an object (as lace_list_objects names it; a table is LACE_OBJ_TABLE) */
bool tab_create_object(TuiState *state, LaceObjectKind kind, const char *name, const char *table, const char *detail);
/* The sidebar's entries: tables, then the VIEWS, FUNCTIONS and TRIGGERS
 * sections (headers fold), then QUERIES (the connection's query tabs in the
 * workspace; idx is the tab's index), filtered by the search text */
typedef enum { SB_TABLE, SB_OBJECT, SB_HEADER, SB_QUERY } SidebarEntryKind;
typedef struct { SidebarEntryKind kind; size_t idx; int section; } SidebarEntry; /* section 1 views, 2 routines, 3 triggers, 4 queries */
/* The sidebar's width, as the notcurses TUI's: a third of the terminal, at
 * most 28 columns (0 when it is hidden or narrower than 10) */
int tui_sidebar_width(TuiState *state);
size_t tui_sidebar_entries(TuiState *state, SidebarEntry **out);
bool tui_sidebar_entry_at(TuiState *state, size_t i, SidebarEntry *out);
const char *tui_sidebar_entry_name(TuiState *state, const SidebarEntry *e);
/* Open the entry in the current tab (Enter, a click); keep: a new tab */
void sidebar_open_entry(TuiState *state, const SidebarEntry *e, bool new_tab);
/* Show the entry in the connection's preview tab (a single click) */
void sidebar_preview_entry(TuiState *state, const SidebarEntry *e);
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

void tui_show_connect_dialog(TuiState *state);
void tui_show_history_dialog(TuiState *state);
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
bool query_ensure_total(TuiState *state, Tab *tab);
void query_poll_count(TuiState *state);

/* ============================================================================
 * Filters UI
 * ============================================================================
 */

void tui_draw_filters_panel(TuiState *state);
bool tui_handle_filters_input(TuiState *state, const UiEvent *event);

/* Move keyboard focus to the next (dir > 0) or previous (dir < 0) panel in
 * screen order: sidebar, then filters and table, or query editor and
 * results. Returns false when there is nowhere to move. */
bool tui_focus_cycle(TuiState *state, int dir);
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
/* False while the text does not fit the column (the edit stays open) */
bool tui_add_row_confirm_cell(TuiState *state);

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

#endif /* LACE_TUI_H */
