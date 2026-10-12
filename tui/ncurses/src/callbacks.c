/*
 * Lace
 * UICallbacks adapters: core actions call the TUI through these
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/session.h"
#include "core/actions.h"
#include "../../liblace/include/util/mem.h"
#include "tui_internal.h"
#include "views/config_view.h"
#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


/* ============================================================================
 * UICallbacks wrapper functions for core/actions dispatch
 * ============================================================================
 * These adapt TUI functions to the UICallbacks interface, allowing core
 * actions.c to call TUI functions without direct dependency.
 */

static void ui_move_cursor(void *ctx, int row_delta, int col_delta) {
  tui_move_cursor((TuiState *)ctx, row_delta, col_delta);
}

static void ui_page_up(void *ctx) { tui_page_up((TuiState *)ctx); }

static void ui_page_down(void *ctx) { tui_page_down((TuiState *)ctx); }

static void ui_home(void *ctx) { tui_home((TuiState *)ctx); }

static void ui_end(void *ctx) { tui_end((TuiState *)ctx); }

static void ui_column_first(void *ctx) { tui_column_first((TuiState *)ctx); }

static void ui_column_last(void *ctx) { tui_column_last((TuiState *)ctx); }

static void ui_start_edit(void *ctx) { tui_start_edit((TuiState *)ctx); }

static void ui_start_modal_edit(void *ctx) {
  tui_start_modal_edit((TuiState *)ctx);
}

static void ui_cancel_edit(void *ctx) { tui_cancel_edit((TuiState *)ctx); }

static void ui_set_cell_null(void *ctx) {
  tui_set_cell_direct((TuiState *)ctx, true);
}

static void ui_set_cell_empty(void *ctx) {
  tui_set_cell_direct((TuiState *)ctx, false);
}

static void ui_cell_copy(void *ctx) { tui_cell_copy((TuiState *)ctx); }

static void ui_cell_paste(void *ctx) { tui_cell_paste((TuiState *)ctx); }

static void ui_delete_row(void *ctx) { tui_delete_row((TuiState *)ctx); }

static void ui_recreate_layout(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  /* Sync layout-related state before recreating windows */
  AppState *app = state->app;
  if (app) {
    state->header_visible = app->header_visible;
    state->status_visible = app->status_visible;
  }
  tui_recreate_windows(state);
}

static void ui_recalculate_widths(void *ctx) {
  tui_calculate_column_widths((TuiState *)ctx);
}

static bool ui_load_more_rows(void *ctx) {
  return tui_load_more_rows((TuiState *)ctx);
}

static bool ui_load_prev_rows(void *ctx) {
  return tui_load_prev_rows((TuiState *)ctx);
}

static void ui_disconnect(void *ctx) { tui_disconnect((TuiState *)ctx); }

static size_t ui_get_sidebar_highlight_for_table(void *ctx, size_t table_idx) {
  return tui_get_sidebar_highlight_for_table((TuiState *)ctx, table_idx);
}

/* ============================================================================
 * UI State Callbacks - Sidebar
 * These read/write sidebar state from UITabState (source of truth)
 * Also sync to TuiState cache for legacy code compatibility
 * ============================================================================
 */

static bool ui_is_sidebar_visible(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  return tui_sidebar_visible(state);
}

static bool ui_is_sidebar_focused(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  return tui_sidebar_focused(state);
}

static void ui_set_sidebar_visible(void *ctx, bool visible) {
  TuiState *state = (TuiState *)ctx;
  tui_set_sidebar_visible(state, visible);
}

static void ui_set_sidebar_focused(void *ctx, bool focused) {
  TuiState *state = (TuiState *)ctx;
  tui_set_sidebar_focused(state, focused);
}

static size_t ui_get_sidebar_highlight(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  return tui_sidebar_highlight(state);
}

static void ui_set_sidebar_highlight(void *ctx, size_t highlight) {
  TuiState *state = (TuiState *)ctx;
  tui_set_sidebar_highlight(state, highlight);
}

