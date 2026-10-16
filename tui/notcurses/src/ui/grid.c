#include "ui/grid.h"
#include "ui/sidebar.h"
#include "ui/query_view.h"
#include "ui/filter_panel.h"
#include "ui/tabstrip.h"
#include "edit_ops.h"
#include "undo.h"
#include "core/utf8.h"
#include "model/workspace.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Rect content_rect(const NcApp *app) {
  int top = 1 + tabstrip_rows(app);
  Rect r = {top, 0, app->rows - 1 - top, app->cols};
  if (r.rows < 0) r.rows = 0;
  return r;
}

Rect grid_content_rect(const NcApp *app) {
  Rect r = content_rect(app);
  int sw = sidebar_width(app);
  r.x += sw;
  r.cols -= sw;
  const ConnTab *t = app->num_workspaces ? workspace_current_tab(app->workspaces[app->current_ws]) : NULL;
  int ph = filter_panel_rows(app, conntab_current_view(t));
  r.y += ph;
  r.rows -= ph;
  if (r.rows < 0) r.rows = 0;
  return r;
}

Rect grid_view_rect(const NcApp *app, const View *v) {
  return v && v->kind == VIEW_QUERY ? query_view_results_rect(app, v) : grid_content_rect(app);
}

size_t grid_rows_visible(Rect r) { return r.rows > 1 ? (size_t)(r.rows - 1) : 0; }

static int digits(size_t n) {
  int d = 1;
  while (n >= 10) { n /= 10; d++; }
  return d;
}

int grid_gutter(const View *v, const NcApp *app) {
  if (!view_row_numbers(v, app->config)) return 0;
  return digits(page_cache_extent(v->cache)) + 1;
}

static int col_width(const View *v, size_t c) { return c < v->num_widths ? v->widths[c] : DEFAULT_COL_WIDTH; }

static const LaceColumn *schema_col(const View *v, const char *name) {
  if (!v->schema || !name) return NULL;
  for (size_t i = 0; i < v->schema->num_columns; i++)
    if (v->schema->columns[i].name && strcmp(v->schema->columns[i].name, name) == 0)
      return &v->schema->columns[i];
  return NULL;
}

static const char *col_name(const View *v, size_t c) {
  const LaceColumn *col = page_cache_column(v->cache, c);
  return col && col->name ? col->name : "";
}

/* ==========================================================================
 * Drawing
 * ========================================================================== */

static void want_page(View *v, size_t row) {
  PageCache *pc = v->cache;
  size_t page = row / page_cache_page_size(pc);
  if (page_cache_page_loaded_p(pc, page) || page_cache_page_pending(pc, page)) return;
  if (view_fail_blocks(v)) return; /* something failed here; try again once the user moves */
  v->fail_set = false;
  page_cache_request(pc, row);
}

static const char *const SUPER[] = {"\xe2\x81\xb0", "\xc2\xb9", "\xc2\xb2", "\xc2\xb3", "\xe2\x81\xb4",
                                    "\xe2\x81\xb5", "\xe2\x81\xb6", "\xe2\x81\xb7", "\xe2\x81\xb8"};

