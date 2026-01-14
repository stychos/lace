/*
 * Lace ncurses frontend
 * Filter panel UI
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "filters.h"
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Operator display strings */
static const char *op_strings[] = {
  "=", "!=", "<", "<=", ">", ">=", "LIKE", "IS NULL", "IS NOT NULL"
};
#define NUM_OPS (sizeof(op_strings) / sizeof(op_strings[0]))

/* ==========================================================================
 * Filter Panel Functions
 * ========================================================================== */

void filters_toggle(TuiState *tui, FilterPanelState *fp) {
  fp->visible = !fp->visible;
  if (fp->visible) {
    fp->focused = true;
  }
  tui->app->needs_redraw = true;
}

int filters_get_height(TuiState *tui, FilterPanelState *fp) {
  if (!fp->visible) return 0;

  Tab *tab = app_current_tab(tui->app);
  if (!tab) return 1;

  int num_filters = (int)tab->num_filters;
  if (num_filters < 1) num_filters = 1;
  if (num_filters > MAX_VISIBLE_FILTERS) num_filters = MAX_VISIBLE_FILTERS;

  return 1 + num_filters; /* title + filter rows */
}

void filters_draw(TuiState *tui, FilterPanelState *fp, WINDOW *win, int y) {
  if (!fp->visible || !win) return;

  Tab *tab = app_current_tab(tui->app);
  if (!tab) return;

  int win_cols;
  int win_rows;
  getmaxyx(win, win_rows, win_cols);
  (void)win_rows;

  /* Title bar */
  wattron(win, A_BOLD);
  if (tab->num_filters > 0) {
    mvwprintw(win, y, 1, "Filters (%zu) [+:add -:del c:clear Tab:switch Esc:close]",
              tab->num_filters);
  } else {
    mvwprintw(win, y, 1, "Filters [+:add -:del c:clear Tab:switch Esc:close]");
  }
  wattroff(win, A_BOLD);

  /* Column positions */
  int col_x = 1;
  int op_x = 20;
  int val_x = 35;
  int del_x = win_cols - 5;

  y++;

  /* Ensure at least one filter row for adding */
  size_t num_filters = tab->num_filters;
  if (num_filters == 0) {
    /* Show empty row for adding first filter */
    if (fp->focused && fp->cursor_row == 0) {
      wattron(win, COLOR_PAIR(COLOR_SELECTED));
    }
    mvwprintw(win, y, col_x, "(add filter with +)");
    if (fp->focused && fp->cursor_row == 0) {
      wattroff(win, COLOR_PAIR(COLOR_SELECTED));
    }
    return;
  }

  /* Draw filter rows */
  size_t visible_start = fp->scroll;
  size_t visible_count = num_filters - visible_start;
  if (visible_count > MAX_VISIBLE_FILTERS) visible_count = MAX_VISIBLE_FILTERS;

  for (size_t i = 0; i < visible_count; i++) {
    size_t idx = visible_start + i;
    LaceFilter *filter = &tab->filters[idx];
    bool selected = fp->focused && (fp->cursor_row == idx);

    /* Get column name */
    const char *col_name = "?";
    if (tab->schema && filter->column < tab->schema->num_columns) {
      col_name = tab->schema->columns[filter->column].name;
    }

    /* Get operator string */
    const char *op_str = "?";
    if (filter->op < LACE_FILTER_COUNT) {
      op_str = op_strings[filter->op];
    }

    /* Draw column name */
    if (selected && fp->cursor_field == 0) {
      wattron(win, COLOR_PAIR(COLOR_SELECTED));
    }
    mvwprintw(win, y + (int)i, col_x, "%-18.18s", col_name);
    if (selected && fp->cursor_field == 0) {
      wattroff(win, COLOR_PAIR(COLOR_SELECTED));
    }

    /* Draw operator */
    if (selected && fp->cursor_field == 1) {
      wattron(win, COLOR_PAIR(COLOR_SELECTED));
    }
    mvwprintw(win, y + (int)i, op_x, "%-14s", op_str);
    if (selected && fp->cursor_field == 1) {
      wattroff(win, COLOR_PAIR(COLOR_SELECTED));
    }

    /* Draw value */
    if (selected && fp->cursor_field == 2) {
      if (fp->editing) {
        wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_UNDERLINE);
        mvwprintw(win, y + (int)i, val_x, "%-*.*s",
                  del_x - val_x - 1, del_x - val_x - 1, fp->edit_buffer);
        wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_UNDERLINE);
      } else {
        wattron(win, COLOR_PAIR(COLOR_SELECTED));
        mvwprintw(win, y + (int)i, val_x, "%-*.*s",
                  del_x - val_x - 1, del_x - val_x - 1,
                  filter->value ? filter->value : "");
        wattroff(win, COLOR_PAIR(COLOR_SELECTED));
      }
    } else {
      mvwprintw(win, y + (int)i, val_x, "%-*.*s",
                del_x - val_x - 1, del_x - val_x - 1,
                filter->value ? filter->value : "");
    }

    /* Draw delete button */
    if (selected) {
      wattron(win, COLOR_PAIR(COLOR_ERROR));
    }
    mvwprintw(win, y + (int)i, del_x, "[x]");
    if (selected) {
      wattroff(win, COLOR_PAIR(COLOR_ERROR));
    }
  }
}

