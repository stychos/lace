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
#include "views/text_input_draw.h"
#include "tui_internal.h"
#include "views/list_picker.h"
#include "../../liblace/include/util/str.h"
#include "core/chrome.h"
#include "views/theme.h"
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

/* A field of a filter row: the focused one in the cursor colours */
static void draw_field(WINDOW *w, int y, int x, int width, const char *text, bool focused, bool dim) {
  if (width <= 0)
    return;
  if (focused)
    tui_style(w, TC_CURSOR_FG, TC_CURSOR_BG, A_BOLD);
  else
    tui_style(w, dim ? TC_DIM : TC_FG, TC_BG, 0);
  tui_print_fit(w, y, x, width, text ? text : "");
}

/* The first key of an action, for the title's hints */
static char *first_key(const Config *c, HotkeyAction a, const char *dflt) {
  char *all = hotkey_get_display(c, a);
  if (!all || !*all) {
    free(all);
    return str_dup(dflt);
  }
  char *comma = strchr(all, ',');
  if (comma)
    *comma = '\0';
  return all;
}

/* A filter row's name for its column */
static const char *filter_column_name(TuiState *state, const ColumnFilter *cf) {
  VmTable *vm = tui_vm_table(state);
  Tab *tab = TUI_TAB(state);
  if (vm) {
    const char *name = table_vm_column_name(vm, cf->column_index);
    if (name)
      return name;
  } else if (tab && tab->schema && cf->column_index < tab->schema->num_columns) {
    return tab->schema->columns[cf->column_index].name;
  }
  return "???";
}

/* Draw the filters panel, as the notcurses TUI's: a title row ("Filters
 * (n)" and the keys), then a row per filter with its column, operator and
 * value fields (two for BETWEEN) and a × that removes it */