static void draw_header(NcApp *app, View *v, Rect r, int gutter) {
  const Theme *t = &app->theme;
  struct ncplane *p = app->std;
  nc_style(p, t, TC_HEADER_FG, TC_HEADER_BG, NCSTYLE_BOLD);
  nc_put(p, r.y, r.x, r.cols, "");
  size_t ncols = page_cache_num_columns(v->cache);
  int x = r.x + gutter, end = r.x + r.cols;
  for (size_t c = v->scroll_col; c < ncols && x < end; c++) {
    int w = col_width(v, c);
    if (w > end - x) w = end - x;
    const char *name = col_name(v, c);
    const LaceColumn *sc = schema_col(v, name);
    char *label = NULL;
    for (size_t i = 0; i < v->num_sorts; i++) {
      if (strcmp(v->sorts[i].column, name) != 0) continue;
      const char *rank = v->num_sorts > 1 && i < 9 ? SUPER[i + 1] : "";
      label = str_printf("%s%s%s", v->sorts[i].desc ? "\xe2\x86\x93" : "\xe2\x86\x91", name, rank);
      break;
    }
    if (!label) label = str_dup(name);
    nc_style(p, t, c == v->cursor_col ? TC_ACCENT : TC_HEADER_FG, TC_HEADER_BG, NCSTYLE_BOLD);
    int lw = nc_text_width(label);
    bool pk = sc && sc->primary_key && lw + 2 <= w;
    nc_put(p, r.y, x, w, label);
    if (pk) {
      nc_style(p, t, TC_PK_FG, TC_HEADER_BG, NCSTYLE_BOLD);
      nc_put(p, r.y, x + lw + 1, 1, "\xe2\x9a\xb7");
    }
    free(label);
    x += w + 1;
  }
}

static void draw_center(NcApp *app, Rect r, ThemeColor fg, const char *msg) {
  nc_style(app->std, &app->theme, fg, TC_BG, 0);
  int w = nc_text_width(msg);
  if (w > r.cols) w = r.cols;
  nc_put(app->std, r.y + 1 + (r.rows - 1) / 2, r.x + (r.cols - w) / 2, w, msg);
}

static void draw_cell(NcApp *app, int y, int x, int w, const LaceValue *cell, ThemeColor bg,
                      bool cursor_cell, bool dim, bool selected) {
  const Theme *t = &app->theme;
  ThemeColor fg = TC_FG;
  bool right = false;
  if (cell->omitted) fg = TC_DIM;
  else if (cell->is_null) fg = TC_NULL_FG;
  else if (lace_type_is_numeric(cell->type)) fg = TC_NUMBER_FG, right = true;
  else if (selected) fg = TC_SELECTED_FG;
  if (dim) fg = TC_DIM;
  if (cursor_cell) {
    nc_style(app->std, t, TC_BG, TC_ACCENT, NCSTYLE_BOLD);
  } else {
    nc_style(app->std, t, fg, bg, 0);
  }
  char *text = lace_value_display_text(cell, (size_t)w);
  if (!text) text = str_dup("");
  for (char *c = text; *c; c++)
    if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
  if (right) nc_put_right(app->std, y, x, w, text);
  else nc_put(app->std, y, x, w, text);
  free(text);
}

