/*
 * lace - Database Viewer and Manager
 * Table filters implementation
 */

#include "tui_internal.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Maximum number of visible filter rows in panel */
#define MAX_VISIBLE_FILTERS 8

/* Sentinel value for RAW filter (virtual column) */
#define FILTER_COL_RAW SIZE_MAX

/* Operator names for display */
static const char *FILTER_OP_NAMES[] = {
    "=",          "<>",           ">",      ">=",           "<",
    "<=",         "in",           "contains", "regex",      "is empty",
    "is not empty", "is null",    "is not null", "RAW"};

/* Initialize filters structure */
void filters_init(TableFilters *f) {
  if (!f)
    return;
  memset(f, 0, sizeof(TableFilters));
}

/* Free filters structure */
void filters_free(TableFilters *f) {
  if (!f)
    return;
  free(f->filters);
  f->filters = NULL;
  f->num_filters = 0;
  f->filters_cap = 0;
}

/* Clear all filters */
void filters_clear(TableFilters *f) {
  if (!f)
    return;
  f->num_filters = 0;
}

/* Add a filter */
bool filters_add(TableFilters *f, size_t col_idx, FilterOperator op,
                 const char *value) {
  if (!f)
    return false;

  /* Grow array if needed */
  if (f->num_filters >= f->filters_cap) {
    size_t new_cap = f->filters_cap == 0 ? 4 : f->filters_cap * 2;
    ColumnFilter *new_filters =
        realloc(f->filters, new_cap * sizeof(ColumnFilter));
    if (!new_filters)
      return false;
    f->filters = new_filters;
    f->filters_cap = new_cap;
  }

  ColumnFilter *cf = &f->filters[f->num_filters];
  cf->column_index = col_idx;
  cf->op = op;
  if (value) {
    strncpy(cf->value, value, sizeof(cf->value) - 1);
    cf->value[sizeof(cf->value) - 1] = '\0';
  } else {
    cf->value[0] = '\0';
  }

  f->num_filters++;
  return true;
}

/* Remove a filter by index */
void filters_remove(TableFilters *f, size_t index) {
  if (!f || index >= f->num_filters)
    return;

  /* Shift remaining filters down */
  for (size_t i = index; i < f->num_filters - 1; i++) {
    f->filters[i] = f->filters[i + 1];
  }
  f->num_filters--;
}

/* Get operator display name */
const char *filter_op_name(FilterOperator op) {
  if (op < 0 || op >= FILTER_OP_COUNT)
    return "?";
  return FILTER_OP_NAMES[op];
}

/* Get operator SQL symbol */
const char *filter_op_sql(FilterOperator op) {
  switch (op) {
  case FILTER_OP_EQ:
    return "=";
  case FILTER_OP_NE:
    return "<>";
  case FILTER_OP_GT:
    return ">";
  case FILTER_OP_GE:
    return ">=";
  case FILTER_OP_LT:
    return "<";
  case FILTER_OP_LE:
    return "<=";
  default:
    return "=";
  }
}

/* Check if operator needs a value */
bool filter_op_needs_value(FilterOperator op) {
  switch (op) {
  case FILTER_OP_IS_EMPTY:
  case FILTER_OP_IS_NOT_EMPTY:
  case FILTER_OP_IS_NULL:
  case FILTER_OP_IS_NOT_NULL:
    return false;
  default:
    return true;
  }
}