void tui_draw_filters_panel(TuiState *state) {
  if (!state || !tui_filters_visible(state))
    return;
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;
  TableFilters *f = &tab->filters;
  WINDOW *w = state->main_win;
  int panel_height = tui_get_filters_panel_height(state);
  int win_cols = getmaxx(w);
  int start_y = tui_subview_strip_rows(state); /* under the sub-view strip */
  tui_style(w, TC_FG, TC_BG, 0);
  for (int row = 0; row < panel_height; row++)
    tui_print_fit(w, start_y + row, 0, win_cols, "");
  if (f->num_filters == 0) /* at least one row */
    filters_add(f, 0, FILTER_OP_EQ, "");

  size_t active = 0;
  for (size_t i = 0; i < f->num_filters; i++)
    active += f->filters[i].value[0] != '\0' || !filter_op_needs_value(f->filters[i].op);
  char *title = active ? str_printf("Filters (%zu)", active) : str_dup("Filters");
  tui_style(w, TC_ACCENT, TC_BG, A_BOLD);
  int tw = utf8_width(title, strlen(title));
  tui_print_fit(w, start_y, 1, tw < win_cols - 2 ? tw : win_cols - 2, title);
  const Config *cfg = state->app->config;
  char *ka = first_key(cfg, HOTKEY_ADD_FILTER, "+"), *kr = first_key(cfg, HOTKEY_REMOVE_FILTER, "-");
  char *kc = first_key(cfg, HOTKEY_CLEAR_FILTERS, "X"), *kt = first_key(cfg, HOTKEY_TOGGLE_FILTERS, "/");
  char *hints = str_printf("%s add  %s remove  %s clear  Enter edit  Tab next  Esc table  %s close", ka, kr, kc, kt);
  free(ka);
  free(kr);
  free(kc);
  free(kt);
  int vis = panel_height - 1;
  char *pos = f->num_filters > (size_t)vis ? str_printf("%zu/%zu", state->filters_cursor_row + 1, f->num_filters) : NULL;
  int pw = pos ? utf8_width(pos, strlen(pos)) : 0;
  int hx = 1 + tw + 2, hw = win_cols - 1 - pw - (pw ? 2 : 0) - hx;
  tui_style(w, TC_DIM, TC_BG, 0);
  if (hw > 8)
    tui_print_fit(w, start_y, hx, hw, hints);
  if (pos)
    tui_print_fit(w, start_y, win_cols - 1 - pw, pw, pos);
  free(title);
  free(hints);
  free(pos);

  filters_clamp_view(state, f);
  bool focused = tui_filters_focused(state);
  for (size_t i = state->filters_scroll; i < f->num_filters && i < state->filters_scroll + (size_t)vis; i++) {
    ColumnFilter *cf = &f->filters[i];
    int y = start_y + 1 + (int)(i - state->filters_scroll);
    bool raw = cf->column_index == FILTER_COL_RAW;
    bool two = !raw && cf->op == FILTER_OP_BETWEEN;
    bool on = focused && i == state->filters_cursor_row;
    size_t fc = state->filters_cursor_col;
    bool inactive = cf->value[0] == '\0' && (raw || filter_op_needs_value(cf->op));
    ChromeFilterLayout L;
    chrome_filter_layout(win_cols, two, &L);
    draw_field(w, y, L.col_x, L.col_w, raw ? "(raw SQL)" : filter_column_name(state, cf), on && fc == 0, inactive);
    draw_field(w, y, L.op_x, L.op_w, raw ? "-" : filter_op_name(cf->op), on && fc == 1, inactive || raw);
    if (raw || filter_op_needs_value(cf->op)) {
      if (on && fc == 2 && state->filters_editing)
        text_input_draw(w, &state->filters_edit, y, L.val_x, L.val_w, tui_theme_attr(TC_FG, TC_EDIT_BG), true);
      else
        draw_field(w, y, L.val_x, L.val_w, cf->value, on && fc == 2, false);
    }
    if (two) {
      tui_style(w, TC_DIM, TC_BG, 0);
      tui_print_fit(w, y, L.val_x + L.val_w, 5, " and ");
      if (on && fc == 3 && state->filters_editing)
        text_input_draw(w, &state->filters_edit, y, L.val2_x, L.val2_w, tui_theme_attr(TC_FG, TC_EDIT_BG), true);
      else
        draw_field(w, y, L.val2_x, L.val2_w, cf->value2, on && fc == 3, false);
    }
    /* the remove mark: a field of the row here (Tab reaches it) */
    size_t del_col = two ? 4 : 3;
    if (on && fc == del_col)
      tui_style(w, TC_CURSOR_FG, TC_CURSOR_BG, A_BOLD);
    else
      tui_style(w, TC_DIM, TC_BG, 0);
    tui_print_fit(w, y, L.del_x, 1, "\xc3\x97");
  }
  tui_style(w, TC_FG, TC_BG, 0);
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

/* Operators offered in the dropdown, in menu order: RAW is a virtual column */
static const FilterOperator VISIBLE_OPS[] = {
    FILTER_OP_EQ, FILTER_OP_NE, FILTER_OP_GT, FILTER_OP_GE, FILTER_OP_LT,
    FILTER_OP_LE, FILTER_OP_IN, FILTER_OP_CONTAINS, FILTER_OP_STARTS,
    FILTER_OP_ENDS, FILTER_OP_REGEX, FILTER_OP_BETWEEN, FILTER_OP_IS_EMPTY,
    FILTER_OP_IS_NOT_EMPTY, FILTER_OP_IS_NULL, FILTER_OP_IS_NOT_NULL,
};
#define FILTER_OP_VISIBLE (sizeof(VISIBLE_OPS) / sizeof(VISIBLE_OPS[0]))

/* Show operator dropdown and return the chosen FilterOperator (mapped back
 * through VISIBLE_OPS), or -1 if cancelled */
static int show_operator_dropdown(TuiState *state, FilterOperator current_op,
                                  size_t filter_row) {
  const char *labels[FILTER_OP_VISIBLE];
  size_t initial = 0;
  for (size_t i = 0; i < FILTER_OP_VISIBLE; i++) {
    labels[i] = filter_op_name(VISIBLE_OPS[i]);
    if (VISIBLE_OPS[i] == current_op)
      initial = i;
  }

  int main_y, main_x;
  getbegyx(state->main_win, main_y, main_x);
  int sel = tui_pick_from_list(state->app->config, "Operator", labels,
                               FILTER_OP_VISIBLE, initial,
                               main_y + (int)filter_row + 2, main_x + 18, 18,
                               60, 16);
  tui_refresh(state);
  if (sel < 0 || (size_t)sel >= FILTER_OP_VISIBLE)
    return -1;
  return (int)VISIBLE_OPS[sel];
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

  /* Escape - back to the table, the panel stays (as the notcurses TUI) */
  if (render_event_is_special(event, UI_KEY_ESCAPE)) {
    tui_set_filters_focused(state, false);
  }
  /* The filter hotkey (/) with the panel focused hides it */
  else if (hotkey_matches(cfg, event, HOTKEY_TOGGLE_FILTERS)) {
    tui_set_filters_visible(state, false);
    tui_set_filters_focused(state, false);
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
  Tab *pin = TUI_TAB(state);
  if (pin)
    pin->preview = false; /* a filtered view is kept */
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

  /* The fields where tui_draw_filters_panel puts them */
  int win_cols = getmaxx(state->main_win);
  const ColumnFilter *tf = &f->filters[target_filter];
  bool row_between = tf->column_index != FILTER_COL_RAW && tf->op == FILTER_OP_BETWEEN;
  ChromeFilterLayout L;
  chrome_filter_layout(win_cols, row_between, &L);

  size_t target_col;
  if (rel_x == L.del_x) {
    target_col = row_between ? 4 : 3; /* the remove mark */
  } else if (row_between && rel_x >= L.val2_x) {
    target_col = 3; /* Second value */
  } else if (rel_x >= L.val_x) {
    target_col = 2; /* Value field */
  } else if (rel_x >= L.op_x) {
    target_col = 1; /* Operator field */
  } else if (rel_x >= L.col_x) {
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
