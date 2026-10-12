/*
 * Lace
 * Table filters UI implementation
 *
 * Core filter logic (filters_init, filters_add, filters_build_where, etc.)
 * is in core/filters.c. This file contains only TUI-specific code.
 *
 * Filter panel state is stored in TuiState.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/config.h"
#include "../../liblace/include/util/mem.h"
#include "tui_internal.h"
#include "views/list_picker.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Start editing a filter value: the editor works on filters_edit_buffer,
 * which is as large as ColumnFilter's values */
static void filters_edit_load(TuiState *state, const char *text) {
  state->filters_edit_buffer[0] = '\0';
  text_input_bind(&state->filters_edit, state->filters_edit_buffer,
                  sizeof(state->filters_edit_buffer), false);
  text_input_set(&state->filters_edit, text);
}

/* Sentinel value for RAW filter (virtual column) */
#define FILTER_COL_RAW SIZE_MAX

/* Get panel height based on filter count */
int tui_get_filters_panel_height(TuiState *state) {
  if (!state || !tui_filters_visible(state))
    return 0;

  Tab *tab = TUI_TAB(state);
  if (!tab)
    return 1;
  int filter_rows = (int)tab->filters.num_filters;
  if (filter_rows < 1)
    filter_rows = 1; /* Always at least one filter row */
  if (filter_rows > MAX_VISIBLE_FILTERS)
    filter_rows = MAX_VISIBLE_FILTERS;

  return 1 + filter_rows; /* title + filters */
}

/* Store the value being edited in its filter field, end the edit and
 * apply the filters (Enter, Tab, or a click elsewhere in the panel) */
static void filters_commit_edit(TuiState *state, TableFilters *f) {
  if (state->filters_cursor_row < f->num_filters) {
    ColumnFilter *cf = &f->filters[state->filters_cursor_row];
    /* The edit buffer is as large as the values: it always fits */
    if (state->filters_cursor_col == 2) {
      snprintf(cf->value, sizeof(cf->value), "%s", state->filters_edit.buf);
    } else if (state->filters_cursor_col == 3 && cf->op == FILTER_OP_BETWEEN) {
      snprintf(cf->value2, sizeof(cf->value2), "%s", state->filters_edit.buf);
    }
  }
  state->filters_editing = false;
  tui_apply_filters(state); /* Auto-apply */
}

/* Keep the filter cursor and scroll inside the list (after a delete) */
static void filters_clamp_view(TuiState *state, const TableFilters *f) {
  if (state->filters_cursor_row >= f->num_filters)
    state->filters_cursor_row = f->num_filters > 0 ? f->num_filters - 1 : 0;
  size_t max_scroll =
      f->num_filters > MAX_VISIBLE_FILTERS ? f->num_filters - MAX_VISIBLE_FILTERS : 0;
  if (state->filters_scroll > max_scroll)
    state->filters_scroll = max_scroll;
  if (state->filters_cursor_row < state->filters_scroll)
    state->filters_scroll = state->filters_cursor_row;
}

/* Where the BETWEEN second value starts (the draw and click code agree) */
static int filters_val2_x(int val_x, int val_width) {
  int half_width = (val_width - 5) / 2; /* -5 for " AND " */
  if (half_width < 5)
    half_width = 5;
  return val_x + half_width + 5;
}