/* Parse IN values with smart quoting */
char *filters_parse_in_values(const char *input, char **err) {
  if (!input || !*input) {
    if (err)
      *err = str_dup("Empty value list");
    return NULL;
  }

  StringBuilder *sb = sb_new(strlen(input) * 2);
  if (!sb) {
    if (err)
      *err = str_dup("Out of memory");
    return NULL;
  }

  const char *p = input;

  /* Skip leading whitespace and optional ( */
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p == '(')
    p++;

  bool first = true;
  while (*p) {
    /* Skip whitespace */
    while (*p && isspace((unsigned char)*p))
      p++;
    if (!*p || *p == ')')
      break;

    if (!first)
      sb_append(sb, ", ");
    first = false;

    if (*p == '\'' || *p == '"') {
      /* Quoted value - find closing quote */
      char quote = *p;
      p++;
      const char *start = p;
      while (*p && *p != quote) {
        if (*p == '\\' && *(p + 1))
          p++; /* Skip escaped chars */
        p++;
      }
      sb_append_char(sb, '\'');
      /* Copy and escape single quotes */
      for (const char *c = start; c < p; c++) {
        if (*c == '\'')
          sb_append(sb, "''");
        else
          sb_append_char(sb, *c);
      }
      sb_append_char(sb, '\'');
      if (*p == quote)
        p++;
    } else {
      /* Unquoted value - read until comma or end */
      const char *start = p;
      while (*p && *p != ',' && *p != ')')
        p++;
      /* Trim trailing whitespace */
      const char *end = p;
      while (end > start && isspace((unsigned char)*(end - 1)))
        end--;

      if (end > start) {
        /* Check if numeric */
        bool is_numeric = true;
        for (const char *c = start; c < end; c++) {
          if (!isdigit((unsigned char)*c) && *c != '.' && *c != '-' &&
              *c != '+') {
            is_numeric = false;
            break;
          }
        }

        if (is_numeric) {
          sb_append_len(sb, start, end - start);
        } else {
          sb_append_char(sb, '\'');
          /* Escape single quotes in value */
          for (const char *c = start; c < end; c++) {
            if (*c == '\'')
              sb_append(sb, "''");
            else
              sb_append_char(sb, *c);
          }
          sb_append_char(sb, '\'');
        }
      }
    }

    /* Skip comma */
    while (*p && isspace((unsigned char)*p))
      p++;
    if (*p == ',')
      p++;
  }

  return sb_to_string(sb);
}

/* Escape a value for SQL */
static char *escape_sql_value(const char *value) {
  if (!value)
    return str_dup("");

  size_t len = strlen(value);
  StringBuilder *sb = sb_new(len * 2 + 2);
  if (!sb)
    return NULL;

  for (const char *p = value; *p; p++) {
    if (*p == '\'')
      sb_append(sb, "''");
    else
      sb_append_char(sb, *p);
  }

  return sb_to_string(sb);
}

