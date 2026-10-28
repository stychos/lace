/*
 * Lace
 * Drawing functions
 *
 * Uses RenderBackend abstraction for portability while maintaining
 * ncurses compatibility during migration.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/config.h"
#include "core/app_state.h"
#include "core/connstr_display.h"
#include "util/connstr.h"
#include "render_helpers.h"
#include "core/catalog_rows.h"
#include "tui_internal.h"
#include "views/form_tab.h"
#include "views/theme.h"
#include "views/overlay.h"
#include <stdlib.h>
#include <string.h>

/* Maximum number of visible columns (limited by terminal width) */
#define MAX_VISIBLE_COLUMNS 256

/* ============================================================================
 * Helper Functions
 * ============================================================================
 */


/* Helper to get column width from params */
static int grid_get_col_width(GridDrawParams *params, size_t col) {
  if (params->col_widths && col < params->num_col_widths) {
    return params->col_widths[col];
  }
  return DEFAULT_COL_WIDTH;
}

/* ============================================================================
 * Result Grid Drawing
 * ============================================================================
 */

/* The sort marks' ranks after the arrow: ¹ ² ³ ... */
static const char *const SUPER[] = {"\xe2\x81\xb0", "\xc2\xb9", "\xc2\xb2", "\xc2\xb3", "\xe2\x81\xb4",
                                    "\xe2\x81\xb5", "\xe2\x81\xb6", "\xe2\x81\xb7", "\xe2\x81\xb8"};

/* The schema column a result column is, by name (NULL: none) */
static const ColumnDef *schema_col(const TableSchema *sc, const char *name) {
  size_t i = db_schema_column_index(sc, name);
  return i != SIZE_MAX ? &sc->columns[i] : NULL;
}

/* One cell, as the notcurses grid draws it: NULL and numbers in their
 * colours (numbers right), the cursor cell in the accent colour */
static void draw_cell(WINDOW *win, int y, int x, int w, const DbValue *val, ThemeColor bg, bool cursor_cell,
                      bool selected) {
  ThemeColor fg = TC_FG;
  bool right = false;
  if (val->omitted)
    fg = TC_DIM;
  else if (val->is_null)
    fg = TC_NULL_FG;
  else if (lace_type_is_numeric(val->type))
    fg = TC_NUMBER_FG, right = true;
  else if (selected)
    fg = TC_SELECTED_FG;
  if (cursor_cell)
    tui_style(win, TC_BG, TC_ACCENT, A_BOLD);
  else
    tui_style(win, fg, bg, 0);
  char *text = lace_value_display_text(val, (size_t)w);
  if (!text)
    text = str_dup("");
  for (char *c = text; *c; c++)
    if (*c == '\n' || *c == '\r' || *c == '\t')
      *c = ' ';
  int tw = utf8_width(text, strlen(text));
  if (right && tw < w) {
    tui_print_fit(win, y, x, w - tw, "");
    tui_print_fit(win, y, x + w - tw, tw, text);
  } else {
    tui_print_fit(win, y, x, w, text);
  }
  free(text);
}

/* Draw a result set grid, as the notcurses TUI's: a header row of the
 * column names (the cursor's in the accent colour, a sort's arrow before a
 * name, ⚷ after a key column's, ⇒ after a foreign key's first column),
 * then the rows, striped, the cursor row highlighted and its cell in the
 * accent colour; columns one space apart, the last one cut at the edge.
 * Shared by table tabs and query results. */