/* Draw the filters panel */
void tui_draw_filters_panel(TuiState *state) {
  if (!state || !tui_filters_visible(state))
    return;

  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;
  TableFilters *f = &tab->filters;

  int panel_height = tui_get_filters_panel_height(state);

  /* Get actual window dimensions */
  int win_cols;
  int win_rows;
  getmaxyx(state->main_win, win_rows, win_cols);
  (void)win_rows;

  int panel_width = win_cols;

  /* Draw within main_win at (0,0) - window is already positioned correctly */
  int start_x = 0;
  int start_y = 0;

  /* Draw panel background */
  for (int row = 0; row < panel_height; row++) {
    mvwhline(state->main_win, start_y + row, start_x, ' ', panel_width);
  }

  /* Ensure at least one filter exists */
  if (f->num_filters == 0) {
    filters_add(f, 0, FILTER_OP_EQ, "");
  }

  /* Count active filters */
  size_t active_count = 0;
  for (size_t i = 0; i < f->num_filters; i++) {
    ColumnFilter *cf = &f->filters[i];
    bool is_raw = (cf->column_index == FILTER_COL_RAW);
    if (cf->value[0] == '\0' && (is_raw || cf->op == FILTER_OP_EQ)) {
      continue;
    }
    active_count++;
  }

  /* Column positions */
  int col_x = start_x + 1; /* Align with title */
  int op_x = start_x + 17;
  int val_x = start_x + 31;
  int del_x = panel_width - 4;
  int val_width = del_x - val_x - 1; /* Dynamic width to fill available space */
  if (val_width < 10)
    val_width = 10;
  if (val_width > 255)
    val_width = 255;

  /* Title bar with hotkeys (like SQL editor) */
  wattron(state->main_win, A_BOLD);
  char *add_key = hotkey_get_display(state->app->config, HOTKEY_ADD_FILTER);
  char *rem_key = hotkey_get_display(state->app->config, HOTKEY_REMOVE_FILTER);
  char *clr_key = hotkey_get_display(state->app->config, HOTKEY_CLEAR_FILTERS);
  if (active_count > 0) {
    mvwprintw(state->main_win, start_y, col_x,
              "Filters (%zu) (%s/%s:add/del, %s:clear, Tab:next, Esc:close)",
              active_count, add_key ? add_key : "+", rem_key ? rem_key : "-",
              clr_key ? clr_key : "c");
  } else {
    mvwprintw(state->main_win, start_y, col_x,
              "Filters (%s/%s:add/del, %s:clear, Tab:next, Esc:close)",
              add_key ? add_key : "+", rem_key ? rem_key : "-",
              clr_key ? clr_key : "c");
  }
  free(add_key);
  free(rem_key);
  free(clr_key);
  wattroff(state->main_win, A_BOLD);

  /* Position indicator - right-aligned with delete button */
  if (f->num_filters > 1) {
    wattron(state->main_win, A_DIM);
    mvwprintw(state->main_win, start_y, del_x, "%zu/%zu",
              state->filters_cursor_row + 1, f->num_filters);
    wattroff(state->main_win, A_DIM);
  }

  int y = start_y + 1;

  /* Draw filter rows */
  filters_clamp_view(state, f);
  size_t visible_start = state->filters_scroll;
  size_t visible_count = f->num_filters - visible_start;
  if (visible_count > MAX_VISIBLE_FILTERS)
    visible_count = MAX_VISIBLE_FILTERS;

  for (size_t i = 0; i < visible_count; i++) {
    size_t filter_idx = visible_start + i;
    ColumnFilter *cf = &f->filters[filter_idx];
    bool row_selected =
        tui_filters_focused(state) && (state->filters_cursor_row == filter_idx);
    bool is_raw = (cf->column_index == FILTER_COL_RAW);

    /* Column name - use VmTable if available, fallback to Tab */
    const char *col_name = is_raw ? "(RAW)" : "???";
    if (!is_raw) {
      VmTable *vm = tui_vm_table(state);
      Tab *tab = TUI_TAB(state);
      if (vm) {
        const char *name = table_vm_column_name(vm, cf->column_index);
        if (name)
          col_name = name;
      } else if (tab && tab->schema &&
                 cf->column_index < tab->schema->num_columns) {
        col_name = tab->schema->columns[cf->column_index].name;
      }
    }

    /* Column field */
    if (row_selected && state->filters_cursor_col == 0) {
      wattron(state->main_win, A_REVERSE);
    }
    tui_print_fit(state->main_win, y, col_x, 14, col_name);
    if (row_selected && state->filters_cursor_col == 0) {
      wattroff(state->main_win, A_REVERSE);
    }

    /* Operator field - show "-" for RAW filters */
    if (is_raw) {
      wattron(state->main_win, A_DIM);
      mvwprintw(state->main_win, y, op_x, "%-12.12s", "-");
      wattroff(state->main_win, A_DIM);
    } else {
      if (row_selected && state->filters_cursor_col == 1) {
        wattron(state->main_win, A_REVERSE);
      }
      mvwprintw(state->main_win, y, op_x, "%-12.12s", filter_op_name(cf->op));
      if (row_selected && state->filters_cursor_col == 1) {
        wattroff(state->main_win, A_REVERSE);
      }
    }

    /* Value field(s) - BETWEEN has two values, others have one */
    bool is_between = (cf->op == FILTER_OP_BETWEEN);
    if (is_raw || filter_op_needs_value(cf->op)) {
      bool show_placeholder = is_raw && cf->value[0] == '\0';

      if (is_between) {
        /* BETWEEN: two value fields split evenly */
        int val2_x = filters_val2_x(val_x, val_width);
        int half_width = val2_x - val_x - 5;

        /* Value 1 */
        if (row_selected && state->filters_cursor_col == 2) {
          if (state->filters_editing) {
            text_input_draw(state->main_win, &state->filters_edit, y, val_x, half_width,
                                COLOR_PAIR(COLOR_EDIT), true);
          } else {
            wattron(state->main_win, A_REVERSE);
            tui_print_fit(state->main_win, y, val_x, half_width, cf->value);
            wattroff(state->main_win, A_REVERSE);
          }
        } else {
          tui_print_fit(state->main_win, y, val_x, half_width, cf->value);
        }

        /* " AND " separator */
        mvwprintw(state->main_win, y, val_x + half_width, " AND ");

        /* Value 2 */
        if (row_selected && state->filters_cursor_col == 3) {
          if (state->filters_editing) {
            text_input_draw(state->main_win, &state->filters_edit, y, val2_x, half_width,
                                COLOR_PAIR(COLOR_EDIT), true);
          } else {
            wattron(state->main_win, A_REVERSE);
            tui_print_fit(state->main_win, y, val2_x, half_width, cf->value2);
            wattroff(state->main_win, A_REVERSE);
          }
        } else {
          tui_print_fit(state->main_win, y, val2_x, half_width, cf->value2);
        }
      } else {
        /* Regular: single value field */
        const char *display_val = cf->value;
        if (row_selected && state->filters_cursor_col == 2) {
          if (state->filters_editing) {
            text_input_draw(state->main_win, &state->filters_edit, y, val_x, val_width,
                                COLOR_PAIR(COLOR_EDIT), true);
          } else {
            wattron(state->main_win, A_REVERSE);
            if (show_placeholder) {
              wattron(state->main_win, A_DIM);
              tui_print_fit(state->main_win, y, val_x, val_width, "WHERE ...");
              wattroff(state->main_win, A_DIM);
            } else {
              tui_print_fit(state->main_win, y, val_x, val_width, display_val);
            }
            wattroff(state->main_win, A_REVERSE);
          }
        } else {
          if (show_placeholder) {
            wattron(state->main_win, A_DIM);
            tui_print_fit(state->main_win, y, val_x, val_width, "WHERE ...");
            wattroff(state->main_win, A_DIM);
          } else {
            tui_print_fit(state->main_win, y, val_x, val_width, display_val);
          }
        }
      }
    }

    /* Delete button - column 3 for regular ops, column 4 for BETWEEN */
    int del_col = is_between ? 4 : 3;
    if (row_selected && state->filters_cursor_col == (size_t)del_col)
      wattron(state->main_win, A_REVERSE);
    mvwprintw(state->main_win, y, del_x, "[x]");
    if (row_selected && state->filters_cursor_col == (size_t)del_col)
      wattroff(state->main_win, A_REVERSE);

    y++;
  }
}

