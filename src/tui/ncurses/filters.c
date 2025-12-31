/*
 * Lace
 * Table filters UI implementation
 *
 * Core filter logic (filters_init, filters_add, filters_build_where, etc.)
 * is in core/filters.c. This file contains only TUI-specific code.
 *
 * Uses VmTable for schema access where applicable.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../config/config.h"
#include "../../viewmodel/vm_table.h"
#include "tui_internal.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Helper to get VmTable, returns NULL if not valid */
static VmTable *get_vm_table(TuiState *state) {
  if (!state || !state->vm_table)
    return NULL;
  if (!vm_table_valid(state->vm_table))
    return NULL;
  return state->vm_table;
}

/* Sentinel value for RAW filter (virtual column) */
#define FILTER_COL_RAW SIZE_MAX

/* Get panel height based on filter count */
int tui_get_filters_panel_height(TuiState *state) {
  if (!state || !state->filters_visible)
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

/* Draw the filters panel */
void tui_draw_filters_panel(TuiState *state) {
  if (!state || !state->filters_visible)
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
  char *sw_key =
      hotkey_get_display(state->app->config, HOTKEY_FILTERS_SWITCH_FOCUS);
  if (active_count > 0) {
    mvwprintw(state->main_win, start_y, col_x,
              "Filters (%zu) (%s/%s:add/del, %s:clear, %s:switch, Esc)",
              active_count, add_key ? add_key : "+", rem_key ? rem_key : "-",
              clr_key ? clr_key : "c", sw_key ? sw_key : "^W");
  } else {
    mvwprintw(state->main_win, start_y, col_x,
              "Filters (%s/%s:add/del, %s:clear, %s:switch, Esc)",
              add_key ? add_key : "+", rem_key ? rem_key : "-",
              clr_key ? clr_key : "c", sw_key ? sw_key : "^W");
  }
  free(add_key);
  free(rem_key);
  free(clr_key);
  free(sw_key);
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
  size_t visible_start = state->filters_scroll;
  size_t visible_count = f->num_filters - visible_start;
  if (visible_count > MAX_VISIBLE_FILTERS)
    visible_count = MAX_VISIBLE_FILTERS;

  for (size_t i = 0; i < visible_count; i++) {
    size_t filter_idx = visible_start + i;
    ColumnFilter *cf = &f->filters[filter_idx];
    bool row_selected =
        state->filters_focused && (state->filters_cursor_row == filter_idx);
    bool is_raw = (cf->column_index == FILTER_COL_RAW);

    /* Column name - use VmTable if available */
    const char *col_name = is_raw ? "(RAW)" : "???";
    if (!is_raw) {
      VmTable *vm = get_vm_table(state);
      if (vm) {
        const char *name = vm_table_column_name(vm, cf->column_index);
        if (name)
          col_name = name;
      } else if (state->schema &&
                 cf->column_index < state->schema->num_columns) {
        col_name = state->schema->columns[cf->column_index].name;
      }
    }

    /* Column field */
    if (row_selected && state->filters_cursor_col == 0) {
      wattron(state->main_win, A_REVERSE);
    }
    mvwprintw(state->main_win, y, col_x, "%-14.14s", col_name);
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
        int half_width = (val_width - 5) / 2; /* -5 for " AND " */
        if (half_width < 5)
          half_width = 5;
        int val2_x = val_x + half_width + 5;

        /* Value 1 */
        if (row_selected && state->filters_cursor_col == 2) {
          if (state->filters_editing) {
            wattron(state->main_win, COLOR_PAIR(COLOR_EDIT));
            mvwprintw(state->main_win, y, val_x, "%-*.*s", half_width,
                      half_width, state->filters_edit_buffer);
            wattroff(state->main_win, COLOR_PAIR(COLOR_EDIT));
          } else {
            wattron(state->main_win, A_REVERSE);
            mvwprintw(state->main_win, y, val_x, "%-*.*s", half_width,
                      half_width, cf->value);
            wattroff(state->main_win, A_REVERSE);
          }
        } else {
          mvwprintw(state->main_win, y, val_x, "%-*.*s", half_width, half_width,
                    cf->value);
        }

        /* " AND " separator */
        mvwprintw(state->main_win, y, val_x + half_width, " AND ");

        /* Value 2 */
        if (row_selected && state->filters_cursor_col == 3) {
          if (state->filters_editing) {
            wattron(state->main_win, COLOR_PAIR(COLOR_EDIT));
            mvwprintw(state->main_win, y, val2_x, "%-*.*s", half_width,
                      half_width, state->filters_edit_buffer);
            wattroff(state->main_win, COLOR_PAIR(COLOR_EDIT));
          } else {
            wattron(state->main_win, A_REVERSE);
            mvwprintw(state->main_win, y, val2_x, "%-*.*s", half_width,
                      half_width, cf->value2);
            wattroff(state->main_win, A_REVERSE);
          }
        } else {
          mvwprintw(state->main_win, y, val2_x, "%-*.*s", half_width, half_width,
                    cf->value2);
        }
      } else {
        /* Regular: single value field */
        const char *display_val = cf->value;
        if (row_selected && state->filters_cursor_col == 2) {
          if (state->filters_editing) {
            wattron(state->main_win, COLOR_PAIR(COLOR_EDIT));
            mvwprintw(state->main_win, y, val_x, "%-*.*s", val_width, val_width,
                      state->filters_edit_buffer);
            wattroff(state->main_win, COLOR_PAIR(COLOR_EDIT));
          } else {
            wattron(state->main_win, A_REVERSE);
            if (show_placeholder) {
              wattron(state->main_win, A_DIM);
              mvwprintw(state->main_win, y, val_x, "%-*.*s", val_width,
                        val_width, "WHERE ...");
              wattroff(state->main_win, A_DIM);
            } else {
              mvwprintw(state->main_win, y, val_x, "%-*.*s", val_width,
                        val_width, display_val);
            }
            wattroff(state->main_win, A_REVERSE);
          }
        } else {
          if (show_placeholder) {
            wattron(state->main_win, A_DIM);
            mvwprintw(state->main_win, y, val_x, "%-*.*s", val_width, val_width,
                      "WHERE ...");
            wattroff(state->main_win, A_DIM);
          } else {
            mvwprintw(state->main_win, y, val_x, "%-*.*s", val_width, val_width,
                      display_val);
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
  /* Get schema via VmTable if available, fallback to state->schema */
  VmTable *vm = get_vm_table(state);
  const TableSchema *schema = vm ? vm_table_schema(vm) : state->schema;
  if (!state || !schema || schema->num_columns == 0)
    return -1;

  size_t num_cols = schema->num_columns;
  size_t total_items = num_cols + 1; /* +1 for RAW */

  /* Calculate dropdown dimensions */
  int max_width = 5; /* "(RAW)" length */
  for (size_t i = 0; i < num_cols; i++) {
    int len = (int)strlen(schema->columns[i].name);
    if (len > max_width)
      max_width = len;
  }

  int width = max_width + 6; /* padding + marker */
  if (width < 20)
    width = 20;
  if (width > 40)
    width = 40;

  int height = (int)total_items + 2;
  if (height > 15)
    height = 15;

  /* Position dropdown below the filter row being edited */
  int main_y, main_x;
  getbegyx(state->main_win, main_y, main_x);
  int start_y = main_y + (int)filter_row + 2; /* +2 for panel header */
  int start_x = main_x + 2;

  /* Ensure dropdown fits on screen */
  int max_y, max_x;
  getmaxyx(stdscr, max_y, max_x);
  if (start_y + height > max_y)
    start_y = max_y - height;
  if (start_y < 0)
    start_y = 0;
  if (start_x + width > max_x)
    start_x = max_x - width;
  (void)max_x; /* unused after bounds check */

  /* Create window */
  WINDOW *menu_win = newwin(height, width, start_y, start_x);
  if (!menu_win)
    return -1;

  keypad(menu_win, TRUE);
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
  wattron(menu_win, COLOR_PAIR(COLOR_BORDER));
  box(menu_win, 0, 0);
  wattroff(menu_win, COLOR_PAIR(COLOR_BORDER));

  wattron(menu_win, A_BOLD);
  mvwprintw(menu_win, 0, 2, " Column ");
  wattroff(menu_win, A_BOLD);

  /* Create menu items: columns + RAW */
  ITEM **items = calloc(total_items + 1, sizeof(ITEM *));
  if (!items) {
    delwin(menu_win);
    return -1;
  }

  for (size_t i = 0; i < num_cols; i++) {
    items[i] = new_item(schema->columns[i].name, "");
  }
  items[num_cols] = new_item("(RAW)", ""); /* RAW as last item */
  items[total_items] = NULL;

  MENU *menu = new_menu(items);
  if (!menu) {
    for (size_t i = 0; i < total_items; i++)
      free_item(items[i]);
    free(items);
    delwin(menu_win);
    return -1;
  }

  set_menu_win(menu, menu_win);
  WINDOW *menu_sub = derwin(menu_win, height - 2, width - 2, 1, 1);
  set_menu_sub(menu, menu_sub);
  set_menu_mark(menu, "> ");
  set_menu_format(menu, height - 2, 1);

  /* Set current item */
  if (current_col == FILTER_COL_RAW)
    set_current_item(menu, items[num_cols]);
  else if (current_col < num_cols)
    set_current_item(menu, items[current_col]);

  post_menu(menu);
  wrefresh(menu_win);

  ssize_t result = -1;
  bool running = true;
  while (running) {
    int ch = wgetch(menu_win);
    switch (ch) {
    case KEY_MOUSE: {
      MEVENT mevent;
      if (getmouse(&mevent) == OK) {
        /* Check if click is within menu window */
        if (wenclose(menu_win, mevent.y, mevent.x)) {
          /* Convert to window-relative coordinates */
          int rel_y = mevent.y - start_y;
          int rel_x = mevent.x - start_x;
          (void)rel_x;

          /* Handle scroll wheel */
          if (mevent.bstate & BUTTON4_PRESSED) {
            menu_driver(menu, REQ_UP_ITEM);
          } else if (mevent.bstate & BUTTON5_PRESSED) {
            menu_driver(menu, REQ_DOWN_ITEM);
          }
          /* Handle click - select item at clicked row */
          else if (mevent.bstate & BUTTON1_CLICKED ||
                   mevent.bstate & BUTTON1_DOUBLE_CLICKED) {
            int menu_row = rel_y - 1; /* -1 for border */
            if (menu_row >= 0 && menu_row < height - 2) {
              /* Get top row index */
              int top_idx = top_row(menu);
              int target_idx = top_idx + menu_row;
              if (target_idx >= 0 && target_idx < (int)total_items) {
                set_current_item(menu, items[target_idx]);
                wrefresh(menu_win);
                /* Click selects and confirms - return index directly
                 * (num_cols = RAW, handled by caller) */
                result = target_idx;
                running = false;
              }
            }
          }
        } else {
          /* Click outside menu - close */
          running = false;
        }
      }
      break;
    }
    case KEY_DOWN:
    case 'j':
      menu_driver(menu, REQ_DOWN_ITEM);
      break;
    case KEY_UP:
    case 'k':
      menu_driver(menu, REQ_UP_ITEM);
      break;
    case KEY_NPAGE:
      menu_driver(menu, REQ_SCR_DPAGE);
      break;
    case KEY_PPAGE:
      menu_driver(menu, REQ_SCR_UPAGE);
      break;
    case '\n':
    case KEY_ENTER: {
      ITEM *cur = current_item(menu);
      if (cur) {
        /* Return index directly (num_cols = RAW, handled by caller) */
        result = item_index(cur);
      }
      running = false;
      break;
    }
    case 27: /* Escape */
    case 'q':
      running = false;
      break;
    }
    wrefresh(menu_win);
  }

  /* Cleanup */
  unpost_menu(menu);
  free_menu(menu);
  for (size_t i = 0; i < total_items; i++)
    free_item(items[i]);
  free(items);
  delwin(menu_win);

  touchwin(stdscr);
  tui_refresh(state);

  return result;
}

/* Number of operators to show (excludes RAW which is now a virtual column) */
#define FILTER_OP_VISIBLE (FILTER_OP_COUNT - 1)

/* Show operator dropdown and return selected index, or -1 if cancelled */
static int show_operator_dropdown(TuiState *state, FilterOperator current_op,
                                  size_t filter_row) {
  if (!state)
    return -1;

  /* Calculate dropdown dimensions */
  int max_width = 0;
  for (int i = 0; i < FILTER_OP_VISIBLE; i++) {
    int len = (int)strlen(filter_op_name((FilterOperator)i));
    if (len > max_width)
      max_width = len;
  }

  int width = max_width + 6;
  if (width < 18)
    width = 18;

  int height = FILTER_OP_VISIBLE + 2;
  if (height > 16)
    height = 16;

  /* Position dropdown below the filter row being edited */
  int main_y, main_x;
  getbegyx(state->main_win, main_y, main_x);
  int start_y = main_y + (int)filter_row + 2;
  int start_x = main_x + 18; /* Operator column offset */

  /* Ensure dropdown fits on screen */
  int max_y, max_x;
  getmaxyx(stdscr, max_y, max_x);
  if (start_y + height > max_y)
    start_y = max_y - height;
  if (start_y < 0)
    start_y = 0;
  if (start_x + width > max_x)
    start_x = max_x - width;
  (void)max_x;

  /* Create window */
  WINDOW *menu_win = newwin(height, width, start_y, start_x);
  if (!menu_win)
    return -1;

  keypad(menu_win, TRUE);
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
  wattron(menu_win, COLOR_PAIR(COLOR_BORDER));
  box(menu_win, 0, 0);
  wattroff(menu_win, COLOR_PAIR(COLOR_BORDER));

  wattron(menu_win, A_BOLD);
  mvwprintw(menu_win, 0, 2, " Operator ");
  wattroff(menu_win, A_BOLD);

  /* Create menu items (exclude RAW) */
  ITEM **items = calloc(FILTER_OP_VISIBLE + 1, sizeof(ITEM *));
  if (!items) {
    delwin(menu_win);
    return -1;
  }

  for (int i = 0; i < FILTER_OP_VISIBLE; i++) {
    items[i] = new_item(filter_op_name((FilterOperator)i), "");
  }
  items[FILTER_OP_VISIBLE] = NULL;

  MENU *menu = new_menu(items);
  if (!menu) {
    for (int i = 0; i < FILTER_OP_VISIBLE; i++)
      free_item(items[i]);
    free(items);
    delwin(menu_win);
    return -1;
  }

  set_menu_win(menu, menu_win);
  set_menu_sub(menu, derwin(menu_win, height - 2, width - 2, 1, 1));
  set_menu_mark(menu, "> ");
  set_menu_format(menu, height - 2, 1);

  if ((int)current_op < FILTER_OP_VISIBLE)
    set_current_item(menu, items[(int)current_op]);

  post_menu(menu);
  wrefresh(menu_win);

  int result = -1;
  bool running = true;
  while (running) {
    int ch = wgetch(menu_win);
    switch (ch) {
    case KEY_MOUSE: {
      MEVENT mevent;
      if (getmouse(&mevent) == OK) {
        /* Check if click is within menu window */
        if (wenclose(menu_win, mevent.y, mevent.x)) {
          /* Convert to window-relative coordinates */
          int rel_y = mevent.y - start_y;
          int rel_x = mevent.x - start_x;
          (void)rel_x;

          /* Handle scroll wheel */
          if (mevent.bstate & BUTTON4_PRESSED) {
            menu_driver(menu, REQ_UP_ITEM);
          } else if (mevent.bstate & BUTTON5_PRESSED) {
            menu_driver(menu, REQ_DOWN_ITEM);
          }
          /* Handle click - select item at clicked row */
          else if (mevent.bstate & BUTTON1_CLICKED ||
                   mevent.bstate & BUTTON1_DOUBLE_CLICKED) {
            int menu_row = rel_y - 1; /* -1 for border */
            if (menu_row >= 0 && menu_row < height - 2) {
              /* Get top row index */
              int top_idx = top_row(menu);
              int target_idx = top_idx + menu_row;
              if (target_idx >= 0 && target_idx < FILTER_OP_VISIBLE) {
                set_current_item(menu, items[target_idx]);
                wrefresh(menu_win);
                /* Click selects and confirms */
                result = target_idx;
                running = false;
              }
            }
          }
        } else {
          /* Click outside menu - close */
          running = false;
        }
      }
      break;
    }
    case KEY_DOWN:
    case 'j':
      menu_driver(menu, REQ_DOWN_ITEM);
      break;
    case KEY_UP:
    case 'k':
      menu_driver(menu, REQ_UP_ITEM);
      break;
    case KEY_NPAGE:
      menu_driver(menu, REQ_SCR_DPAGE);
      break;
    case KEY_PPAGE:
      menu_driver(menu, REQ_SCR_UPAGE);
      break;
    case '\n':
    case KEY_ENTER: {
      ITEM *cur = current_item(menu);
      if (cur)
        result = item_index(cur);
      running = false;
      break;
    }
    case 27: /* Escape */
    case 'q':
      running = false;
      break;
    }
    wrefresh(menu_win);
  }

  /* Cleanup */
  unpost_menu(menu);
  free_menu(menu);
  for (int i = 0; i < FILTER_OP_VISIBLE; i++)
    free_item(items[i]);
  free(items);
  delwin(menu_win);

  touchwin(stdscr);
  tui_refresh(state);

  return result;
}

/* Handle filters panel input */
bool tui_handle_filters_input(TuiState *state, const UiEvent *event) {
  if (!state || !state->filters_visible || !state->filters_focused)
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

  int key_char = render_event_get_char(event);

  /* Ctrl+W - switch focus to table */
  if (render_event_is_ctrl(event, 'W')) {
    state->filters_focused = false;
    return true;
  }

  /* Handle editing mode */
  if (state->filters_editing) {
    if (render_event_is_special(event, UI_KEY_ESCAPE)) {
      /* Escape - cancel edit */
      state->filters_editing = false;
    } else if (render_event_is_special(event, UI_KEY_ENTER)) {
      /* Confirm edit and auto-apply */
      if (state->filters_cursor_row < f->num_filters) {
        size_t filter_idx = state->filters_cursor_row;
        ColumnFilter *cf = &f->filters[filter_idx];
        if (state->filters_cursor_col == 2) {
          strncpy(cf->value, state->filters_edit_buffer, sizeof(cf->value) - 1);
          cf->value[sizeof(cf->value) - 1] = '\0';
        } else if (state->filters_cursor_col == 3 &&
                   cf->op == FILTER_OP_BETWEEN) {
          strncpy(cf->value2, state->filters_edit_buffer,
                  sizeof(cf->value2) - 1);
          cf->value2[sizeof(cf->value2) - 1] = '\0';
        }
      }
      state->filters_editing = false;
      tui_apply_filters(state); /* Auto-apply */
    } else if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
      if (state->filters_edit_len > 0) {
        state->filters_edit_buffer[--state->filters_edit_len] = '\0';
      }
    } else if (render_event_is_char(event) && key_char >= 32 &&
               key_char < 127) {
      /* Printable character */
      if (state->filters_edit_len < sizeof(state->filters_edit_buffer) - 1) {
        state->filters_edit_buffer[state->filters_edit_len++] = (char)key_char;
        state->filters_edit_buffer[state->filters_edit_len] = '\0';
      }
    }
    /* Sync focus state to Tab so it persists across tab switches */
    tab_sync_focus(state);
    return true;
  }

  /* Navigation mode */
  const Config *cfg = state->app ? state->app->config : NULL;

  /* Escape, f, / - close panel (toggle filters or escape to close) */
  if (render_event_is_special(event, UI_KEY_ESCAPE) ||
      hotkey_matches(cfg, event, HOTKEY_TOGGLE_FILTERS)) {
    /* Save cursor position to UITabState before closing */
    UITabState *ui = TUI_TAB_UI(state);
    if (ui) {
      ui->filters_cursor_row = state->filters_cursor_row;
      ui->filters_cursor_col = state->filters_cursor_col;
    }
    state->filters_visible = false;
    state->filters_focused = false;
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
      state->filters_focused = false;
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
    } else if (state->sidebar_visible) {
      /* At leftmost column - move focus to sidebar */
      state->filters_was_focused = true; /* Remember filters were focused */
      state->sidebar_focused = true;
      state->filters_focused = false;
      /* Restore last sidebar position */
      state->sidebar_highlight = state->sidebar_last_position;
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
  /* Tab - move to next field/row */
  else if (render_event_is_special(event, UI_KEY_TAB)) {
    size_t idx = state->filters_cursor_row;
    ColumnFilter *cf = &f->filters[idx];
    bool is_between = (cf->op == FILTER_OP_BETWEEN);
    size_t max_col = is_between ? 4 : 3;
    state->filters_cursor_col++;
    if (state->filters_cursor_col > max_col) {
      state->filters_cursor_col = 0;
      if (state->filters_cursor_row < f->num_filters - 1) {
        state->filters_cursor_row++;
      } else {
        state->filters_cursor_row = 0; /* Wrap to first */
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
        size_t num_cols = state->schema ? state->schema->num_columns : 0;
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
        strncpy(state->filters_edit_buffer, cf->value,
                sizeof(state->filters_edit_buffer) - 1);
        state->filters_edit_buffer[sizeof(state->filters_edit_buffer) - 1] =
            '\0';
        state->filters_edit_len = strlen(state->filters_edit_buffer);
      }
    } else if (state->filters_cursor_col == 3 && is_between) {
      /* Value2 - edit (only for BETWEEN) */
      state->filters_editing = true;
      strncpy(state->filters_edit_buffer, cf->value2,
              sizeof(state->filters_edit_buffer) - 1);
      state->filters_edit_buffer[sizeof(state->filters_edit_buffer) - 1] = '\0';
      state->filters_edit_len = strlen(state->filters_edit_buffer);
    } else if (state->filters_cursor_col == del_col) {
      /* Delete */
      bool had_effect = cf->value[0] != '\0' || !filter_op_needs_value(cf->op);
      if (is_between) {
        had_effect = had_effect || cf->value2[0] != '\0';
      }
      if (f->num_filters > 1) {
        filters_remove(f, filter_idx);
        if (state->filters_cursor_row >= f->num_filters)
          state->filters_cursor_row = f->num_filters - 1;
      } else {
        /* Last filter - remove it and close panel */
        filters_remove(f, filter_idx);
        state->filters_visible = false;
        state->filters_focused = false;
        UITabState *ui = TUI_TAB_UI(state);
        if (ui) {
          ui->filters_visible = false;
          ui->filters_focused = false;
        }
      }
      /* Only apply if deleted filter had effect */
      if (had_effect) {
        tui_apply_filters(state);
      }
    }
  }
  /* + or = - add new filter */
  else if (hotkey_matches(cfg, event, HOTKEY_ADD_FILTER)) {
    if (state->schema && state->schema->num_columns > 0) {
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
  /* c/C - clear all filters */
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
      state->filters_visible = false;
      state->filters_focused = false;
      UITabState *ui = TUI_TAB_UI(state);
      if (ui) {
        ui->filters_visible = false;
        ui->filters_focused = false;
      }
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
    if (key_char >= 32 && key_char < 127) {
      size_t filter_idx = state->filters_cursor_row;
      ColumnFilter *cf = &f->filters[filter_idx];
      bool is_raw = (cf->column_index == FILTER_COL_RAW);

      if (is_raw || filter_op_needs_value(cf->op)) {
        /* Start editing with the typed character */
        state->filters_editing = true;
        state->filters_edit_buffer[0] = (char)key_char;
        state->filters_edit_buffer[1] = '\0';
        state->filters_edit_len = 1;
      }
    }
  }
  /* Consume all other keys when filters focused */

  /* Sync focus state to Tab so it persists across tab switches */
  tab_sync_focus(state);
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
    tui_set_status(state, "%zu rows (%zu filter%s applied)", state->total_rows,
                   active_count, active_count == 1 ? "" : "s");
  } else {
    tui_set_status(state, "%zu rows", state->total_rows);
  }
}

/* Handle mouse click in filters panel
 * rel_x, rel_y are coordinates relative to main_win (0,0)
 * Returns true if click was handled */
bool tui_handle_filters_click(TuiState *state, int rel_x, int rel_y) {
  if (!state || !state->filters_visible)
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

  /* Determine which field was clicked */
  size_t target_col;
  if (rel_x >= del_x && rel_x < del_x + 3) {
    target_col = 3; /* Delete button */
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
  state->filters_focused = true;
  state->sidebar_focused = false;

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
      size_t num_cols = state->schema ? state->schema->num_columns : 0;
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
    strncpy(state->filters_edit_buffer, cf->value,
            sizeof(state->filters_edit_buffer) - 1);
    state->filters_edit_buffer[sizeof(state->filters_edit_buffer) - 1] = '\0';
    state->filters_edit_len = strlen(state->filters_edit_buffer);
  } else if (target_col == 3 && is_between) {
    /* Value2 field for BETWEEN - start editing */
    state->filters_editing = true;
    strncpy(state->filters_edit_buffer, cf->value2,
            sizeof(state->filters_edit_buffer) - 1);
    state->filters_edit_buffer[sizeof(state->filters_edit_buffer) - 1] = '\0';
    state->filters_edit_len = strlen(state->filters_edit_buffer);
  } else if (target_col == del_col) {
    /* Delete button - delete filter */
    bool had_effect = cf->value[0] != '\0' || !filter_op_needs_value(cf->op);
    if (is_between) {
      had_effect = had_effect || cf->value2[0] != '\0';
    }
    if (f->num_filters > 1) {
      filters_remove(f, target_filter);
      if (state->filters_cursor_row >= f->num_filters)
        state->filters_cursor_row = f->num_filters - 1;
    } else {
      /* Last filter - remove it and close panel */
      filters_remove(f, target_filter);
      state->filters_visible = false;
      state->filters_focused = false;
      UITabState *ui = TUI_TAB_UI(state);
      if (ui) {
        ui->filters_visible = false;
        ui->filters_focused = false;
      }
    }
    if (had_effect) {
      tui_apply_filters(state);
    }
  }

  return true;
}
