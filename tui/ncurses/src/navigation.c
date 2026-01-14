/*
 * Lace ncurses frontend
 * Navigation - cursor and page movement
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "navigation.h"
#include "app.h"
#include <stdlib.h>
#include <string.h>

/* Column width constants */
#define MIN_COL_WIDTH 4
#define MAX_COL_WIDTH 40
#define DEFAULT_COL_WIDTH 15

/* ==========================================================================
 * Helpers
 * ========================================================================== */

int nav_get_visible_rows(TuiState *tui) {
  if (!tui || !tui->main_win) return 10;

  /* Content height minus header row and border */
  int visible = tui->content_height - 2;
  return visible > 0 ? visible : 1;
}

int nav_get_column_width(TuiState *tui, size_t col) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->data) return DEFAULT_COL_WIDTH;

  LaceResult *data = tab->data;
  if (col >= data->num_columns) return DEFAULT_COL_WIDTH;

  /* Calculate width based on column name and first few rows */
  const char *name = data->columns[col].name;
  int width = name ? (int)strlen(name) : MIN_COL_WIDTH;

  /* Check data widths (first 50 rows for performance) */
  size_t check_rows = data->num_rows < 50 ? data->num_rows : 50;
  for (size_t row = 0; row < check_rows; row++) {
    if (row >= data->num_rows) break;
    LaceValue *val = &data->rows[row].cells[col];
    if (val->type == LACE_TYPE_TEXT && val->text.data) {
      int len = (int)strlen(val->text.data);
      if (len > width) width = len;
    } else if (val->type == LACE_TYPE_INT) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%lld", (long long)val->int_val);
      int len = (int)strlen(buf);
      if (len > width) width = len;
    } else if (val->type == LACE_TYPE_FLOAT) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.6g", val->float_val);
      int len = (int)strlen(buf);
      if (len > width) width = len;
    }
  }

  /* Clamp to min/max */
  if (width < MIN_COL_WIDTH) width = MIN_COL_WIDTH;
  if (width > MAX_COL_WIDTH) width = MAX_COL_WIDTH;

  return width;
}

void nav_calculate_column_widths(TuiState *tui) {
  /* Column widths are calculated dynamically in nav_get_column_width */
  /* This function is a no-op placeholder for compatibility */
  (void)tui;
}

void nav_ensure_cursor_visible(TuiState *tui) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->data) return;

  int visible_rows = nav_get_visible_rows(tui);

  /* Adjust vertical scroll */
  if (tab->cursor_row < tab->scroll_row) {
    tab->scroll_row = tab->cursor_row;
  } else if (tab->cursor_row >= tab->scroll_row + (size_t)visible_rows) {
    tab->scroll_row = tab->cursor_row - visible_rows + 1;
  }

  /* Adjust horizontal scroll */
  size_t num_cols = tab->data->num_columns;
  if (tab->cursor_col < tab->scroll_col) {
    tab->scroll_col = tab->cursor_col;
  } else {
    /* Calculate visible columns */
    int x = 1;
    size_t last_visible = tab->scroll_col;
    int win_cols = tui->content_width;

    for (size_t col = tab->scroll_col; col < num_cols; col++) {
      int w = nav_get_column_width(tui, col);
      if (x + w + 1 > win_cols) break;
      x += w + 1;
      last_visible = col;
    }

    if (tab->cursor_col > last_visible) {
      tab->scroll_col = tab->cursor_col;
    }
  }

  tui->app->needs_redraw = true;
}

/* ==========================================================================
 * Cursor Movement
 * ========================================================================== */

void nav_move_cursor(TuiState *tui, int row_delta, int col_delta) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->data || tab->data->num_rows == 0) return;

  size_t num_rows = tab->data->num_rows;
  size_t num_cols = tab->data->num_columns;

  /* Update row */
  if (row_delta < 0) {
    if (tab->cursor_row > 0) {
      tab->cursor_row--;
    } else if (tab->data_offset > 0) {
      /* At first loaded row, try to load previous page */
      if (app_load_more(tui->app, false)) {
        /* Stay at row 0 after loading previous data */
      }
    }
  } else if (row_delta > 0) {
    if (tab->cursor_row < num_rows - 1) {
      tab->cursor_row++;
    } else if (tab->data_offset + num_rows < tab->total_rows) {
      /* At last loaded row, try to load more */
      if (app_load_more(tui->app, true)) {
        tab->cursor_row++;
      }
    }
  }

  /* Update column */
  if (col_delta < 0 && tab->cursor_col > 0) {
    tab->cursor_col--;
  } else if (col_delta > 0 && num_cols > 0 && tab->cursor_col < num_cols - 1) {
    tab->cursor_col++;
  }

  nav_ensure_cursor_visible(tui);
}