/* Show column dropdown and return selected index, or -1 if cancelled.
 * Returns 0 to num_cols-1 for regular columns, num_cols for RAW.
 * Caller must check if result == schema->num_columns to detect RAW. */
static ssize_t show_column_dropdown(TuiState *state, size_t current_col,
                                    size_t filter_row) {
  VmTable *vm = tui_vm_table(state);
  Tab *tab = TUI_TAB(state);
  const TableSchema *schema =
      vm ? table_vm_get_schema(vm) : (tab ? tab->schema : NULL);
  if (!schema || schema->num_columns == 0)
    return -1;

  /* The columns, then (RAW): index num_columns means RAW to the caller */
  size_t n = schema->num_columns + 1;
  const char **labels = safe_calloc(n, sizeof(*labels));
  for (size_t i = 0; i < schema->num_columns; i++)
    labels[i] = schema->columns[i].name ? schema->columns[i].name : "?";
  labels[n - 1] = "(RAW)";
  size_t current =
      current_col == FILTER_COL_RAW ? n - 1 : current_col;

  int main_y, main_x;
  getbegyx(state->main_win, main_y, main_x);
  int sel = tui_pick_from_list(state->app->config, "Column", labels, n,
                               current, main_y + (int)filter_row + 2,
                               main_x + 2, 20, 40, 15);
  free(labels);
  tui_refresh(state);
  return sel;
}

/* Number of operators to show (excludes RAW which is now a virtual column) */
#define FILTER_OP_VISIBLE (FILTER_OP_COUNT - 1)

/* Show operator dropdown and return selected index, or -1 if cancelled */
static int show_operator_dropdown(TuiState *state, FilterOperator current_op,
                                  size_t filter_row) {
  const char *labels[FILTER_OP_VISIBLE];
  for (int i = 0; i < FILTER_OP_VISIBLE; i++)
    labels[i] = filter_op_name((FilterOperator)i); /* RAW is not offered */

  int main_y, main_x;
  getbegyx(state->main_win, main_y, main_x);
  int sel = tui_pick_from_list(state->app->config, "Operator", labels,
                               FILTER_OP_VISIBLE, (size_t)current_op,
                               main_y + (int)filter_row + 2, main_x + 18, 18,
                               60, 16);
  tui_refresh(state);
  return sel;
}