void tui_draw_result_grid(TuiState *state, GridDrawParams *params) {
  if (!params || !params->win || !params->data || params->data->num_columns == 0 || !params->data->columns)
    return;
  WINDOW *win = params->win;
  ResultSet *data = params->data;
  int y0 = params->start_y, x0 = params->start_x;
  int end_y = params->start_y + params->height, end = params->start_x + params->width;
  if (params->height < 1 || params->width < 4)
    return;
  Tab *tab = state ? TUI_TAB(state) : NULL;
  const TableSchema *keys = tab && tab->type == TAB_TYPE_QUERY ? tab->query_source_schema : tab ? tab->schema : NULL;
  const TableSchema *sorts = params->sort_schema;

  for (int y = y0; y < end_y; y++) {
    tui_style(win, TC_FG, TC_BG, 0);
    tui_print_fit(win, y, x0, params->width, "");
  }

  /* the header */
  tui_style(win, TC_HEADER_FG, TC_HEADER_BG, A_BOLD);
  tui_print_fit(win, y0, x0, params->width, "");
  int x = x0;
  for (size_t col = params->scroll_col; col < data->num_columns && x < end; col++) {
    int w = grid_get_col_width(params, col);
    if (w > end - x)
      w = end - x;
    const char *name = data->columns[col].name ? data->columns[col].name : "";
    char *label = NULL;
    size_t sc = sorts ? db_schema_column_index(sorts, name) : SIZE_MAX;
    for (size_t i = 0; sc != SIZE_MAX && i < params->num_sort_entries; i++) {
      if (params->sort_entries[i].column != sc)
        continue;
      const char *rank = params->num_sort_entries > 1 && i < 8 ? SUPER[i + 1] : "";
      label = str_printf("%s%s%s", params->sort_entries[i].direction == SORT_DESC ? "\xe2\x86\x93" : "\xe2\x86\x91",
                         name, rank);
      break;
    }
    if (!label)
      label = str_dup(name);
    tui_style(win, col == params->cursor_col ? TC_ACCENT : TC_HEADER_FG, TC_HEADER_BG, A_BOLD);
    int lw = utf8_width(label, strlen(label));
    const ColumnDef *kc = schema_col(keys, name);
    bool pk = kc && kc->primary_key && lw + 2 <= w;
    tui_print_fit(win, y0, x, w, label);
    if (pk) {
      tui_style(win, TC_PK_FG, TC_HEADER_BG, A_BOLD);
      tui_print_fit(win, y0, x + lw + 1, 1, "\xe2\x9a\xb7"); /* ⚷ */
    }
    int fx = x + lw + (pk ? 3 : 1); /* a foreign key column can be followed */
    if (catalog_fk_first_column(keys, name) && fx < x + w) {
      tui_style(win, TC_ACCENT, TC_HEADER_BG, A_BOLD);
      tui_print_fit(win, y0, fx, 1, "\xe2\x87\x92"); /* ⇒ */
    }
    free(label);
    x += w + 1;
  }

  /* the rows */
  if (!data->rows)
    return;
  int y = y0 + 1;
  for (size_t row = params->scroll_row; row < data->num_rows && y < end_y; row++, y++) {
    Row *r = &data->rows[row];
    bool cur = row == params->cursor_row;
    bool sel = tab && tab_is_row_selected(tab, params->selection_offset + row);
    size_t global = params->selection_offset + row;
    ThemeColor bg = cur ? TC_CURSOR_BG : (global % 2 ? TC_STRIPE_BG : TC_ROW_BG);
    tui_style(win, TC_FG, bg, 0);
    tui_print_fit(win, y, x0, params->width, "");
    if (!r->cells)
      continue;
    x = x0;
    for (size_t col = params->scroll_col; col < data->num_columns && col < r->num_cells && x < end; col++) {
      int w = grid_get_col_width(params, col);
      if (w > end - x)
        w = end - x;
      bool cursor_cell = cur && col == params->cursor_col;
      if (cursor_cell && params->is_editing && params->is_focused) {
        int cx = tui_draw_edit_field(win, y, x, w, params->edit_buffer, params->edit_pos,
                                     tui_theme_attr(TC_FG, TC_EDIT_BG));
        if (cx >= 0 && state) {
          state->edit_cursor_y = y;
          state->edit_cursor_x = cx;
        }
      } else {
        draw_cell(win, y, x, w, &r->cells[col], bg, cursor_cell, sel);
      }
      x += w + 1;
    }
  }
  tui_style(win, TC_FG, TC_BG, 0);
  if (data->num_rows == 0 && y0 + 1 + (end_y - y0 - 1) / 2 < end_y) {
    tui_style(win, TC_DIM, TC_BG, 0);
    const char *msg = "No rows";
    int mx = x0 + (params->width - 7) / 2;
    tui_print_fit(win, y0 + 1 + (end_y - y0 - 1) / 2, mx > x0 ? mx : x0, 7, msg);
    tui_style(win, TC_FG, TC_BG, 0);
  }
}

/* ============================================================================
 * Header Drawing
 * ============================================================================
 */