/* Build WHERE clause from filters */
char *filters_build_where(TableFilters *f, TableSchema *schema,
                          const char *driver_name, char **err) {
  if (!f || !schema)
    return NULL;

  /* No filters? Return NULL (no WHERE clause) */
  if (f->num_filters == 0)
    return NULL;

  StringBuilder *sb = sb_new(256);
  if (!sb) {
    if (err)
      *err = str_dup("Out of memory");
    return NULL;
  }

  bool first = true;
  bool use_backticks = str_eq(driver_name, "mysql") || str_eq(driver_name, "mariadb");

  /* Process each column filter */
  for (size_t i = 0; i < f->num_filters; i++) {
    ColumnFilter *cf = &f->filters[i];

    /* Skip empty filters:
     * - Empty = filters should not be applied
     * - Empty RAW filters should not be applied */
    if (cf->value[0] == '\0') {
      if (cf->column_index == SIZE_MAX || cf->op == FILTER_OP_EQ) {
        continue;
      }
    }

    if (!first)
      sb_append(sb, " AND ");
    first = false;

    /* Handle RAW filters (virtual column) */
    if (cf->column_index == SIZE_MAX) {
      sb_printf(sb, "(%s)", cf->value);
      continue;
    }

    /* Validate column index */
    if (cf->column_index >= schema->num_columns)
      continue;

    const char *col_name = schema->columns[cf->column_index].name;

    /* Escape column name */
    char *escaped_col;
    if (use_backticks) {
      escaped_col = str_escape_identifier_backtick(col_name);
    } else {
      escaped_col = str_escape_identifier_dquote(col_name);
    }
    if (!escaped_col)
      continue;

    switch (cf->op) {
    case FILTER_OP_EQ:
    case FILTER_OP_NE:
    case FILTER_OP_GT:
    case FILTER_OP_GE:
    case FILTER_OP_LT:
    case FILTER_OP_LE: {
      char *escaped_val = escape_sql_value(cf->value);
      sb_printf(sb, "%s %s '%s'", escaped_col, filter_op_sql(cf->op),
                escaped_val ? escaped_val : "");
      free(escaped_val);
      break;
    }

    case FILTER_OP_IN: {
      char *in_err = NULL;
      char *in_list = filters_parse_in_values(cf->value, &in_err);
      if (in_list) {
        sb_printf(sb, "%s IN (%s)", escaped_col, in_list);
        free(in_list);
      } else {
        /* Fall back to empty IN */
        sb_printf(sb, "%s IN (NULL)", escaped_col);
        free(in_err);
      }
      break;
    }

    case FILTER_OP_CONTAINS: {
      char *escaped_val = escape_sql_value(cf->value);
      /* Escape LIKE wildcards */
      sb_printf(sb, "%s LIKE '%%%s%%'", escaped_col,
                escaped_val ? escaped_val : "");
      free(escaped_val);
      break;
    }

    case FILTER_OP_REGEX: {
      char *escaped_val = escape_sql_value(cf->value);
      /* Driver-specific regex */
      if (str_eq(driver_name, "mysql") || str_eq(driver_name, "mariadb")) {
        sb_printf(sb, "%s REGEXP '%s'", escaped_col,
                  escaped_val ? escaped_val : "");
      } else if (str_eq(driver_name, "postgres") ||
                 str_eq(driver_name, "postgresql") ||
                 str_eq(driver_name, "pg")) {
        sb_printf(sb, "%s ~ '%s'", escaped_col, escaped_val ? escaped_val : "");
      } else {
        /* SQLite - use GLOB as fallback (not true regex) */
        sb_printf(sb, "%s GLOB '*%s*'", escaped_col,
                  escaped_val ? escaped_val : "");
      }
      free(escaped_val);
      break;
    }

    case FILTER_OP_IS_EMPTY:
      sb_printf(sb, "%s = ''", escaped_col);
      break;

    case FILTER_OP_IS_NOT_EMPTY:
      sb_printf(sb, "%s <> ''", escaped_col);
      break;

    case FILTER_OP_IS_NULL:
      sb_printf(sb, "%s IS NULL", escaped_col);
      break;

    case FILTER_OP_IS_NOT_NULL:
      sb_printf(sb, "%s IS NOT NULL", escaped_col);
      break;

    case FILTER_OP_RAW:
      /* This case shouldn't occur - RAW is now a virtual column */
      sb_printf(sb, "(%s)", cf->value);
      break;
    }

    free(escaped_col);
  }

  /* If all filters were skipped, return NULL */
  if (first) {
    sb_free(sb);
    return NULL;
  }

  return sb_to_string(sb);
}

/* Get panel height based on filter count */
int tui_get_filters_panel_height(TuiState *state) {
  if (!state || !state->filters_visible)
    return 0;

  Workspace *ws = &state->workspaces[state->current_workspace];
  int filter_rows = (int)ws->filters.num_filters;
  if (filter_rows < 1)
    filter_rows = 1; /* Always at least one filter row */
  if (filter_rows > MAX_VISIBLE_FILTERS)
    filter_rows = MAX_VISIBLE_FILTERS;

  return 1 + filter_rows; /* title + filters */
}