void grid_draw(NcApp *app, View *v, Rect r) {
  grid_edit_begin_frame();
  if (r.rows < 2 || r.cols < 4) return;
  const Theme *t = &app->theme;
  struct ncplane *p = app->std;
  PageCache *pc = v->cache;
  size_t vis = grid_rows_visible(r);
  nc_style(p, t, TC_FG, TC_BG, 0);
  for (int y = r.y; y < r.y + r.rows; y++) nc_put(p, y, r.x, r.cols, "");

  if (v->schema_error) {
    draw_center(app, r, TC_ERROR_FG, v->schema_error);
    return;
  }
  size_t extent = page_cache_extent(pc);
  size_t ncols = page_cache_num_columns(pc);
  if (v->kind == VIEW_QUERY && ncols == 0) return; /* the query view says what is going on */
  if (v->kind == VIEW_TABLE && (!v->schema || (ncols == 0 && !v->error))) {
    draw_center(app, r, TC_DIM, "Loading\xe2\x80\xa6");
    return;
  }
  if (ncols == 0 && v->error) {
    draw_center(app, r, TC_ERROR_FG, v->error);
    return;
  }
  /* The view may have shrunk (a reload with fewer rows) */
  if (v->scroll_row > 0 && v->scroll_row >= extent) v->scroll_row = extent ? extent - 1 : 0;
  int gutter = grid_gutter(v, app);
  if (gutter > r.cols - 1) gutter = r.cols - 1;
  view_scroll_into_view(v, (int)vis, r.cols, gutter); /* after a resize the cursor stays visible */
  draw_header(app, v, r, gutter);

  bool stale = page_cache_stale(pc);
  int edit_y = -1, edit_x = 0, edit_w = 0;
  for (size_t i = 0; i < vis; i++) {
    size_t row = v->scroll_row + i;
    int y = r.y + 1 + (int)i;
    if (row >= extent) break;
    want_page(v, row);
    bool cur = row == v->cursor_row;
    bool sel = view_is_selected(v, row);
    ThemeColor bg = cur ? TC_CURSOR_BG : (row % 2 ? TC_STRIPE_BG : TC_ROW_BG);
    nc_style(p, t, TC_FG, bg, 0);
    nc_put(p, y, r.x, r.cols, "");
    if (gutter) {
      char num[32];
      snprintf(num, sizeof num, "%*zu", gutter - 1, row + 1);
      nc_style(p, t, TC_DIM, bg, 0);
      nc_put(p, y, r.x, gutter - 1, num);
      if (sel) {
        nc_style(p, t, TC_SELECTED_FG, bg, NCSTYLE_BOLD);
        nc_put(p, y, r.x + gutter - 1, 1, "\xe2\x96\x8c");
      }
    }
    bool loaded = page_cache_row_loaded(pc, row);
    bool row_stale = loaded && stale && !page_cache_page_loaded_p(pc, row / page_cache_page_size(pc));
    int x = r.x + gutter, end = r.x + r.cols;
    for (size_t c = v->scroll_col; c < ncols && x < end; c++) {
      int w = col_width(v, c);
      if (w > end - x) w = end - x;
      const LaceValue *cell = loaded ? page_cache_cell(pc, row, c) : NULL;
      if (cell) {
        if (cur && c == v->cursor_col) edit_y = y, edit_x = x, edit_w = w;
        draw_cell(app, y, x, w, cell, bg, cur && c == v->cursor_col, row_stale, sel && !gutter);
      } else {
        bool cc = cur && c == v->cursor_col;
        if (cc) nc_style(p, t, TC_BG, TC_ACCENT, 0);
        else nc_style(p, t, TC_DIM, bg, 0);
        const char *sh = ((app->spinner / 3 + (int)row) & 1) ? "\xe2\x96\x92\xe2\x96\x92\xe2\x96\x92"
                                                             : "\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91";
        nc_put(p, y, x, w, view_fail_blocks(v) ? "" : sh);
      }
      x += w + 1;
    }
  }
  if (extent == 0 && page_cache_total_known(pc)) draw_center(app, r, TC_DIM, "No rows");
  if (v->editing && edit_y >= 0) grid_edit_draw(app, v, edit_y, edit_x, edit_w, r.x + r.cols);
}

/* ==========================================================================
 * Moving
 * ========================================================================== */

void grid_follow(NcApp *app, View *v, bool jumped) {
  Rect r = grid_view_rect(app, v);
  int vis = (int)grid_rows_visible(r);
  if (vis < 1) vis = 1;
  if (jumped && (v->cursor_row < v->scroll_row || v->cursor_row >= v->scroll_row + (size_t)vis)) {
    size_t half = (size_t)vis / 2;
    v->scroll_row = v->cursor_row > half ? v->cursor_row - half : 0;
  }
  view_scroll_into_view(v, vis, r.cols, grid_gutter(v, app));
}

void grid_goto_row(NcApp *app, View *v, size_t row) {
  PageCache *pc = v->cache;
  if (page_cache_total_known(pc) || row == 0 || (row != SIZE_MAX && row < page_cache_extent(pc))) {
    v->jump_pending = false;
    view_set_cursor(v, row, v->cursor_col);
  } else {
    /* The total is not known: go as far as is loaded, the rest when it is */
    v->jump_pending = true;
    v->jump_row = row;
    view_set_cursor(v, page_cache_extent(pc) ? page_cache_extent(pc) - 1 : 0, v->cursor_col);
    if (!(v->count_pending && v->count_gen == page_cache_generation(pc))) app_view_count(app, v);
    app_set_status(app, false, "Counting rows\xe2\x80\xa6");
  }
  grid_follow(app, v, true);
}