static void ui_set_sidebar_scroll(void *ctx, size_t scroll) {
  TuiState *state = (TuiState *)ctx;
  tui_set_sidebar_scroll(state, scroll);
}

static size_t ui_get_sidebar_last_position(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  return state ? state->sidebar_last_position : 0;
}

static void ui_set_sidebar_last_position(void *ctx, size_t position) {
  TuiState *state = (TuiState *)ctx;
  if (state)
    state->sidebar_last_position = position;
}

/* ============================================================================
 * UI State Callbacks - Filters Panel
 * These read/write filters state from UITabState (source of truth)
 * Also sync to TuiState cache for legacy code compatibility
 * ============================================================================
 */

static bool ui_is_filters_visible(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  return tui_filters_visible(state);
}

static bool ui_is_filters_focused(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  return tui_filters_focused(state);
}

static void ui_set_filters_visible(void *ctx, bool visible) {
  TuiState *state = (TuiState *)ctx;
  tui_set_filters_visible(state, visible);
}

static void ui_set_filters_focused(void *ctx, bool focused) {
  TuiState *state = (TuiState *)ctx;
  tui_set_filters_focused(state, focused);
}

static void ui_set_filters_editing(void *ctx, bool editing) {
  TuiState *state = (TuiState *)ctx;
  if (state)
    state->filters_editing = editing;
}

static bool ui_get_filters_was_focused(void *ctx) {
  TuiState *state = (TuiState *)ctx;
  return state ? state->filters_was_focused : false;
}

static void ui_set_filters_was_focused(void *ctx, bool was_focused) {
  TuiState *state = (TuiState *)ctx;
  if (state)
    state->filters_was_focused = was_focused;
}

/* Build UICallbacks structure for dispatch */
UICallbacks tui_make_callbacks(TuiState *state) {
  return (UICallbacks){
      .ctx = state,
      .move_cursor = ui_move_cursor,
      .page_up = ui_page_up,
      .page_down = ui_page_down,
      .home = ui_home,
      .end = ui_end,
      .column_first = ui_column_first,
      .column_last = ui_column_last,
      .start_edit = ui_start_edit,
      .start_modal_edit = ui_start_modal_edit,
      .cancel_edit = ui_cancel_edit,
      .set_cell_null = ui_set_cell_null,
      .set_cell_empty = ui_set_cell_empty,
      .cell_copy = ui_cell_copy,
      .cell_paste = ui_cell_paste,
      .delete_row = ui_delete_row,
      .recreate_layout = ui_recreate_layout,
      .recalculate_widths = ui_recalculate_widths,
      .load_more_rows = ui_load_more_rows,
      .load_prev_rows = ui_load_prev_rows,
      .disconnect = ui_disconnect,
      /* UI State - Sidebar */
      .is_sidebar_visible = ui_is_sidebar_visible,
      .is_sidebar_focused = ui_is_sidebar_focused,
      .set_sidebar_visible = ui_set_sidebar_visible,
      .set_sidebar_focused = ui_set_sidebar_focused,
      .get_sidebar_highlight = ui_get_sidebar_highlight,
      .set_sidebar_highlight = ui_set_sidebar_highlight,
      .set_sidebar_scroll = ui_set_sidebar_scroll,
      .get_sidebar_last_position = ui_get_sidebar_last_position,
      .set_sidebar_last_position = ui_set_sidebar_last_position,
      .get_sidebar_highlight_for_table = ui_get_sidebar_highlight_for_table,
      /* UI State - Filters Panel */
      .is_filters_visible = ui_is_filters_visible,
      .is_filters_focused = ui_is_filters_focused,
      .set_filters_visible = ui_set_filters_visible,
      .set_filters_focused = ui_set_filters_focused,
      .set_filters_editing = ui_set_filters_editing,
      .get_filters_was_focused = ui_get_filters_was_focused,
      .set_filters_was_focused = ui_set_filters_was_focused,
  };
}