bool filters_handle_input(TuiState *tui, FilterPanelState *fp, int ch) {
  if (!fp->visible) return false;

  Tab *tab = app_current_tab(tui->app);
  if (!tab) return false;

  /* Handle editing mode */
  if (fp->editing) {
    switch (ch) {
    case '\n':
    case KEY_ENTER:
      /* Confirm edit */
      if (fp->cursor_row < tab->num_filters) {
        free(tab->filters[fp->cursor_row].value);
        tab->filters[fp->cursor_row].value = strdup(fp->edit_buffer);
        filters_apply(tui);
      }
      fp->editing = false;
      tui->app->needs_redraw = true;
      return true;

    case 27: /* Escape */
      fp->editing = false;
      tui->app->needs_redraw = true;
      return true;

    case KEY_BACKSPACE:
    case 127:
    case 8:
      if (fp->edit_pos > 0) {
        memmove(fp->edit_buffer + fp->edit_pos - 1,
                fp->edit_buffer + fp->edit_pos,
                strlen(fp->edit_buffer + fp->edit_pos) + 1);
        fp->edit_pos--;
        tui->app->needs_redraw = true;
      }
      return true;

    default:
      if (ch >= 32 && ch < 127 && fp->edit_pos < sizeof(fp->edit_buffer) - 1) {
        size_t len = strlen(fp->edit_buffer);
        memmove(fp->edit_buffer + fp->edit_pos + 1,
                fp->edit_buffer + fp->edit_pos,
                len - fp->edit_pos + 1);
        fp->edit_buffer[fp->edit_pos++] = (char)ch;
        tui->app->needs_redraw = true;
        return true;
      }
      break;
    }
    return false;
  }

  /* Normal mode */
  switch (ch) {
  case 'j':
  case KEY_DOWN:
    if (tab->num_filters > 0 && fp->cursor_row < tab->num_filters - 1) {
      fp->cursor_row++;
      /* Adjust scroll */
      if (fp->cursor_row >= fp->scroll + MAX_VISIBLE_FILTERS) {
        fp->scroll = fp->cursor_row - MAX_VISIBLE_FILTERS + 1;
      }
      tui->app->needs_redraw = true;
    }
    return true;

  case 'k':
  case KEY_UP:
    if (fp->cursor_row > 0) {
      fp->cursor_row--;
      if (fp->cursor_row < fp->scroll) {
        fp->scroll = fp->cursor_row;
      }
      tui->app->needs_redraw = true;
    }
    return true;

  case 'h':
  case KEY_LEFT:
    if (fp->cursor_field > 0) {
      fp->cursor_field--;
      tui->app->needs_redraw = true;
    }
    return true;

  case 'l':
  case KEY_RIGHT:
    if (fp->cursor_field < 2) {
      fp->cursor_field++;
      tui->app->needs_redraw = true;
    }
    return true;

  case '\n':
  case KEY_ENTER:
    if (tab->num_filters > 0 && fp->cursor_row < tab->num_filters) {
      if (fp->cursor_field == 0) {
        /* Cycle column */
        if (tab->schema && tab->schema->num_columns > 0) {
          tab->filters[fp->cursor_row].column =
            (tab->filters[fp->cursor_row].column + 1) % tab->schema->num_columns;
          filters_apply(tui);
        }
      } else if (fp->cursor_field == 1) {
        /* Cycle operator */
        tab->filters[fp->cursor_row].op =
          (tab->filters[fp->cursor_row].op + 1) % LACE_FILTER_COUNT;
        filters_apply(tui);
      } else {
        /* Start editing value */
        fp->editing = true;
        const char *val = tab->filters[fp->cursor_row].value;
        strncpy(fp->edit_buffer, val ? val : "", sizeof(fp->edit_buffer) - 1);
        fp->edit_pos = strlen(fp->edit_buffer);
      }
      tui->app->needs_redraw = true;
    }
    return true;

  case '+':
  case '=':
    filters_add(tui, fp);
    return true;

  case '-':
    filters_remove(tui, fp);
    return true;

  case 'c':
    filters_clear(tui, fp);
    return true;

  case '\t':
  case 23: /* Ctrl+W */
    /* Switch focus to table */
    fp->focused = false;
    tui->app->needs_redraw = true;
    return true;

  case 27: /* Escape */
    fp->visible = false;
    fp->focused = false;
    tui->app->needs_redraw = true;
    return true;
  }

  return false;
}