void grid_resolve_jump(NcApp *app, View *v) {
  if (!v->jump_pending || !page_cache_total_known(v->cache)) return;
  v->jump_pending = false;
  view_set_cursor(v, v->jump_row, v->cursor_col);
  grid_follow(app, v, true);
}

void grid_state_changed(NcApp *app, View *v) {
  v->preview = false; /* a sorted or filtered preview is kept */
  v->cursor_row = v->scroll_row = 0;
  v->jump_pending = false;
  view_clear_selection(v);
  app_view_reload(app, v);
}

static const char *cursor_column_name(const View *v) {
  const LaceColumn *c = page_cache_column(v->cache, v->cursor_col);
  return c && c->name ? c->name : NULL;
}

static void sort_by(NcApp *app, View *v, const char *column) {
  if (!column || !*column) return;
  if (!schema_col(v, column)) {
    app_set_status(app, false, "Cannot sort by %s", column);
    return;
  }
  view_checkpoint(v);
  view_cycle_sort(v, column);
  grid_state_changed(app, v);
}

/* The writing actions: all of them go through edit_ops.c */
static bool grid_edit_action(NcApp *app, View *v, HotkeyAction a) {
  if (a == HOTKEY_ROW_ADD) {
    if (!v->schema) {
      app_set_status(app, true, "The table is still loading");
      return true;
    }
    v->preview = false;
    add_row_open(app, v);
    return true;
  }
  if (page_cache_extent(v->cache) == 0) return true;
  EditContext ctx;
  const char *why = NULL;
  size_t row = v->cursor_row, col = v->cursor_col;
  uint64_t gen = page_cache_generation(v->cache);
  if (a == HOTKEY_CELL_COPY || a == HOTKEY_CELL_CUT) { /* a cut is a copy and then the empty write */
    const LaceValue *cell = page_cache_cell(v->cache, row, col);
    if (!cell) {
      app_set_status(app, true, "The row is not loaded yet");
      return true;
    }
    if (cell->omitted) {
      app_set_status(app, true, "The value is too large to copy");
      return true;
    }
    char *text = lace_value_to_edit_text(cell);
    app_clipboard_set(app, text ? text : "");
    str_secure_free(text);
    app_set_status(app, false, a == HOTKEY_CELL_CUT ? "Cut to clipboard" : "Copied to clipboard");
    if (a == HOTKEY_CELL_COPY) return true;
  }
  if (!edit_context_for_view(app, v, &ctx, &why)) {
    grid_edit_report(app, why);
    return true;
  }
  v->preview = false; /* an edited preview is kept */
  switch (a) {
  case HOTKEY_EDIT_MODAL: cell_editor_open(app, v, row, col); return true;
  case HOTKEY_SET_NULL:
    if (!edit_cell(&ctx, row, col, NULL, true, false, gen, &why)) grid_edit_report(app, why);
    return true;
  case HOTKEY_SET_EMPTY:
  case HOTKEY_CELL_CUT:
    if (!edit_cell(&ctx, row, col, "", false, true, gen, &why)) grid_edit_report(app, why);
    return true;
  case HOTKEY_CELL_PASTE:
    if (!edit_paste_cell(&ctx, row, col, app_clipboard_get(app), gen, &why)) grid_edit_report(app, why);
    return true;
  case HOTKEY_DELETE_ROW:
    if (v->num_selected) edit_delete_rows(&ctx, v->selected, v->num_selected, gen);
    else edit_delete_rows(&ctx, &row, 1, gen);
    return true;
  default: return false;
  }
}