/* Handle filters panel input */
bool tui_handle_filters_input(TuiState *state, const UiEvent *event) {
  if (!state || !tui_filters_visible(state) || !tui_filters_focused(state))
    return false;
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  Tab *tab = TUI_TAB(state);
  if (!tab)
    return false;
  TableFilters *f = &tab->filters;

  /* Ensure at least one filter exists */
  if (f->num_filters == 0) {
    filters_add(f, 0, FILTER_OP_EQ, "");
  }

  bool is_tab = render_event_is_special(event, UI_KEY_TAB);
  bool is_btab = render_event_is_special(event, UI_KEY_BTAB);

  /* Handle editing mode */
  if (state->filters_editing) {
    if (render_event_is_special(event, UI_KEY_ESCAPE)) {
      /* Escape - cancel edit */
      state->filters_editing = false;
    } else if (render_event_is_special(event, UI_KEY_ENTER) || is_tab ||
               is_btab) {
      /* Enter, or Tab/Shift+Tab before moving on: confirm edit */
      filters_commit_edit(state, f);
    } else if (render_event_is_ctrl(event, 'K')) {
      /* Ctrl+K - copy filter value to clipboard */
      if (state->filters_edit.len > 0) {
        tui_clipboard_copy(state, state->filters_edit.buf);
        tui_set_status(state, "Copied to clipboard");
      }
    } else if (render_event_is_ctrl(event, 'U')) {
      /* Ctrl+U - paste from clipboard at the cursor */
      char *paste_text = tui_clipboard_read(state);
      text_input_insert(&state->filters_edit, paste_text);
      free(paste_text);
    } else {
      text_input_handle_key(&state->filters_edit, event);
    }
    if (!(is_tab || is_btab))
      return true;
    /* Tab/Shift+Tab confirmed the edit; now move like in navigation mode */
  }

  /* Navigation mode */
  const Config *cfg = state->app ? state->app->config : NULL;

  /* Escape - close panel */
  if (render_event_is_special(event, UI_KEY_ESCAPE)) {
    tui_set_filters_visible(state, false);
    tui_set_filters_focused(state, false);
  }
  /* Filter hotkey (f, /) - switch focus to table if filters exist, else close */
  else if (hotkey_matches(cfg, event, HOTKEY_TOGGLE_FILTERS)) {
    if (f && f->num_filters > 0) {
      /* Filters exist - just switch focus to table, keep panel visible */
      tui_set_filters_focused(state, false);
    } else {
      /* No filters - close the panel */
      tui_set_filters_visible(state, false);
      tui_set_filters_focused(state, false);
    }
  }
  /* Up / k - move cursor up */
  else if (hotkey_matches(cfg, event, HOTKEY_MOVE_UP)) {
    if (state->filters_cursor_row > 0) {
      state->filters_cursor_row--;
      /* Adjust scroll if cursor moved above visible area */
      if (state->filters_cursor_row < state->filters_scroll) {
        state->filters_scroll = state->filters_cursor_row;
      }
    }
  }
  /* Down / j - move cursor down */
  else if (hotkey_matches(cfg, event, HOTKEY_MOVE_DOWN)) {
    if (state->filters_cursor_row < f->num_filters - 1) {
      state->filters_cursor_row++;
      /* Adjust scroll if cursor moved below visible area */
      if (state->filters_cursor_row >=
          state->filters_scroll + MAX_VISIBLE_FILTERS) {
        state->filters_scroll =
            state->filters_cursor_row - MAX_VISIBLE_FILTERS + 1;
      }
    } else {
      /* At last filter row - move focus to table */
      tui_set_filters_focused(state, false);
    }
  }
  /* Left / h - move cursor left */
  else if (hotkey_matches(cfg, event, HOTKEY_MOVE_LEFT)) {
    size_t idx = state->filters_cursor_row;
    ColumnFilter *cf = &f->filters[idx];
    bool is_raw = (cf->column_index == FILTER_COL_RAW);
    bool needs_value = filter_op_needs_value(cf->op);
    if (state->filters_cursor_col > 0) {
      state->filters_cursor_col--;
      /* Skip operator column for RAW filters */
      if (is_raw && state->filters_cursor_col == 1)
        state->filters_cursor_col = 0;
      /* Skip value column for is* operators that don't need a value */
      if (!is_raw && !needs_value && state->filters_cursor_col == 2)
        state->filters_cursor_col = 1;
    } else if (tui_sidebar_visible(state)) {
      /* At leftmost column - move focus to sidebar */
      state->filters_was_focused = true; /* Remember filters were focused */
      tui_set_sidebar_focused(state, true);
      tui_set_filters_focused(state, false);
      /* Restore last sidebar position */
      tui_set_sidebar_highlight(state, state->sidebar_last_position);
    }
  }
  /* Right / l - move cursor right */
  else if (hotkey_matches(cfg, event, HOTKEY_MOVE_RIGHT)) {
    size_t idx = state->filters_cursor_row;
    ColumnFilter *cf = &f->filters[idx];
    bool is_raw = (cf->column_index == FILTER_COL_RAW);
    bool is_between = (cf->op == FILTER_OP_BETWEEN);
    bool needs_value = filter_op_needs_value(cf->op);
    size_t max_col = is_between ? 4 : 3; /* BETWEEN has extra value2 column */
    if (state->filters_cursor_col < max_col) {
      state->filters_cursor_col++;
      /* Skip operator column for RAW filters */
      if (is_raw && state->filters_cursor_col == 1)
        state->filters_cursor_col = 2;
      /* Skip value column for is* operators that don't need a value */
      if (!is_raw && !needs_value && state->filters_cursor_col == 2)
        state->filters_cursor_col = 3;
    }
  }
  /* Tab / Shift+Tab - next or previous field, row by row; past the last
   * field (or before the first) focus moves on to the next panel */
  else if (is_tab || is_btab) {
    size_t row = state->filters_cursor_row;
    size_t max_col = f->filters[row].op == FILTER_OP_BETWEEN ? 4 : 3;
    if (is_tab) {
      if (state->filters_cursor_col < max_col) {
        state->filters_cursor_col++;
      } else if (row + 1 < f->num_filters) {
        state->filters_cursor_row++;
        state->filters_cursor_col = 0;
      } else {
        tui_focus_cycle(state, 1);
        return true;
      }
    } else {
      if (state->filters_cursor_col > 0) {
        state->filters_cursor_col--;
      } else if (row > 0) {
        state->filters_cursor_row--;
        state->filters_cursor_col =
            f->filters[row - 1].op == FILTER_OP_BETWEEN ? 4 : 3;
      } else {
        tui_focus_cycle(state, -1);
        return true;
      }
    }
  }
  /* Enter - activate field */
  else if (render_event_is_special(event, UI_KEY_ENTER)) {
    size_t filter_idx = state->filters_cursor_row;
    ColumnFilter *cf = &f->filters[filter_idx];
    bool is_raw = (cf->column_index == FILTER_COL_RAW);
    bool is_between = (cf->op == FILTER_OP_BETWEEN);
    size_t del_col = is_between ? 4 : 3;

    if (state->filters_cursor_col == 0) {
      /* Column - show dropdown */
      ssize_t sel = show_column_dropdown(state, cf->column_index,
                                         state->filters_cursor_row);
      if (sel >= 0) {
        /* Check if RAW was selected (index == num_columns) */
        Tab *col_tab = TUI_TAB(state);
        size_t num_cols = (col_tab && col_tab->schema) ? col_tab->schema->num_columns : 0;
        if ((size_t)sel == num_cols) {
          cf->column_index = FILTER_COL_RAW;
        } else {
          cf->column_index = (size_t)sel;
        }
        /* Only apply if filter has a value or operator doesn't need one */
        if (cf->value[0] != '\0' || !filter_op_needs_value(cf->op)) {
          tui_apply_filters(state);
        }
      }
    } else if (state->filters_cursor_col == 1) {
      /* Operator - show dropdown (not for RAW) */
      if (!is_raw) {
        int sel =
            show_operator_dropdown(state, cf->op, state->filters_cursor_row);
        if (sel >= 0) {
          FilterOperator new_op = (FilterOperator)sel;
          bool had_effect =
              cf->value[0] != '\0' || !filter_op_needs_value(cf->op);
          bool will_have_effect =
              cf->value[0] != '\0' || !filter_op_needs_value(new_op);
          cf->op = new_op;
          /* Clear value2 if switching away from BETWEEN */
          if (cf->op != FILTER_OP_BETWEEN) {
            cf->value2[0] = '\0';
          }
          /* Only apply if filter had or will have effect */
          if (had_effect || will_have_effect) {
            tui_apply_filters(state);
          }
        }
      }
    } else if (state->filters_cursor_col == 2) {
      /* Value - edit */
      if (is_raw || filter_op_needs_value(cf->op)) {
        state->filters_editing = true;
        filters_edit_load(state, cf->value);
      }
    } else if (state->filters_cursor_col == 3 && is_between) {
      /* Value2 - edit (only for BETWEEN) */
      state->filters_editing = true;
      filters_edit_load(state, cf->value2);
    } else if (state->filters_cursor_col == del_col) {
      /* Delete */
      bool had_effect = cf->value[0] != '\0' || !filter_op_needs_value(cf->op);
      if (is_between) {
        had_effect = had_effect || cf->value2[0] != '\0';
      }
      if (f->num_filters > 1) {
        filters_remove(f, filter_idx);
        filters_clamp_view(state, f);
      } else {
        /* Last filter - remove it and close panel */
        filters_remove(f, filter_idx);
        tui_set_filters_visible(state, false);
        tui_set_filters_focused(state, false);
      }
      /* Only apply if deleted filter had effect */
      if (had_effect) {
        tui_apply_filters(state);
      }
    }
  }
  /* + or = - add new filter */
  else if (hotkey_matches(cfg, event, HOTKEY_ADD_FILTER)) {
    Tab *add_tab = TUI_TAB(state);
    if (add_tab && add_tab->schema && add_tab->schema->num_columns > 0) {
      filters_add(f, 0, FILTER_OP_EQ, "");
      state->filters_cursor_row = f->num_filters - 1;
      state->filters_cursor_col = 0;
      /* Scroll to show new filter */
      if (state->filters_cursor_row >=
          state->filters_scroll + MAX_VISIBLE_FILTERS) {
        state->filters_scroll =
            state->filters_cursor_row - MAX_VISIBLE_FILTERS + 1;
      }
    }
  }
  /* c - copy current filter value to clipboard */
  else if (hotkey_matches(cfg, event, HOTKEY_CELL_COPY)) {
    size_t filter_idx = state->filters_cursor_row;
    if (filter_idx < f->num_filters) {
      ColumnFilter *cf = &f->filters[filter_idx];
      const char *value_to_copy = NULL;
      if (state->filters_cursor_col == 2 && cf->value[0]) {
        value_to_copy = cf->value;
      } else if (state->filters_cursor_col == 3 &&
                 cf->op == FILTER_OP_BETWEEN && cf->value2[0]) {
        value_to_copy = cf->value2;
      } else if (cf->value[0]) {
        /* Default to value if cursor not on a value column */
        value_to_copy = cf->value;
      }
      if (value_to_copy) {
        tui_clipboard_copy(state, value_to_copy);
        tui_set_status(state, "Copied to clipboard");
      }
    }
  }
  /* v - paste into current filter value */
  else if (hotkey_matches(cfg, event, HOTKEY_CELL_PASTE)) {
    size_t filter_idx = state->filters_cursor_row;
    if (filter_idx < f->num_filters) {
      ColumnFilter *cf = &f->filters[filter_idx];
      char *paste_text = tui_clipboard_read(state);
      if (paste_text && paste_text[0]) {
        /* Strip newlines */
        for (char *p = paste_text; *p; p++) {
          if (*p == '\n' || *p == '\r')
            *p = ' ';
        }
        /* Paste into appropriate value field */
        if (state->filters_cursor_col == 3 && cf->op == FILTER_OP_BETWEEN) {
          strncpy(cf->value2, paste_text, sizeof(cf->value2) - 1);
          cf->value2[sizeof(cf->value2) - 1] = '\0';
        } else {
          strncpy(cf->value, paste_text, sizeof(cf->value) - 1);
          cf->value[sizeof(cf->value) - 1] = '\0';
          /* Move cursor to value column if not there */
          if (state->filters_cursor_col < 2)
            state->filters_cursor_col = 2;
        }
        tui_apply_filters(state);
        tui_set_status(state, "Pasted from clipboard");
      }
      free(paste_text);
    }
  }
  /* X - clear all filters */
  else if (hotkey_matches(cfg, event, HOTKEY_CLEAR_FILTERS)) {
    /* Check if any filters had effect before clearing */
    bool had_effect = false;
    for (size_t i = 0; i < f->num_filters; i++) {
      ColumnFilter *cf = &f->filters[i];
      if (cf->value[0] != '\0' || !filter_op_needs_value(cf->op)) {
        had_effect = true;
        break;
      }
    }
    filters_clear(f);
    filters_add(f, 0, FILTER_OP_EQ, "");
    state->filters_cursor_row = 0;
    state->filters_cursor_col = 0;
    state->filters_scroll = 0;
    /* Only apply if there were active filters */
    if (had_effect) {
      tui_apply_filters(state);
    }
  }
  /* - or x or Delete - delete current filter */
  else if (hotkey_matches(cfg, event, HOTKEY_REMOVE_FILTER)) {
    size_t filter_idx = state->filters_cursor_row;
    ColumnFilter *cf = &f->filters[filter_idx];
    /* Check if filter had an effect before deleting */
    bool had_effect = cf->value[0] != '\0' || !filter_op_needs_value(cf->op);
    if (f->num_filters > 1) {
      filters_remove(f, filter_idx);
      if (state->filters_cursor_row >= f->num_filters)
        state->filters_cursor_row = f->num_filters - 1;
      /* Adjust scroll if needed */
      if (state->filters_scroll > 0 &&
          f->num_filters <= state->filters_scroll + MAX_VISIBLE_FILTERS) {
        state->filters_scroll = f->num_filters > MAX_VISIBLE_FILTERS
                                    ? f->num_filters - MAX_VISIBLE_FILTERS
                                    : 0;
      }
    } else {
      /* Last filter - remove it and close panel */
      filters_remove(f, filter_idx);
      tui_set_filters_visible(state, false);
      tui_set_filters_focused(state, false);
    }
    /* Only apply if deleted filter had effect */
    if (had_effect) {
      tui_apply_filters(state);
    }
  }
  /* Global keys - pass through to main loop */
  else if (hotkey_matches(cfg, event, HOTKEY_PREV_TAB) ||
           hotkey_matches(cfg, event, HOTKEY_NEXT_TAB) ||
           hotkey_matches(cfg, event, HOTKEY_PREV_WORKSPACE) ||
           hotkey_matches(cfg, event, HOTKEY_NEXT_WORKSPACE) ||
           hotkey_matches(cfg, event, HOTKEY_TOGGLE_SIDEBAR) ||
           hotkey_matches(cfg, event, HOTKEY_TOGGLE_HEADER) ||
           hotkey_matches(cfg, event, HOTKEY_TOGGLE_STATUS) ||
           hotkey_matches(cfg, event, HOTKEY_OPEN_QUERY) ||
           hotkey_matches(cfg, event, HOTKEY_REFRESH) ||
           hotkey_matches(cfg, event, HOTKEY_SHOW_SCHEMA) ||
           hotkey_matches(cfg, event, HOTKEY_QUIT) ||
           hotkey_matches(cfg, event, HOTKEY_CONFIG)) {
    return false; /* Let global keys work from filters */
  }
  /* Printable character on Value column(s) - start editing immediately */
  else if ((state->filters_cursor_col == 2 ||
            (state->filters_cursor_col == 3 &&
             f->filters[state->filters_cursor_row].op == FILTER_OP_BETWEEN)) &&
           render_event_is_char(event)) {
    int key_char = render_event_get_char(event);
    /* Any text key except a stray UTF-8 continuation byte */
    if (utf8_is_text_key(key_char) && !(key_char >= 0x80 && key_char < 0xC0)) {
      size_t filter_idx = state->filters_cursor_row;
      ColumnFilter *cf = &f->filters[filter_idx];
      bool is_raw = (cf->column_index == FILTER_COL_RAW);

      if (is_raw || filter_op_needs_value(cf->op)) {
        /* Start editing with the typed character */
        state->filters_editing = true;
        filters_edit_load(state, "");
        text_input_handle_key(&state->filters_edit, event);
      }
    }
  }
  /* Consume all other keys when filters focused */

  return true;
}