char *tui_connstr_display(const char *connstr) {
  return connstr_display(connstr);
}

/* ============================================================================
 * Add Row Drawing
 * ============================================================================
 */

/* Helper to draw a single cell in the new row */
static void draw_add_row_cell(WINDOW *win, int y, int x, int width,
                              TuiState *state, size_t col, bool is_selected,
                              bool is_editing) {
  Tab *tab = TUI_TAB(state);
  TableSchema *schema = tab ? tab->schema : NULL;
  bool is_edited = state->new_row_edited && state->new_row_edited[col];
  const ColumnDef *cdef =
      schema && col < schema->num_columns ? &schema->columns[col] : NULL;
  /* Placeholders the database fills in: auto-increment, generated, or a
   * required column with no default (left for the database to reject) */
  const char *auto_label = NULL;
  if (cdef && cdef->generated)
    auto_label = "GENERATED";
  else if (cdef && cdef->auto_increment)
    auto_label = "AI";
  else if (cdef && !cdef->nullable &&
           !(cdef->default_val && cdef->default_val[0]))
    auto_label = "required";
  bool is_auto_increment = auto_label != NULL;
  bool is_placeholder = state->new_row_placeholders[col] && !is_edited;

  if (is_editing) {
    /* Draw edit field with distinctive background */
    int cursor_x = tui_draw_edit_field(win, y, x, width,
                                       state->new_row_edit_buffer,
                                       state->new_row_edit_pos,
                                       COLOR_PAIR(COLOR_EDIT));
    if (cursor_x >= 0)
      wmove(win, y, cursor_x);
  } else if (is_selected) {
    wattron(win, COLOR_PAIR(COLOR_SELECTED));
    mvwhline(win, y, x, ' ', width);

    if (is_placeholder && is_auto_increment) {
      /* Auto-increment placeholder - show in dim */
      wattron(win, A_DIM);
      tui_print_fit(win, y, x, width, auto_label);
      wattroff(win, A_DIM);
    } else if (is_placeholder) {
      /* Default value placeholder - show in dim */
      wattron(win, A_DIM);
      const DbValue *val = &state->new_row_values[col];
      char *str = db_value_to_string(val);
      tui_print_fit(win, y, x, width, str ? str : "");
      free(str);
      wattroff(win, A_DIM);
    } else {
      const DbValue *val = &state->new_row_values[col];
      if (val->is_null) {
        mvwprintw(win, y, x, "%-*s", width, "NULL");
      } else {
        char *str = db_value_to_string(val);
        if (str) {
          char *safe = tui_sanitize_for_display(str);
          tui_print_fit(win, y, x, width, safe ? safe : str);
          free(safe);
          free(str);
        }
      }
    }
    wattroff(win, COLOR_PAIR(COLOR_SELECTED));
  } else {
    /* Non-selected cell */
    if (is_placeholder && is_auto_increment) {
      /* Auto-increment placeholder - gray/dim */
      wattron(win, A_DIM);
      tui_print_fit(win, y, x, width, auto_label);
      wattroff(win, A_DIM);
    } else if (is_placeholder) {
      /* Default value placeholder - gray/dim */
      wattron(win, A_DIM);
      const DbValue *val = &state->new_row_values[col];
      char *str = db_value_to_string(val);
      tui_print_fit(win, y, x, width, str ? str : "");
      free(str);
      wattroff(win, A_DIM);
    } else {
      const DbValue *val = &state->new_row_values[col];
      if (val->is_null) {
        wattron(win, COLOR_PAIR(COLOR_NULL));
        mvwprintw(win, y, x, "%-*s", width, "NULL");
        wattroff(win, COLOR_PAIR(COLOR_NULL));
      } else {
        char *str = db_value_to_string(val);
        if (str) {
          char *safe = tui_sanitize_for_display(str);
          tui_print_fit(win, y, x, width, safe ? safe : str);
          free(safe);
          free(str);
        }
      }
    }
  }
}

/* The new row as the notcurses TUI's "Add row" overlay: one field per
 * column (its name, then its value or placeholder), the cursor's field
 * highlighted, over the grid */
