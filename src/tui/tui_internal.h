/*
 * lace - Database Viewer and Manager
 * TUI internal declarations shared between modules
 */

#ifndef LACE_TUI_INTERNAL_H
#define LACE_TUI_INTERNAL_H

#include "tui.h"
#include "../db/db.h"
#include "../util/str.h"
#include <menu.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>

/* Shared constants */
#define MIN_COL_WIDTH 4
#define MAX_COL_WIDTH 40
#define DEFAULT_COL_WIDTH 15
#define PAGE_SIZE 500
#define LOAD_THRESHOLD 50     /* Load more when within this many rows of edge */
#define MAX_LOADED_PAGES 5    /* Maximum pages to keep in memory */
#define TRIM_DISTANCE_PAGES 2 /* Trim data farther than this from cursor */

/* ============================================================================
 * Helper functions (tui.c)
 * ============================================================================ */

/* Translate keyboard input from non-Latin layouts to Latin equivalents */
int tui_translate_key(int ch);

/* Sanitize string for single-line cell display */
char *tui_sanitize_for_display(const char *str);

/* Case-insensitive substring search */
const char *tui_str_istr(const char *haystack, const char *needle);

/* Recreate windows after resize or sidebar toggle */
void tui_recreate_windows(TuiState *state);

/* ============================================================================
 * Workspace functions (workspace.c)
 * ============================================================================ */

/* Save current state to workspace */
void workspace_save(TuiState *state);

/* Restore state from current workspace */
void workspace_restore(TuiState *state);

/* Switch to a different workspace */
void workspace_switch(TuiState *state, size_t index);

/* Create new workspace for a table */
bool workspace_create(TuiState *state, size_t table_index);

/* Close current workspace */
void workspace_close(TuiState *state);

/* ============================================================================
 * Sidebar functions (sidebar.c)
 * ============================================================================ */

/* Count tables matching current filter */
size_t tui_count_filtered_tables(TuiState *state);

/* Get actual table index from filtered index */
size_t tui_get_filtered_table_index(TuiState *state, size_t filtered_idx);

/* Get sidebar highlight for a table index */
size_t tui_get_sidebar_highlight_for_table(TuiState *state, size_t table_idx);

/* Handle sidebar input when focused */
bool tui_handle_sidebar_input(TuiState *state, int ch);

/* Update sidebar name scroll animation */
void tui_update_sidebar_scroll_animation(TuiState *state);

/* ============================================================================
 * Pagination functions (pagination.c)
 * ============================================================================ */

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

/* ============================================================================
 * Edit functions (edit.c)
 * ============================================================================ */

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
bool tui_handle_edit_input(TuiState *state, int ch);

/* ============================================================================
 * Navigation functions (navigation.c)
 * ============================================================================ */

/* Note: tui_move_cursor, tui_page_up/down, tui_home/end,
 * tui_next/prev_table are declared in tui.h as public API */

/* ============================================================================
 * Draw functions (draw.c)
 * ============================================================================ */

/* Note: tui_draw_header, tui_draw_table, tui_draw_status, tui_draw_sidebar
 * are declared in tui.h as public API */

/* Handle mouse events */
bool tui_handle_mouse_event(TuiState *state);

/* ============================================================================
 * Dialog functions (dialogs.c)
 * ============================================================================ */

/* Show go-to row dialog */
void tui_show_goto_dialog(TuiState *state);

/* Note: tui_show_schema, tui_show_connect_dialog, tui_show_table_selector,
 * tui_show_help are declared in tui.h as public API */

#endif /* LACE_TUI_INTERNAL_H */