void filters_add(TuiState *tui, FilterPanelState *fp) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->schema) return;

  /* Grow filters array */
  LaceFilter *new_filters = realloc(tab->filters,
                                    (tab->num_filters + 1) * sizeof(LaceFilter));
  if (!new_filters) return;

  tab->filters = new_filters;

  /* Initialize new filter */
  LaceFilter *f = &tab->filters[tab->num_filters];
  f->column = 0;
  f->op = LACE_FILTER_EQ;
  f->value = strdup("");

  tab->num_filters++;
  fp->cursor_row = tab->num_filters - 1;
  fp->cursor_field = 2; /* Focus on value field */

  tui->app->needs_redraw = true;
}

void filters_remove(TuiState *tui, FilterPanelState *fp) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || tab->num_filters == 0) return;

  if (fp->cursor_row >= tab->num_filters) {
    fp->cursor_row = tab->num_filters - 1;
  }

  /* Free value string */
  free(tab->filters[fp->cursor_row].value);

  /* Remove from array */
  for (size_t i = fp->cursor_row; i < tab->num_filters - 1; i++) {
    tab->filters[i] = tab->filters[i + 1];
  }
  tab->num_filters--;

  /* Adjust cursor */
  if (fp->cursor_row > 0 && fp->cursor_row >= tab->num_filters) {
    fp->cursor_row--;
  }

  filters_apply(tui);
  tui->app->needs_redraw = true;
}

void filters_clear(TuiState *tui, FilterPanelState *fp) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab) return;

  /* Free all filter values */
  for (size_t i = 0; i < tab->num_filters; i++) {
    free(tab->filters[i].value);
  }
  free(tab->filters);
  tab->filters = NULL;
  tab->num_filters = 0;

  fp->cursor_row = 0;
  fp->scroll = 0;

  filters_apply(tui);
  tui->app->needs_redraw = true;
}

void filters_apply(TuiState *tui) {
  /* Reload data with new filters */
  Tab *tab = app_current_tab(tui->app);
  if (!tab) return;

  tab->data_offset = 0;
  tab->cursor_row = 0;
  tab->scroll_row = 0;

  app_refresh_data(tui->app);
}