/* Draw the filters panel */
void tui_draw_filters_panel(TuiState *state) {
  if (!state || !state->filters_visible || state->num_workspaces == 0)
    return;

  Workspace *ws = &state->workspaces[state->current_workspace];
  TableFilters *f = &ws->filters;

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
  int col_x = start_x + 1;  /* Align with title */
  int op_x = start_x + 17;
  int val_x = start_x + 31;
  int del_x = panel_width - 4;

  /* Title bar with hotkeys (like SQL editor) */
  wattron(state->main_win, A_BOLD);
  if (active_count > 0) {
    mvwprintw(state->main_win, start_y, col_x,
              "Filters (%zu) (+/-:add/del, c:clear, ^W:switch, Esc)", active_count);
  } else {
    mvwprintw(state->main_win, start_y, col_x,
              "Filters (+/-:add/del, c:clear, ^W:switch, Esc)");
  }
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
    bool row_selected = state->filters_focused && (state->filters_cursor_row == filter_idx);
    bool is_raw = (cf->column_index == FILTER_COL_RAW);

    /* Column name */
    const char *col_name = is_raw ? "(RAW)" : "???";
    if (!is_raw && state->schema && cf->column_index < state->schema->num_columns) {
      col_name = state->schema->columns[cf->column_index].name;
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

    /* Value field - always shown for RAW, depends on operator otherwise */
    if (is_raw || filter_op_needs_value(cf->op)) {
      const char *display_val = cf->value;
      bool show_placeholder = is_raw && cf->value[0] == '\0';

      if (row_selected && state->filters_cursor_col == 2) {
        if (state->filters_editing) {
          wattron(state->main_win, COLOR_PAIR(COLOR_EDIT));
          mvwprintw(state->main_win, y, val_x, "%-20.20s",
                    state->filters_edit_buffer);
          wattroff(state->main_win, COLOR_PAIR(COLOR_EDIT));
        } else {
          wattron(state->main_win, A_REVERSE);
          if (show_placeholder) {
            wattron(state->main_win, A_DIM);
            mvwprintw(state->main_win, y, val_x, "%-20.20s", "WHERE ...");
            wattroff(state->main_win, A_DIM);
          } else {
            mvwprintw(state->main_win, y, val_x, "%-20.20s", display_val);
          }
          wattroff(state->main_win, A_REVERSE);
        }
      } else {
        if (show_placeholder) {
          wattron(state->main_win, A_DIM);
          mvwprintw(state->main_win, y, val_x, "%-20.20s", "WHERE ...");
          wattroff(state->main_win, A_DIM);
        } else {
          mvwprintw(state->main_win, y, val_x, "%-20.20s", display_val);
        }
      }
    }

    /* Delete button */
    if (row_selected && state->filters_cursor_col == 3)
      wattron(state->main_win, A_REVERSE);
    mvwprintw(state->main_win, y, del_x, "[x]");
    if (row_selected && state->filters_cursor_col == 3)
      wattroff(state->main_win, A_REVERSE);

    y++;
  }
}

/* Show column dropdown and return selected index, or -1 if cancelled
 * Returns FILTER_COL_RAW (SIZE_MAX) if RAW is selected */
