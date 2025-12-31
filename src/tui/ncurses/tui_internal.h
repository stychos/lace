/*
 * Lace
 * TUI internal declarations shared between modules
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_INTERNAL_H
#define LACE_TUI_INTERNAL_H

#include "../../db/db.h"
#include "../../util/str.h"
#include "render_helpers.h"
#include "tui.h"
#include <menu.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>

/* Shared constants */
#define MIN_COL_WIDTH 4
#define MAX_COL_WIDTH 40
#define DEFAULT_COL_WIDTH 15
#define PAGE_SIZE 1000
#define PREFETCH_PAGES 2      /* Number of pages to load at once */
#define LOAD_THRESHOLD 50     /* Load more when within this many rows of edge */
#define MAX_LOADED_PAGES 5    /* Maximum pages to keep in memory */
#define TRIM_DISTANCE_PAGES 2 /* Trim data farther than this from cursor */
#define PREFETCH_THRESHOLD                                                     \
  PAGE_SIZE /* Start prefetch when within 1 page of edge */
#define MAX_VISIBLE_FILTERS 8 /* Max filter rows visible in panel */

/* ============================================================================
 * Helper functions (tui.c)
 * ============================================================================
 */

/* Sanitize string for single-line cell display */
char *tui_sanitize_for_display(const char *str);

/* Case-insensitive substring search */
const char *tui_str_istr(const char *haystack, const char *needle);

/* Recreate windows after resize or sidebar toggle */
void tui_recreate_windows(TuiState *state);

/* ============================================================================
 * Workspace functions (workspace.c)
 * ============================================================================
 */

/* Initialize a workspace struct (zeroes and inits filters) */
void workspace_init(Workspace *ws);

/* Save current TUI state to current tab */
void tab_save(TuiState *state);

/* Restore TUI state from current tab */
void tab_restore(TuiState *state);

/* Sync focus and panel state from TuiState to current Tab */
void tab_sync_focus(TuiState *state);

/* Switch to a different tab */
void tab_switch(TuiState *state, size_t index);

/* Create new tab for a table */
bool tab_create(TuiState *state, size_t table_index);

/* Create new query tab */
bool tab_create_query(TuiState *state);

/* Close current tab */
void tab_close(TuiState *state);

/* Legacy wrappers for compatibility */
void workspace_save(TuiState *state);
void workspace_restore(TuiState *state);
void workspace_switch(TuiState *state, size_t index);
bool workspace_create(TuiState *state, size_t table_index);
void workspace_close(TuiState *state);

/* ============================================================================
 * Sidebar functions (sidebar.c)
 * ============================================================================
 */

/* Count tables matching current filter */
size_t tui_count_filtered_tables(TuiState *state);

/* Get actual table index from filtered index */
size_t tui_get_filtered_table_index(TuiState *state, size_t filtered_idx);

/* Get sidebar highlight for a table index */
size_t tui_get_sidebar_highlight_for_table(TuiState *state, size_t table_idx);

/* Handle sidebar input when focused */
bool tui_handle_sidebar_input(TuiState *state, const UiEvent *event);

/* Update sidebar name scroll animation */
void tui_update_sidebar_scroll_animation(TuiState *state);

/* ============================================================================
 * Pagination functions (pagination.c)
 * ============================================================================
 */

/* Load more rows at end of current data */
bool tui_load_more_rows(TuiState *state);

/* Load rows at specific offset (replaces current data) */
bool tui_load_rows_at(TuiState *state, size_t offset);

/* Load previous rows (prepend to current data) */
bool tui_load_prev_rows(TuiState *state);

/* Trim loaded data to keep memory bounded */
void tui_trim_loaded_data(TuiState *state);

/* Check if more rows need to be loaded based on cursor position */
void tui_check_load_more(TuiState *state);

/* Load a page with blocking dialog (for fast scrolling past loaded data) */
bool tui_load_page_with_dialog(TuiState *state, bool forward);

/* Load rows at specific offset with blocking dialog (for goto/home/end) */
bool tui_load_rows_at_with_dialog(TuiState *state, size_t offset);

/* Start background load (non-blocking) - returns true if started */
bool tui_start_background_load(TuiState *state, bool forward);

/* Poll background load, merge if complete - call from main loop */
bool tui_poll_background_load(TuiState *state);

/* Cancel pending background load */
void tui_cancel_background_load(TuiState *state);

/* Check if speculative prefetch should start */
void tui_check_speculative_prefetch(TuiState *state);

/* ============================================================================
 * Edit functions (edit.c)
 * ============================================================================
 */

/* Find primary key column indices */
size_t tui_find_pk_columns(TuiState *state, size_t *pk_indices, size_t max_pks);

/* Start inline editing */
void tui_start_edit(TuiState *state);

/* Start modal editing */
void tui_start_modal_edit(TuiState *state);

/* Cancel editing */
void tui_cancel_edit(TuiState *state);

/* Confirm edit and update database */
void tui_confirm_edit(TuiState *state);

/* Set cell value directly (NULL or empty string) */
void tui_set_cell_direct(TuiState *state, bool set_null);

/* Delete current row */
void tui_delete_row(TuiState *state);

/* Handle edit mode input */
bool tui_handle_edit_input(TuiState *state, const UiEvent *event);

/* ============================================================================
 * Navigation functions (navigation.c)
 * ============================================================================
 */

/* Note: tui_move_cursor, tui_page_up/down, tui_home/end,
 * tui_next/prev_table are declared in tui.h as public API */

/* ============================================================================
 * Draw functions (draw.c)
 * ============================================================================
 */

/* Note: tui_draw_header, tui_draw_table, tui_draw_status, tui_draw_sidebar
 * are declared in tui.h as public API */

/* Parameters for drawing a result grid */
typedef struct {
  WINDOW *win;             /* Window to draw in */
  int start_y;             /* Starting Y position in window */
  int start_x;             /* Starting X position in window */
  int height;              /* Available height for grid */
  int width;               /* Available width for grid */
  ResultSet *data;         /* Data to display */
  int *col_widths;         /* Column widths array */
  size_t num_col_widths;   /* Number of column widths */
  size_t cursor_row;       /* Current cursor row */
  size_t cursor_col;       /* Current cursor column */
  size_t scroll_row;       /* Vertical scroll offset */
  size_t scroll_col;       /* Horizontal scroll offset */
  size_t selection_offset; /* Offset for selection calculation (global row) */
  bool is_focused;         /* Whether this grid has focus */
  bool is_editing;         /* Whether editing is active */
  char *edit_buffer;       /* Edit buffer content */
  size_t edit_pos;         /* Cursor position in edit buffer */
  bool show_header_line;   /* Whether to draw top border line */
  SortEntry *sort_entries; /* Array of sort columns (NULL if none) */
  size_t num_sort_entries; /* Number of sort columns */
} GridDrawParams;

/* Draw a result set grid (used by table view and query results) */
void tui_draw_result_grid(TuiState *state, GridDrawParams *params);

/* Handle mouse events (using pre-translated UiEvent) */
bool tui_handle_mouse_event(TuiState *state, const UiEvent *event);

/* ============================================================================
 * Dialog functions (dialogs.c)
 * ============================================================================
 */

/* Show confirmation dialog - returns true if user confirms */
bool tui_show_confirm_dialog(TuiState *state, const char *message);

/* Show go-to row dialog */
void tui_show_goto_dialog(TuiState *state);

/* Note: tui_show_schema, tui_show_connect_dialog, tui_show_table_selector,
 * tui_show_config are declared in tui.h as public API */

#endif /* LACE_TUI_INTERNAL_H */