/* Apply current filters and reload data */
void tui_apply_filters(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE || !tab->table_name)
    return;

  /* Cancel any pending background load before reload */
  tui_cancel_background_load(state);

  /* Reload table data with filters applied */
  tui_load_table_data(state, tab->table_name);

  /* Update status - count only active (non-empty) filters */
  TableFilters *f = &tab->filters;
  size_t active_count = 0;
  for (size_t i = 0; i < f->num_filters; i++) {
    ColumnFilter *cf = &f->filters[i];
    bool is_raw = (cf->column_index == FILTER_COL_RAW);
    /* Skip empty = filters and empty RAW filters */
    if (cf->value[0] == '\0' && (is_raw || cf->op == FILTER_OP_EQ)) {
      continue;
    }
    active_count++;
  }

  if (active_count > 0) {
    tui_set_status(state, "%zu rows (%zu filter%s applied)", tab->total_rows,
                   active_count, active_count == 1 ? "" : "s");
  } else {
    tui_set_status(state, "%zu rows", tab->total_rows);
  }
}

/* Handle mouse click in filters panel
 * rel_x, rel_y are coordinates relative to main_win (0,0)
 * Returns true if click was handled */
bool tui_handle_filters_click(TuiState *state, int rel_x, int rel_y) {
  if (!state || !tui_filters_visible(state))
    return false;

  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE)
    return false;

  TableFilters *f = &tab->filters;
  if (f->num_filters == 0)
    return false;

  /* Row 0 is title bar, rows 1+ are filter rows */
  if (rel_y < 1)
    return true; /* Clicked on title bar - just focus filters */

  /* A value being typed is kept, as when Tab leaves the field */
  if (state->filters_editing) {
    filters_commit_edit(state, f);
  }

  int filter_row_idx = rel_y - 1; /* 0-based filter row within visible area */

  /* Account for scroll */
  size_t target_filter = state->filters_scroll + (size_t)filter_row_idx;
  if (target_filter >= f->num_filters)
    return true; /* Clicked below filters - just focus */

  /* Column positions (from tui_draw_filters_panel):
   * Column field: x = 1, width = 14
   * Operator field: x = 17, width = 12
   * Value field: x = 31, width = dynamic
   * Delete button: x = panel_width - 4, width = 3 */
  int col_x = 1;
  int op_x = 17;
  int val_x = 31;

  /* Get panel width for delete button position */
  int win_cols;
  int win_rows;
  getmaxyx(state->main_win, win_rows, win_cols);
  (void)win_rows;
  int del_x = win_cols - 4;
  int val_width = del_x - val_x - 1; /* As tui_draw_filters_panel */
  if (val_width < 10)
    val_width = 10;
  if (val_width > 255)
    val_width = 255;
  /* BETWEEN rows have two value fields, and the delete button is the
   * fifth field (cursor column 4) */
  bool row_between = f->filters[target_filter].op == FILTER_OP_BETWEEN;

  /* Determine which field was clicked */
  size_t target_col;
  if (rel_x >= del_x && rel_x < del_x + 3) {
    target_col = row_between ? 4 : 3; /* Delete button */
  } else if (row_between && rel_x >= filters_val2_x(val_x, val_width)) {
    target_col = 3; /* Second value */
  } else if (rel_x >= val_x) {
    target_col = 2; /* Value field */
  } else if (rel_x >= op_x) {
    target_col = 1; /* Operator field */
  } else if (rel_x >= col_x) {
    target_col = 0; /* Column field */
  } else {
    return true; /* Clicked in margin */
  }

  /* Update cursor position */
  state->filters_cursor_row = target_filter;
  state->filters_cursor_col = target_col;
  tui_set_filters_focused(state, true);
  tui_set_sidebar_focused(state, false);

  /* Get the filter to check if RAW or BETWEEN */
  ColumnFilter *cf = &f->filters[target_filter];
  bool is_raw = (cf->column_index == FILTER_COL_RAW);
  bool is_between = (cf->op == FILTER_OP_BETWEEN);
  size_t del_col = is_between ? 4 : 3;

  /* Refresh display to show highlighted field before opening dropdown */
  tui_refresh(state);

  /* Open dropdown for column or operator fields */
  if (target_col == 0) {
    /* Column field - show column dropdown */
    ssize_t sel = show_column_dropdown(state, cf->column_index, target_filter);
    if (sel >= 0) {
      /* Check if RAW was selected (index == num_columns) */
      Tab *click_tab = TUI_TAB(state);
      size_t num_cols = (click_tab && click_tab->schema) ? click_tab->schema->num_columns : 0;
      if ((size_t)sel == num_cols) {
        cf->column_index = FILTER_COL_RAW;
      } else {
        cf->column_index = (size_t)sel;
      }
      if (cf->value[0] != '\0' || !filter_op_needs_value(cf->op)) {
        tui_apply_filters(state);
      }
    }
  } else if (target_col == 1 && !is_raw) {
    /* Operator field - show operator dropdown (not for RAW) */
    int sel = show_operator_dropdown(state, cf->op, target_filter);
    if (sel >= 0) {
      FilterOperator new_op = (FilterOperator)sel;
      bool had_effect = cf->value[0] != '\0' || !filter_op_needs_value(cf->op);
      bool will_have_effect =
          cf->value[0] != '\0' || !filter_op_needs_value(new_op);
      cf->op = new_op;
      /* Clear value2 if switching away from BETWEEN */
      if (cf->op != FILTER_OP_BETWEEN) {
        cf->value2[0] = '\0';
      }
      if (had_effect || will_have_effect) {
        tui_apply_filters(state);
      }
    }
  } else if (target_col == 2 && (is_raw || filter_op_needs_value(cf->op))) {
    /* Value field - start editing */
    state->filters_editing = true;
    filters_edit_load(state, cf->value);
  } else if (target_col == 3 && is_between) {
    /* Value2 field for BETWEEN - start editing */
    state->filters_editing = true;
    filters_edit_load(state, cf->value2);
  } else if (target_col == del_col) {
    /* Delete button - delete filter */
    bool had_effect = cf->value[0] != '\0' || !filter_op_needs_value(cf->op);
    if (is_between) {
      had_effect = had_effect || cf->value2[0] != '\0';
    }
    if (f->num_filters > 1) {
      filters_remove(f, target_filter);
      filters_clamp_view(state, f);
    } else {
      /* Last filter - remove it and close panel */
      filters_remove(f, target_filter);
      tui_set_filters_visible(state, false);
      tui_set_filters_focused(state, false);
    }
    if (had_effect) {
      tui_apply_filters(state);
    }
  }

  return true;
}