bool grid_do_action(NcApp *app, View *v, HotkeyAction a) {
  if (!v) return false;
  if (v->kind == VIEW_QUERY) { /* sorting, filters, structure and new rows belong to tables */
    if (a == HOTKEY_CYCLE_SORT || a == HOTKEY_TOGGLE_FILTERS || a == HOTKEY_CLEAR_FILTERS ||
        a == HOTKEY_SHOW_SCHEMA || a == HOTKEY_ROW_ADD || a == HOTKEY_EDIT_INLINE)
      return false;
  }
  Rect r = grid_view_rect(app, v);
  long vis = (long)grid_rows_visible(r);
  switch (a) {
  case HOTKEY_MOVE_UP: view_move(v, -1, 0); break;
  case HOTKEY_MOVE_DOWN: view_move(v, 1, 0); break;
  case HOTKEY_MOVE_LEFT: view_move(v, 0, -1); break;
  case HOTKEY_MOVE_RIGHT: view_move(v, 0, 1); break;
  case HOTKEY_PAGE_UP: view_move(v, -(vis > 1 ? vis : 1), 0); break;
  case HOTKEY_PAGE_DOWN: view_move(v, vis > 1 ? vis : 1, 0); break;
  case HOTKEY_FIRST_ROW: grid_goto_row(app, v, 0); return true;
  case HOTKEY_LAST_ROW: grid_goto_row(app, v, SIZE_MAX); return true;
  case HOTKEY_FIRST_COL: view_set_cursor(v, v->cursor_row, 0); break;
  case HOTKEY_LAST_COL: view_set_cursor(v, v->cursor_row, SIZE_MAX); break;
  case HOTKEY_GOTO_ROW: goto_open(app, v); return true;
  case HOTKEY_CYCLE_SORT: sort_by(app, v, cursor_column_name(v)); return true;
  case HOTKEY_TOGGLE_FILTERS:
    if (!v->schema) { app_set_status(app, false, "The table is still loading"); return true; }
    if (filter_panel_enabled(app)) filter_panel_toggle(app, v);
    else filter_editor_open(app, v);
    return true;
  case HOTKEY_ADD_FILTER:
    if (filter_panel_enabled(app)) filter_panel_add(app, v);
    else if (v->schema) filter_editor_open(app, v);
    else app_set_status(app, false, "The table is still loading");
    return true;
  case HOTKEY_REMOVE_FILTER:
    filter_panel_remove(app, v);
    return true;
  case HOTKEY_CLEAR_FILTERS:
    if (!v->num_filters && !v->num_sorts) return true;
    view_checkpoint(v);
    view_clear_filters(v);
    view_clear_sorts(v);
    grid_state_changed(app, v);
    return true;
  case HOTKEY_REFRESH: app_view_reload(app, v); return true;
  case HOTKEY_EDIT_INLINE:
    if (page_cache_extent(v->cache) == 0) return true;
    grid_edit_start(app, v);
    return true;
  case HOTKEY_EDIT_MODAL:
  case HOTKEY_SET_NULL:
  case HOTKEY_SET_EMPTY:
  case HOTKEY_DELETE_ROW:
  case HOTKEY_CELL_COPY:
  case HOTKEY_CELL_CUT:
  case HOTKEY_CELL_PASTE:
  case HOTKEY_ROW_ADD:
    return grid_edit_action(app, v, a);
  case HOTKEY_UNDO:
    undo_last(app, v);
    return true;
  case HOTKEY_SHOW_SCHEMA:
    if (!v->schema) { app_set_status(app, false, "The structure is not loaded"); return true; }
    structure_open(app, v);
    return true;
  case HOTKEY_TOGGLE_SELECTION:
    if (page_cache_extent(v->cache) == 0) return true;
    view_toggle_select(v, v->cursor_row);
    return true;
  case HOTKEY_CLEAR_SELECTIONS:
    if (!v->num_selected) return false;
    view_clear_selection(v);
    return true;
  case HOTKEY_COLUMN_NARROWER:
  case HOTKEY_COLUMN_WIDER:
    if (v->cursor_col >= v->num_widths) return true;
    view_set_width(v, v->cursor_col, v->widths[v->cursor_col] + (a == HOTKEY_COLUMN_WIDER ? 4 : -4));
    break;
  default: return false;
  }
  v->jump_pending = false; /* the user moved on */
  grid_follow(app, v, false);
  return true;
}

