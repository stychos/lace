/*
 * Lace
 * Query tab implementation - Main coordinator
 *
 * Query editor state is stored directly in Tab.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include "query_internal.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>


/* Note: History recording is now handled automatically by the database layer
 * via the history callback set up in app_add_connection(). */

/* Create a new query tab */
bool tab_create_query(TuiState *state) {
  if (!state || !state->app)
    return false;

  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws) {
    /* No workspace - need to create one first */
    ws = app_create_workspace(state->app);
    if (!ws)
      return false;
  }

  /* Get connection index - use current tab's connection if available */
  size_t connection_index = 0;
  Tab *current_tab = TUI_TAB(state);
  if (current_tab) {
    connection_index = current_tab->connection_index;
  }

  /* Find next query number by scanning existing tabs */
  int max_query_num = 0;
  for (size_t i = 0; i < ws->num_tabs; i++) {
    Tab *t = &ws->tabs[i];
    if (t->type == TAB_TYPE_QUERY && t->table_name) {
      int num = 0;
      if (sscanf(t->table_name, "Query %d", &num) == 1) {
        if (num > max_query_num)
          max_query_num = num;
      }
    }
  }

  /* Create new tab with connection reference */
  Tab *tab = workspace_create_query_tab(ws, connection_index);
  if (!tab)
    return false;

  /* Ensure UITabState capacity for new tab */
  tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                             ws->current_tab);

  /* Initialize per-tab UI state */
  UITabState *ui = TUI_TAB_UI(state);
  if (ui) {
    /* Query tab has no filters panel */
    ui->filters_visible = false;
    ui->filters_focused = false;

    /* A new query tab starts with focus on the editor, ready to type */
    ui->query_focus_results = false;
    ui->query_unfocused = false;
  }
  state->shown_tab_uid = tab->uid;

  /* Update tab name (free the default "Query" name set by
   * workspace_create_query_tab) */
  free(tab->table_name);
  tab->table_name = str_printf("Query %d", max_query_num + 1);
  if (!tab->table_name) {
    workspace_close_tab(ws, ws->current_tab);
    return false;
  }

  /* Initialize query buffer */
  tab->query_capacity = QUERY_INITIAL_CAPACITY;
  tab->query_text = safe_calloc(tab->query_capacity, 1);
  tab->query_len = 0;
  tab->query_cursor = 0;
  tab->query_scroll_line = 0;
  tab->query_scroll_col = 0;

  /* Query mode doesn't use table data - Tab starts clean */

  /* Reset TUI state for new tab - all panels start unfocused, filters closed */
  state->sidebar_focused = false;
  tui_set_filters_visible(state, false);
  tui_set_filters_focused(state, false);
  state->filters_was_focused = false;
  state->filters_editing = false;
  state->filters_cursor_row = 0;
  state->filters_cursor_col = 0;
  state->filters_scroll = 0;

  char *run_key = hotkey_get_display(state->app->config, HOTKEY_EXECUTE_QUERY);
  char *all_key = hotkey_get_display(state->app->config, HOTKEY_EXECUTE_ALL);
  tui_set_status(state, "Query tab opened. %s to run, %s to run all",
                 run_key ? run_key : "Ctrl+R", all_key ? all_key : "Ctrl+A");
  free(run_key);
  free(all_key);
  return true;
}

/* Legacy wrapper for compatibility */
bool workspace_create_query(TuiState *state) { return tab_create_query(state); }

/* Load query result rows at specific offset (replaces current data) */
bool query_load_rows_at(TuiState *state, Tab *tab, size_t offset) {
  if (!state || !TUI_CONN(state) || !tab || !tab->query_paginated ||
      !tab->query_base_sql)
    return false;

  /* Clamp offset */
  if (tab->query_total_known && offset >= tab->query_total_rows) {
    offset = tab->query_total_rows > TUI_PAGE_SIZE
                 ? tab->query_total_rows - TUI_PAGE_SIZE
                 : 0;
  }

  char *page_sql = query_page_sql(tab, TUI_PAGE_SIZE, offset);
  char *err = NULL;
  bool cancelled = false;
  ResultSet *data =
      query_run_derived(state, page_sql, "Loading rows...", &err, &cancelled);
  free(page_sql);

  if (!data) {
    if (!cancelled) {
      tui_set_error(state, "Query failed: %s", err ? err : "Unknown error");
    }
    free(err);
    return false;
  }
  free(err);

  /* Free old results */
  if (tab->query_results) {
    db_result_free(tab->query_results);
  }

  tab->query_results = data;
  tab->query_loaded_offset = offset;
  tab->query_loaded_count = data->num_rows;

  /* Recalculate column widths based on content */
  free(tab->query_result_col_widths);
  tab->query_result_col_widths = NULL;
  if (data->num_columns > 0) {
    tab->query_result_col_widths = safe_calloc(data->num_columns, sizeof(int));
    for (size_t c = 0; c < data->num_columns; c++) {
      int w = data->columns[c].name ? (int)strlen(data->columns[c].name) : 0;
      if (w < MIN_COL_WIDTH)
        w = MIN_COL_WIDTH;
      for (size_t r = 0; r < data->num_rows && r < 100; r++) {
        if (r < data->num_rows && c < data->rows[r].num_cells) {
          char *str = db_value_to_string(&data->rows[r].cells[c]);
          if (str) {
            int vw = (int)strlen(str);
            if (vw > w)
              w = vw;
            free(str);
          }
        }
      }
      if (w > DEFAULT_COL_WIDTH)
        w = DEFAULT_COL_WIDTH;
      tab->query_result_col_widths[c] = w;
    }
  }

  tab_clear_selections(tab); /* Selections are row positions */
  tui_set_status(state, "Loaded %zu/%zu rows", offset + tab->query_loaded_count,
                 tab->query_total_rows);
  return true;
}

/* Public wrapper for starting edit from mouse handler */
void tui_query_start_result_edit(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !ui)
    return;
  if (tab->type != TAB_TYPE_QUERY || !ui->query_focus_results)
    return;
  query_result_start_edit(state, tab);
}

/* Public wrapper for confirming edit from mouse handler */
void tui_query_confirm_result_edit(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !ui)
    return;
  if (tab->type != TAB_TYPE_QUERY || !ui->query_result_editing)
    return;
  query_result_confirm_edit(state, tab);
}

/* Public wrapper for scrolling query results (used by mouse handler) */
void tui_query_scroll_results(TuiState *state, int delta) {
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;
  if (tab->type != TAB_TYPE_QUERY || !tab->query_results)
    return;
  if (tab->query_results->num_rows == 0)
    return;

  int visible = query_visible_result_rows(state);

  if (delta < 0) {
    /* Scroll up */
    size_t amount = (size_t)(-delta);
    if (tab->query_result_row >= amount) {
      tab->query_result_row -= amount;
    } else {
      tab->query_result_row = 0;
    }
  } else if (delta > 0) {
    /* Scroll down */
    tab->query_result_row += (size_t)delta;
    if (tab->query_result_row >= tab->query_results->num_rows) {
      tab->query_result_row = tab->query_results->num_rows > 0
                                  ? tab->query_results->num_rows - 1
                                  : 0;
    }
  }

  /* Adjust scroll position */
  if (tab->query_result_row < tab->query_result_scroll_row) {
    tab->query_result_scroll_row = tab->query_result_row;
  } else if (tab->query_result_row >=
             tab->query_result_scroll_row + (size_t)visible) {
    tab->query_result_scroll_row = tab->query_result_row - visible + 1;
  }

  /* Check if more rows need to be loaded */
  query_check_load_more(state, tab);
}