static ssize_t show_column_dropdown(TuiState *state, size_t current_col,
                                    size_t filter_row) {
  if (!state || !state->schema || state->schema->num_columns == 0)
    return -1;

  TableSchema *schema = state->schema;
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
  box(menu_win, 0, 0);

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
  set_menu_sub(menu, derwin(menu_win, height - 2, width - 2, 1, 1));
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
        int idx = item_index(cur);
        if (idx == (int)num_cols)
          result = (ssize_t)FILTER_COL_RAW; /* RAW selected */
        else
          result = idx;
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
  box(menu_win, 0, 0);

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
bool tui_handle_filters_input(TuiState *state, int ch) {
  if (!state || !state->filters_visible || !state->filters_focused ||
      state->num_workspaces == 0)
    return false;

  Workspace *ws = &state->workspaces[state->current_workspace];
  TableFilters *f = &ws->filters;

  /* Ensure at least one filter exists */
  if (f->num_filters == 0) {
    filters_add(f, 0, FILTER_OP_EQ, "");
  }

  /* Ctrl+W - switch focus to table */
  if (ch == 23) {
    state->filters_focused = false;
    return true;
  }

  /* Handle editing mode */
  if (state->filters_editing) {
    switch (ch) {
    case 27: /* Escape - cancel edit */
      state->filters_editing = false;
      break;

    case '\n':
    case KEY_ENTER:
      /* Confirm edit and auto-apply */
      if (state->filters_cursor_row < f->num_filters) {
        size_t filter_idx = state->filters_cursor_row;
        ColumnFilter *cf = &f->filters[filter_idx];
        if (state->filters_cursor_col == 2) {
          strncpy(cf->value, state->filters_edit_buffer,
                  sizeof(cf->value) - 1);
          cf->value[sizeof(cf->value) - 1] = '\0';
        }
      }
      state->filters_editing = false;
      tui_apply_filters(state); /* Auto-apply */
      break;

    case KEY_BACKSPACE:
    case 127:
    case 8:
      if (state->filters_edit_len > 0) {
        state->filters_edit_buffer[--state->filters_edit_len] = '\0';
      }
      break;

    default:
      if (ch >= 32 && ch < 127 &&
          state->filters_edit_len < sizeof(state->filters_edit_buffer) - 1) {
        state->filters_edit_buffer[state->filters_edit_len++] = (char)ch;
        state->filters_edit_buffer[state->filters_edit_len] = '\0';
      }
      break;
    }
    return true;
  }

  /* Navigation mode */
  switch (ch) {
  case 27: /* Escape - close panel */
    state->filters_visible = false;
    break;

  case KEY_UP:
  case 'k':
    if (state->filters_cursor_row > 0) {
      state->filters_cursor_row--;
      /* Adjust scroll if cursor moved above visible area */
      if (state->filters_cursor_row < state->filters_scroll) {
        state->filters_scroll = state->filters_cursor_row;
      }
    }
    break;

  case KEY_DOWN:
  case 'j':
    if (state->filters_cursor_row < f->num_filters - 1) {
      state->filters_cursor_row++;
      /* Adjust scroll if cursor moved below visible area */
      if (state->filters_cursor_row >= state->filters_scroll + MAX_VISIBLE_FILTERS) {
        state->filters_scroll = state->filters_cursor_row - MAX_VISIBLE_FILTERS + 1;
      }
    } else {
      /* At last filter row - move focus to table */
      state->filters_focused = false;
    }
    break;

  case KEY_LEFT:
  case 'h': {
    size_t idx = state->filters_cursor_row;
    bool is_raw = (f->filters[idx].column_index == FILTER_COL_RAW);
    if (state->filters_cursor_col > 0) {
      state->filters_cursor_col--;
      if (is_raw && state->filters_cursor_col == 1)
        state->filters_cursor_col = 0;
    } else if (state->sidebar_visible) {
      /* At leftmost column - move focus to sidebar */
      state->sidebar_focused = true;
      state->filters_focused = false;
    }
    break;
  }

  case KEY_RIGHT:
  case 'l': {
    size_t idx = state->filters_cursor_row;
    bool is_raw = (f->filters[idx].column_index == FILTER_COL_RAW);
    if (state->filters_cursor_col < 3) {
      state->filters_cursor_col++;
      if (is_raw && state->filters_cursor_col == 1)
        state->filters_cursor_col = 2;
    }
    break;
  }

  case '\t':
    /* Tab - move to next field/row */
    state->filters_cursor_col++;
    if (state->filters_cursor_col > 3) {
      state->filters_cursor_col = 0;
      if (state->filters_cursor_row < f->num_filters - 1) {
        state->filters_cursor_row++;
      } else {
        state->filters_cursor_row = 0; /* Wrap to first */
      }
    }
    break;

  case '\n':
  case KEY_ENTER: {
    size_t filter_idx = state->filters_cursor_row;
    ColumnFilter *cf = &f->filters[filter_idx];
    bool is_raw = (cf->column_index == FILTER_COL_RAW);

    switch (state->filters_cursor_col) {
    case 0: /* Column - show dropdown */ {
      ssize_t sel = show_column_dropdown(state, cf->column_index,
                                         state->filters_cursor_row);
      if (sel >= 0 || sel == (ssize_t)FILTER_COL_RAW) {
        cf->column_index = (size_t)sel;
        tui_apply_filters(state); /* Auto-apply */
      }
      break;
    }
    case 1: /* Operator - show dropdown (not for RAW) */
      if (!is_raw) {
        int sel = show_operator_dropdown(state, cf->op,
                                         state->filters_cursor_row);
        if (sel >= 0) {
          cf->op = (FilterOperator)sel;
          tui_apply_filters(state); /* Auto-apply */
        }
      }
      break;
    case 2: /* Value - edit */
      if (is_raw || filter_op_needs_value(cf->op)) {
        state->filters_editing = true;
        strncpy(state->filters_edit_buffer, cf->value,
                sizeof(state->filters_edit_buffer) - 1);
        state->filters_edit_buffer[sizeof(state->filters_edit_buffer) - 1] =
            '\0';
        state->filters_edit_len = strlen(state->filters_edit_buffer);
      }
      break;
    case 3: /* Delete/Reset */
      if (f->num_filters > 1) {
        filters_remove(f, filter_idx);
        if (state->filters_cursor_row >= f->num_filters)
          state->filters_cursor_row = f->num_filters - 1;
      } else {
        cf->column_index = 0;
        cf->op = FILTER_OP_EQ;
        cf->value[0] = '\0';
      }
      tui_apply_filters(state); /* Auto-apply */
      break;
    }
    break;
  }

  case '+':
  case '=':
    /* Add new filter */
    if (state->schema && state->schema->num_columns > 0) {
      filters_add(f, 0, FILTER_OP_EQ, "");
      state->filters_cursor_row = f->num_filters - 1;
      state->filters_cursor_col = 0;
      /* Scroll to show new filter */
      if (state->filters_cursor_row >= state->filters_scroll + MAX_VISIBLE_FILTERS) {
        state->filters_scroll = state->filters_cursor_row - MAX_VISIBLE_FILTERS + 1;
      }
    }
    break;

  case 'c':
  case 'C':
    /* Clear all - reset to single empty filter */
    filters_clear(f);
    filters_add(f, 0, FILTER_OP_EQ, "");
    state->filters_cursor_row = 0;
    state->filters_cursor_col = 0;
    state->filters_scroll = 0;
    tui_apply_filters(state);
    break;

  case '-':
  case 'x':
  case KEY_DC: {
    /* Delete/Reset current filter */
    size_t filter_idx = state->filters_cursor_row;
    if (f->num_filters > 1) {
      filters_remove(f, filter_idx);
      if (state->filters_cursor_row >= f->num_filters)
        state->filters_cursor_row = f->num_filters - 1;
      /* Adjust scroll if needed */
      if (state->filters_scroll > 0 &&
          f->num_filters <= state->filters_scroll + MAX_VISIBLE_FILTERS) {
        state->filters_scroll = f->num_filters > MAX_VISIBLE_FILTERS
                                ? f->num_filters - MAX_VISIBLE_FILTERS : 0;
      }
    } else {
      ColumnFilter *cf = &f->filters[filter_idx];
      cf->column_index = 0;
      cf->op = FILTER_OP_EQ;
      cf->value[0] = '\0';
    }
    tui_apply_filters(state); /* Auto-apply */
    break;
  }

  default:
    /* Pass through to table if not a filter key */
    return false;
  }

  return true;
}

/* Apply current filters and reload data */
void tui_apply_filters(TuiState *state) {
  if (!state || state->num_workspaces == 0)
    return;

  Workspace *ws = &state->workspaces[state->current_workspace];
  if (ws->type != WORKSPACE_TYPE_TABLE || !ws->table_name)
    return;

  /* Reload table data with filters applied */
  tui_load_table_data(state, ws->table_name);

  /* Update status - count only active (non-empty) filters */
  TableFilters *f = &ws->filters;
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