static void tui_draw_add_row_overlay(TuiState *state, GridDrawParams *params) {
  if (!state || !state->adding_row || !params || !params->win)
    return;
  WINDOW *win = params->win;
  Tab *tab = TUI_TAB(state);
  TableSchema *schema = tab ? tab->schema : NULL;
  size_t n = state->new_row_num_cols;
  if (!schema || n == 0)
    return;
  int name_w = 4;
  for (size_t i = 0; i < n && i < schema->num_columns; i++) {
    const char *nm = schema->columns[i].name ? schema->columns[i].name : "";
    int w = utf8_width(nm, strlen(nm));
    if (w > name_w)
      name_w = w;
  }
  if (name_w > 24)
    name_w = 24;
  int cols = params->width - 8 < 70 ? params->width - 8 : 70;
  if (cols < 30)
    cols = 30 < params->width - 2 ? 30 : params->width - 2;
  int rows = (int)n + 1; /* the fields, then the message line */
  int max_rows = params->height - 4;
  if (rows > max_rows)
    rows = max_rows;
  if (rows < 2 || cols < 12)
    return;
  int h = rows + 2, w = cols + 2;
  int y0 = params->start_y + (params->height - h) / 2, x0 = params->start_x + (params->width - w) / 2;
  if (y0 < params->start_y)
    y0 = params->start_y;
  if (x0 < params->start_x)
    x0 = params->start_x;
  tui_frame_rect(win, y0, x0, h, w, "Add row",
                 "\xe2\x86\x91\xe2\x86\x93 field  Enter edit  ^N NULL  ^D empty  F2 save  Esc cancel");
  int vis = rows - 1;
  size_t cur = state->new_row_cursor_col;
  size_t scroll = cur >= (size_t)vis ? cur - (size_t)vis + 1 : 0;
  int vx = x0 + 2 + name_w + 2, vw = x0 + w - 1 - vx;
  for (int i = 0; i < vis; i++) {
    size_t col = scroll + (size_t)i;
    if (col >= n || col >= schema->num_columns)
      break;
    int y = y0 + 1 + i;
    bool is_cur = col == cur;
    tui_style(win, is_cur ? TC_CURSOR_FG : TC_OVERLAY_FG, is_cur ? TC_CURSOR_BG : TC_OVERLAY_BG, is_cur ? A_BOLD : 0);
    tui_print_fit(win, y, x0 + 1, w - 2, "");
    tui_print_fit(win, y, x0 + 2, name_w, schema->columns[col].name ? schema->columns[col].name : "");
    draw_add_row_cell(win, y, vx, vw, state, col, is_cur, is_cur && state->new_row_cell_editing);
  }
  tui_style(win, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
}

/* Print text centered on row y, clipped to the window width (UTF-8 aware) */
static void draw_centered_clip(WINDOW *win, int y, int win_cols,
                               const char *text) {
  int width = utf8_width(text, strlen(text));
  int x = width < win_cols ? (win_cols - width) / 2 : 0;
  tui_print_clip(win, y, x, win_cols - x, text);
}

/* ============================================================================
 * Table Drawing
 * ============================================================================
 */

void tui_draw_table(TuiState *state) {
  if (!state || !state->main_win)
    return;

  werase(state->main_win);

  /* Get actual window dimensions */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);

  /* The sub-view strip, then a list (every sub-view but Data) */
  tui_draw_subview_strip(state);
  Tab *tab = TUI_TAB(state);
  if (tab && tab->type == TAB_TYPE_TABLE && tab->subview != SUBVIEW_DATA) {
    tui_draw_subview(state);
    wrefresh(state->main_win);
    return;
  }

  /* Calculate filters panel height and draw if visible; the panel and the
   * grid sit under the strip */
  int filters_height = tui_subview_strip_rows(state);
  if (tui_filters_visible(state)) {
    filters_height += tui_get_filters_panel_height(state);
    tui_draw_filters_panel(state);
  }

  /* Get tab for model data, widget for view state */
  VmTable *widget = state->vm_table;
  if (!tab) {
    wrefresh(state->main_win);
    return;
  }

  /* Read model data from Tab */
  ResultSet *data = tab->data;

  /* Keep widths in step with the loaded rows (pages loaded after the first
   * one used to keep the first page's widths and truncate wider values) */
  if (widget) {
    table_vm_update_column_widths(widget);
  }

  /* Column widths from the viewmodel; cursor and scroll from the tab */
  int *col_widths = widget ? widget->col_widths : tab->col_widths;
  size_t num_col_widths = widget ? widget->num_col_widths : tab->num_col_widths;
  size_t cursor_row = tab->cursor_row;
  size_t cursor_col = tab->cursor_col;
  size_t scroll_row = tab->scroll_row;
  size_t scroll_col = tab->scroll_col;

  bool is_editing = state->editing;
  char *edit_buffer = state->edit_buffer;
  size_t edit_pos = state->edit_pos;

  /* Check if there's a table error (e.g., table doesn't exist) */
  if (tab && tab->table_error) {
    int center_y = filters_height + (win_rows - filters_height) / 2;
    wattron(state->main_win, COLOR_PAIR(COLOR_ERROR_TEXT) | A_BOLD);
    draw_centered_clip(state->main_win, center_y - 2, win_cols,
                       "Failed to load table:");
    wattroff(state->main_win, A_BOLD);
    draw_centered_clip(state->main_win, center_y - 1, win_cols,
                       tab->table_name ? tab->table_name : "(unknown)");

    /* Show actual error message (clipped to the window) */
    draw_centered_clip(state->main_win, center_y + 1, win_cols,
                       tab->table_error);
    wattroff(state->main_win, COLOR_PAIR(COLOR_ERROR_TEXT));

    wattron(state->main_win, A_DIM);
    char *close_key = hotkey_get_display(state->app->config, HOTKEY_CLOSE_TAB);
    char hint[128];
    snprintf(hint, sizeof(hint), "Press [%s] to close this tab",
             close_key ? close_key : "-");
    free(close_key);
    draw_centered_clip(state->main_win, center_y + 3, win_cols, hint);
    wattroff(state->main_win, A_DIM);
    wrefresh(state->main_win);
    return;
  }

  if (!data || data->num_columns == 0 || !data->columns) { /* as the notcurses grid says it */
    int msg_y = filters_height + 1 + (win_rows - filters_height - 1) / 2;
    tui_style(state->main_win, TC_DIM, TC_BG, 0);
    tui_print_fit(state->main_win, msg_y, (win_cols - 8) / 2, 8, "Loading\xe2\x80\xa6");
    tui_style(state->main_win, TC_FG, TC_BG, 0);
    wrefresh(state->main_win);
    return;
  }

  /* Use the shared grid drawing function */
  GridDrawParams params = {.win = state->main_win,
                           .start_y = filters_height,
                           .start_x = 0,
                           .height = win_rows - filters_height,
                           .width = win_cols,
                           .data = data,
                           .col_widths = col_widths,
                           .num_col_widths = num_col_widths,
                           .cursor_row = cursor_row,
                           .cursor_col = cursor_col,
                           .scroll_row = scroll_row,
                           .scroll_col = scroll_col,
                           .selection_offset = tab->loaded_offset,
                           .is_focused = !tui_sidebar_focused(state) &&
                                         !tui_filters_focused(state),
                           .is_editing = is_editing,
                           .edit_buffer = edit_buffer,
                           .edit_pos = edit_pos,
                           .show_header_line = true,
                           .sort_entries = tab ? tab->sort_entries : NULL,
                           .num_sort_entries = tab ? tab->num_sort_entries : 0,
                           .sort_schema = tab ? tab->schema : NULL};

  tui_draw_result_grid(state, &params);

  /* Draw add-row overlay if in add-row mode */
  if (state->adding_row && state->new_row_values &&
      state->new_row_num_cols > 0) {
    tui_draw_add_row_overlay(state, &params);
  }

  wrefresh(state->main_win);
}

/* ============================================================================
 * Connection Tab Drawing
 * ============================================================================
 */

/* A connection with no table or query open: what to do, as the notcurses
 * TUI says it */
void tui_draw_connection_tab(TuiState *state) {
  if (!state || !state->main_win)
    return;
  WINDOW *w = state->main_win;
  wbkgd(w, tui_theme_attr(TC_FG, TC_BG) | ' ');
  werase(w);
  int cols = getmaxx(w);
  tui_style(w, TC_DIM, TC_BG, 0);
  if (cols > 3)
    tui_print_fit(w, 0, 1, cols - 2, state->sidebar_win ? "Pick a table from the list" : "Press t to show the sidebar");
  tui_style(w, TC_FG, TC_BG, 0);
  wrefresh(w);
}