static const HotkeyAction GRID_ACTIONS[] = {
    HOTKEY_MOVE_UP, HOTKEY_MOVE_DOWN, HOTKEY_MOVE_LEFT, HOTKEY_MOVE_RIGHT, HOTKEY_PAGE_UP,
    HOTKEY_PAGE_DOWN, HOTKEY_FIRST_ROW, HOTKEY_LAST_ROW, HOTKEY_GOTO_ROW, HOTKEY_FIRST_COL,
    HOTKEY_LAST_COL, HOTKEY_CYCLE_SORT, HOTKEY_TOGGLE_FILTERS, HOTKEY_CLEAR_FILTERS,
    HOTKEY_REFRESH, HOTKEY_SHOW_SCHEMA, HOTKEY_TOGGLE_SELECTION, HOTKEY_CLEAR_SELECTIONS,
    HOTKEY_COLUMN_NARROWER, HOTKEY_COLUMN_WIDER};

bool grid_handle_key(NcApp *app, View *v, const UiEvent *ev, Rect r) {
  (void)r;
  if (ev->type != UI_EVENT_KEY) return false;
  for (size_t i = 0; i < sizeof GRID_ACTIONS / sizeof *GRID_ACTIONS; i++)
    if (hotkey_matches(app->config, ev, GRID_ACTIONS[i]) && grid_do_action(app, v, GRID_ACTIONS[i]))
      return true;
  return false;
}

/* The column under screen column x, or SIZE_MAX */
static size_t col_at(const View *v, Rect r, int gutter, int x) {
  size_t ncols = page_cache_num_columns(v->cache);
  int cx = r.x + gutter;
  for (size_t c = v->scroll_col; c < ncols && cx < r.x + r.cols; c++) {
    int w = col_width(v, c);
    if (x >= cx && x < cx + w) return c;
    cx += w + 1;
  }
  return SIZE_MAX;
}

bool grid_handle_mouse(NcApp *app, View *v, const UiEvent *ev, Rect r) {
  if (ev->type != UI_EVENT_MOUSE) return false;
  if (v->editing) return true; /* the field owns the keyboard and the mouse */
  int x = ev->mouse.x, y = ev->mouse.y;
  if (y < r.y || y >= r.y + r.rows || x < r.x || x >= r.x + r.cols) return false;
  if (ev->mouse.action != UI_MOUSE_PRESS) return true;
  if (v->kind == VIEW_QUERY) v->focus_results = true;
  size_t extent = page_cache_extent(v->cache);
  size_t vis = grid_rows_visible(r);
  if (ev->mouse.button == UI_MOUSE_SCROLL_UP || ev->mouse.button == UI_MOUSE_SCROLL_DOWN) {
    if (ev->mouse.button == UI_MOUSE_SCROLL_UP) v->scroll_row = v->scroll_row > 3 ? v->scroll_row - 3 : 0;
    else if (v->scroll_row + 3 < extent) v->scroll_row += 3;
    if (v->cursor_row < v->scroll_row) v->cursor_row = v->scroll_row;
    else if (vis && v->cursor_row >= v->scroll_row + vis) v->cursor_row = v->scroll_row + vis - 1;
    return true;
  }
  if (ev->mouse.button != UI_MOUSE_LEFT) return true;
  int gutter = grid_gutter(v, app);
  size_t c = col_at(v, r, gutter, x);
  if (y == r.y) {
    if (c != SIZE_MAX && v->kind == VIEW_QUERY) {
      v->cursor_col = c;
    } else if (c != SIZE_MAX) {
      v->cursor_col = c;
      sort_by(app, v, col_name(v, c));
    }
    return true;
  }
  size_t row = v->scroll_row + (size_t)(y - r.y - 1);
  if (row >= extent) return true;
  view_set_cursor(v, row, c != SIZE_MAX ? c : v->cursor_col);
  grid_follow(app, v, false);
  return true;
}