void nav_page_up(TuiState *tui) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->data) return;

  int page_size = nav_get_visible_rows(tui);

  /* Check if we need to load previous data */
  if (tab->cursor_row < (size_t)page_size && tab->data_offset > 0) {
    app_load_more(tui->app, false);
  }

  /* Move cursor up by page size */
  if (tab->cursor_row > (size_t)page_size) {
    tab->cursor_row -= page_size;
  } else {
    tab->cursor_row = 0;
  }

  /* Adjust scroll */
  if (tab->scroll_row > (size_t)page_size) {
    tab->scroll_row -= page_size;
  } else {
    tab->scroll_row = 0;
  }

  nav_ensure_cursor_visible(tui);
}

void nav_page_down(TuiState *tui) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->data || tab->data->num_rows == 0) return;

  int page_size = nav_get_visible_rows(tui);
  size_t num_rows = tab->data->num_rows;

  size_t target_row = tab->cursor_row + page_size;

  /* Check if we need to load more data */
  if (target_row >= num_rows) {
    size_t loaded_end = tab->data_offset + num_rows;
    if (loaded_end < tab->total_rows) {
      app_load_more(tui->app, true);
      num_rows = tab->data->num_rows;
    }
    /* Clamp to available data */
    target_row = num_rows > 0 ? num_rows - 1 : 0;
  }

  tab->cursor_row = target_row;

  /* Adjust scroll */
  tab->scroll_row += page_size;
  size_t max_scroll = num_rows > (size_t)page_size ? num_rows - page_size : 0;
  if (tab->scroll_row > max_scroll) {
    tab->scroll_row = max_scroll;
  }

  nav_ensure_cursor_visible(tui);
}

void nav_home(TuiState *tui) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab) return;

  /* If not at start, reload from beginning */
  if (tab->data_offset > 0) {
    tab->data_offset = 0;
    app_refresh_data(tui->app);
  }

  tab->cursor_row = 0;
  tab->cursor_col = 0;
  tab->scroll_row = 0;
  tab->scroll_col = 0;

  tui->app->needs_redraw = true;
}

void nav_end(TuiState *tui) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab) return;

  /* If not at end, load last page */
  size_t loaded_end = tab->data_offset + (tab->data ? tab->data->num_rows : 0);
  if (loaded_end < tab->total_rows) {
    size_t page_size = 500; /* Match PAGE_SIZE from app.c */
    size_t last_offset = tab->total_rows > page_size ? tab->total_rows - page_size : 0;
    tab->data_offset = last_offset;
    app_refresh_data(tui->app);
  }

  if (tab->data && tab->data->num_rows > 0) {
    tab->cursor_row = tab->data->num_rows - 1;

    /* Adjust scroll to show last rows */
    int visible = nav_get_visible_rows(tui);
    if (tab->data->num_rows > (size_t)visible) {
      tab->scroll_row = tab->data->num_rows - visible;
    } else {
      tab->scroll_row = 0;
    }
  }

  tui->app->needs_redraw = true;
}

void nav_column_first(TuiState *tui) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab) return;

  tab->cursor_col = 0;
  tab->scroll_col = 0;

  tui->app->needs_redraw = true;
}

void nav_column_last(TuiState *tui) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->data || tab->data->num_columns == 0) return;

  tab->cursor_col = tab->data->num_columns - 1;
  nav_ensure_cursor_visible(tui);
}

/* ==========================================================================
 * Sidebar Navigation
 * ========================================================================== */

void nav_sidebar_up(TuiState *tui) {
  AppState *app = tui->app;
  Connection *conn = app_current_connection(app);
  if (!conn || conn->num_tables == 0) return;

  if (app->sidebar_selected > 0) {
    app->sidebar_selected--;

    /* Adjust scroll to keep selection visible */
    if (app->sidebar_selected < app->sidebar_scroll) {
      app->sidebar_scroll = app->sidebar_selected;
    }

    app->needs_redraw = true;
  }
}

void nav_sidebar_down(TuiState *tui) {
  AppState *app = tui->app;
  Connection *conn = app_current_connection(app);
  if (!conn || conn->num_tables == 0) return;

  if (app->sidebar_selected < conn->num_tables - 1) {
    app->sidebar_selected++;

    /* Adjust scroll to keep selection visible */
    int visible = tui->content_height - 3;
    if (visible < 1) visible = 1;
    if (app->sidebar_selected >= app->sidebar_scroll + (size_t)visible) {
      app->sidebar_scroll = app->sidebar_selected - visible + 1;
    }

    app->needs_redraw = true;
  }
}

void nav_sidebar_open_table(TuiState *tui) {
  AppState *app = tui->app;
  Connection *conn = app_current_connection(app);
  if (!conn || conn->num_tables == 0) return;

  if (app->sidebar_selected < conn->num_tables) {
    const char *table = conn->tables[app->sidebar_selected];
    app_open_table(app, app->active_connection, table);

    /* Switch focus to main content */
    tui->in_sidebar = false;
  }
}
