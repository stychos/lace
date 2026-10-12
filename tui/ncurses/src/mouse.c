/*
 * Lace
 * Mouse input: tab bar, sidebar, filters panel, table and query panes
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/config.h"
#include "core/app_state.h"
#include "render_helpers.h"
#include "tui_internal.h"
#include "query_internal.h"
#include <stdlib.h>
#include <string.h>

/* Where and how the pointer was used, in screen coordinates */
typedef struct {
  int x, y;
  bool is_double;
  bool scroll_up, scroll_down;
  int sidebar_width; /* 0 when the sidebar is hidden */
  int tab_y;         /* Row of the tab bar */
  int main_y;        /* First row of the sidebar and main windows */
} MouseClick;

/* Scroll wheel: move the table or query results */
static bool mouse_scroll(TuiState *state, const MouseClick *m) {
  /* Only scroll in main area */
  if (m->x >= m->sidebar_width) {
    int scroll_amount = 3; /* Scroll 3 rows at a time */

    /* Check if we're in a query tab with results */
    Tab *scroll_tab = TUI_TAB(state);
    if (scroll_tab && scroll_tab->type == TAB_TYPE_QUERY &&
        scroll_tab->query_results &&
        scroll_tab->query_results->num_rows > 0) {
      /* Calculate results area position */
      int results_start_y =
          m->main_y + query_editor_height(getmaxy(state->main_win)) + 1;

      if (m->y >= results_start_y) {
        /* Scroll query results using helper that handles pagination */
        UITabState *scroll_ui = TUI_TAB_UI(state);
        if (scroll_ui) {
          scroll_ui->query_focus_results = true;
          scroll_ui->query_unfocused = false;
        }
        int delta = m->scroll_up ? -scroll_amount : scroll_amount;
        tui_query_scroll_results(state, delta);
        tui_set_sidebar_focused(state, false);
        return true;
      }
    }

    /* Handle table data scrolling via VmTable */
    VmTable *scroll_vm = tui_vm_table(state);
    if (scroll_vm) {
      size_t cursor_row, cursor_col;
      table_vm_get_cursor(scroll_vm, &cursor_row, &cursor_col);
      size_t scroll_row, scroll_col;
      table_vm_get_scroll(scroll_vm, &scroll_row, &scroll_col);
      size_t loaded_rows = table_vm_row_count(scroll_vm);

      if (m->scroll_up) {
        /* Scroll up - move cursor up */
        if (cursor_row >= (size_t)scroll_amount) {
          cursor_row -= scroll_amount;
        } else {
          cursor_row = 0;
        }
      } else {
        /* Scroll down - move cursor down */
        cursor_row += scroll_amount;
        if (cursor_row >= loaded_rows) {
          cursor_row = loaded_rows > 0 ? loaded_rows - 1 : 0;
        }
      }

      /* Adjust scroll to keep cursor visible using layout info */
      LayoutInfo layout = tui_get_layout_info(state);
      int visible_rows = layout.visible_rows;
      if (cursor_row < scroll_row) {
        scroll_row = cursor_row;
      } else if (cursor_row >= scroll_row + (size_t)visible_rows) {
        scroll_row = cursor_row - visible_rows + 1;
      }

      /* Update via viewmodel */
      table_vm_set_cursor(scroll_vm, cursor_row, cursor_col);
      table_vm_set_scroll(scroll_vm, scroll_row, scroll_col);

      /* Check if we need to load more rows (pagination) */
      tui_check_speculative_prefetch(state);

      tui_set_sidebar_focused(state, false);
      tui_set_filters_focused(state, false);
    }
  }
  return true;
}

/* Tab bar: switch tabs (double click closes) */
static bool mouse_click_tab_bar(TuiState *state, const MouseClick *m) {
  /* If currently editing, save the edit first */
  if (state->editing) {
    tui_confirm_edit(state);
  }

  /* Calculate which tab was clicked based on x position */
  Workspace *click_ws = TUI_WORKSPACE(state);
  if (!click_ws)
    return true;

  int x = 0;
  for (size_t i = 0; i < click_ws->num_tabs; i++) {
    Tab *click_tab = &click_ws->tabs[i];
    if (!click_tab->active)
      continue;

    int tab_width = tui_tab_width(click_tab); /* As tui_draw_tabs lays out */

    if (m->x >= x && m->x < x + tab_width) {
      if (m->is_double) {
        /* Double-click: close the tab */
        Tab *target_tab = &click_ws->tabs[i];

        /* Check if query tab has content */
        if (target_tab->type == TAB_TYPE_QUERY &&
            ((target_tab->query_text && target_tab->query_len > 0) ||
             target_tab->query_results)) {
          /* Ask for confirmation */
          if (!tui_show_confirm_dialog(
                  state, "Close query tab with unsaved content?")) {
            return true; /* User cancelled */
          }
        }

        /* Switch to tab first if not current, then close */
        if (i != click_ws->current_tab) {
          tab_switch(state, i);
        }
        tab_close(state);
        tui_set_sidebar_focused(state, false);
        tui_set_filters_focused(state, false);
      } else {
        /* Single click: switch to tab */
        if (i != click_ws->current_tab) {
          tab_switch(state, i);
          tui_set_sidebar_focused(state, false);
          tui_set_filters_focused(state, false);
        }
      }
      return true;
    }

    x += tab_width;
    if (x > state->term_cols)
      break;
  }
  return true; /* Clicked on tab bar but not on a specific tab */
}

/* Sidebar: open a table (double click: in a new tab) */
static bool mouse_click_sidebar(TuiState *state, const MouseClick *m) {
  /* If currently editing, save the edit first */
  if (state->editing) {
    tui_confirm_edit(state);
  }

  /* Sidebar layout (inside sidebar_win, which starts at screen row m->main_y):
     row 0 = border+title, row 1 = filter, row 2 = separator, row 3+ = table
     list */
  int sidebar_row = m->y - m->main_y; /* Convert to sidebar_win coordinates */

  /* Validate sidebar_row is non-negative before use */
  if (sidebar_row < 0) {
    return true;
  }

  /* Click on filter field (row 1 in sidebar_win = screen row 2) */
  if (sidebar_row == 1) {
    tui_set_sidebar_focused(state, true);
    state->sidebar_filter_active = true;
    tui_set_filters_focused(state, false);
    return true;
  }

  /* Clicking elsewhere in sidebar deactivates filter */
  state->sidebar_filter_active = false;
  tui_set_filters_focused(state, false);

  /* Click on table list */
  int list_start_y = 3; /* First table entry row in sidebar window */
  int clicked_row = sidebar_row - list_start_y;

  if (clicked_row >= 0) {
    size_t filtered_count = tui_count_filtered_tables(state);
    char **tables = TUI_TABLES(state);
    size_t num_tables = TUI_NUM_TABLES(state);

    size_t target_idx = tui_sidebar_scroll(state) + (size_t)clicked_row;
    if (target_idx < filtered_count) {
      /* Find the actual table index */
      size_t actual_idx = tui_get_filtered_table_index(state, target_idx);
      if (actual_idx < num_tables && tables) {
        /* Update sidebar highlight */
        tui_set_sidebar_highlight(state, target_idx);
        tui_set_sidebar_focused(state, true);

        if (m->is_double) {
          /* Double-click: always open in new tab */
          tab_create(state, actual_idx);
          tui_set_sidebar_focused(state, false);
        } else {
          /* Single click: the same as Enter in the sidebar */
          sidebar_open_table(state, actual_idx);
          state->sidebar_last_position = target_idx;
          tui_set_sidebar_focused(state, false);
        }
        return true;
      }
    }
  }
  return true;
}

/* Query tab: focus the editor, pick a result cell */
static bool mouse_click_query(TuiState *state, const MouseClick *m,
                              Tab *query_click_tab,
                              UITabState *query_click_ui) {
  state->sidebar_filter_active = false;
  tui_set_sidebar_focused(state, false);
  tui_set_filters_focused(state, false);

  /* If currently editing query results, save the edit first */
  if (query_click_ui->query_result_editing) {
    tui_query_confirm_result_edit(state);
  }

  /* Get actual main window dimensions */
  int main_win_rows, main_win_cols;
  getmaxyx(state->main_win, main_win_rows, main_win_cols);

  /* Calculate query view layout */
  int win_rows = main_win_rows;
  int editor_height = query_editor_height(win_rows);
  /* Grid starts at results_start = editor_height + 1 (window coords)
   * In screen coords: m->main_y + editor_height + 1
   * Grid layout: column headers (1 row) + separator (1 row) + data */
  int results_start_y = m->main_y + editor_height + 1; /* screen coords */
  int results_data_y =
      results_start_y + 2; /* After col headers + separator */

  if (m->y < results_start_y) {
    /* Clicked in editor area */
    query_click_ui->query_focus_results = false;
    query_click_ui->query_unfocused = false;
  } else if (query_click_tab->query_results &&
             query_click_tab->query_results->num_rows > 0 &&
             m->y >= results_data_y) {
    /* Clicked in results data area */
    query_click_ui->query_focus_results = true;
    query_click_ui->query_unfocused = false;

    /* Calculate which row was clicked */
    int clicked_row = m->y - results_data_y;
    size_t target_row =
        query_click_tab->query_result_scroll_row + (size_t)clicked_row;

    if (target_row < query_click_tab->query_results->num_rows) {
      /* Calculate which column was clicked */
      int rel_x = m->x - m->sidebar_width;
      int x_pos = 1;
      size_t target_col = query_click_tab->query_result_scroll_col;

      for (size_t col = query_click_tab->query_result_scroll_col;
           col < query_click_tab->query_results->num_columns; col++) {
        int width = DEFAULT_COL_WIDTH;
        if (query_click_tab->query_result_col_widths &&
            col < query_click_tab->query_result_num_cols) {
          width = query_click_tab->query_result_col_widths[col];
        }
        if (rel_x >= x_pos && rel_x < x_pos + width) {
          target_col = col;
          break;
        }
        x_pos += width + 1;
        if (x_pos > main_win_cols)
          break;
        target_col = col + 1;
      }

      if (target_col < query_click_tab->query_results->num_columns) {
        query_click_tab->query_result_row = target_row;
        query_click_tab->query_result_col = target_col;

        /* Double-click: enter edit mode */
        if (m->is_double && !query_click_ui->query_result_editing) {
          tui_query_start_result_edit(state);
        }
      }
    }
  } else {
    /* Clicked in results header area */
    query_click_ui->query_focus_results = true;
    query_click_ui->query_unfocused = false;
  }
  return true;
}

/* Filters panel. Returns false for a click below the panel (the table) */
static bool mouse_click_filters(TuiState *state, const MouseClick *m) {
  int rel_y = m->y - m->main_y;
  int rel_x = m->x - m->sidebar_width;
  int filters_height = tui_get_filters_panel_height(state);

  if (rel_y >= 0 && rel_y < filters_height) {
    /* Click is in filters panel area - delegate to filters handler */
    state->sidebar_filter_active = false;
    tui_handle_filters_click(state, rel_x, rel_y);
    return true;
  }
  return false;
}

/* Table: move the cursor, open the cell editor */
static bool mouse_click_table(TuiState *state, const MouseClick *m) {
  /* Connection tab: clicking main area should not unfocus sidebar */
  Tab *main_click_tab = TUI_TAB(state);
  if (main_click_tab && main_click_tab->type == TAB_TYPE_CONNECTION) {
    return true; /* Ignore clicks in connection tab main area */
  }

  /* Clicking in main area deactivates sidebar filter and unfocuses panels */
  state->sidebar_filter_active = false;
  tui_set_sidebar_focused(state, false);
  tui_set_filters_focused(state, false);

  /* If currently editing, save the edit first */
  if (state->editing) {
    tui_confirm_edit(state);
  }

  /* Use VmTable for cursor/scroll access */
  VmTable *click_vm = tui_vm_table(state);
  if (!click_vm) {
    return true; /* No data to select, but focus is updated */
  }

  size_t loaded_rows = table_vm_row_count(click_vm);
  size_t num_cols = table_vm_col_count(click_vm);
  if (loaded_rows == 0 || num_cols == 0) {
    return true;
  }

  /* Get actual main window dimensions */
  int table_win_rows, table_win_cols;
  getmaxyx(state->main_win, table_win_rows, table_win_cols);
  (void)table_win_rows;

  /* Coordinates relative to the main window (starts at screen row m->main_y) */
  int rel_x = m->x - m->sidebar_width;
  int rel_y = m->y - m->main_y;

  /* Calculate data start row accounting for all header elements */
  int filters_height = 0;
  Tab *click_tab = TUI_TAB(state);
  if (tui_filters_visible(state) && click_tab &&
      click_tab->type == TAB_TYPE_TABLE) {
    filters_height = tui_get_filters_panel_height(state);
  }

  /* Data rows start after: filters panel + header line + column names +
   * sort indicator row (if sorting) + separator */
  int data_start_y =
      filters_height + 3; /* +3 = header line + col names + separator */

  /* Add extra row if sorting is active (sort indicator row) */
  if (click_tab && click_tab->num_sort_entries > 0) {
    data_start_y += 1;
  }

  int clicked_data_row = rel_y - data_start_y;

  if (clicked_data_row >= 0) {
    /* Get current scroll position */
    size_t scroll_row, scroll_col;
    table_vm_get_scroll(click_vm, &scroll_row, &scroll_col);

    /* Calculate which row was clicked */
    size_t target_row = scroll_row + (size_t)clicked_data_row;

    if (target_row < loaded_rows) {
      /* Calculate which column was clicked */
      int x_pos = 1; /* Data starts at x=1 */
      size_t target_col = scroll_col;

      /* Use widget col_widths if available (same as drawing code) */
      int *col_widths = click_vm->col_widths;
      size_t num_col_widths = click_vm->num_col_widths;

      for (size_t col = scroll_col; col < num_cols; col++) {
        int width = (col_widths && col < num_col_widths)
                        ? col_widths[col]
                        : DEFAULT_COL_WIDTH;
        /* Include separator in clickable area for preceding column */
        if (rel_x >= x_pos && rel_x <= x_pos + width) {
          target_col = col;
          break;
        }
        x_pos += width + 1; /* +1 for separator */
        if (x_pos > table_win_cols)
          break;
        target_col = col + 1;
      }

      if (target_col < num_cols) {
        /* Update cursor position via viewmodel */
        table_vm_set_cursor(click_vm, target_row, target_col);
        tui_set_sidebar_focused(state, false);
        tui_set_filters_focused(state, false);

        /* Check if we need to load more rows (pagination) */
        tui_check_speculative_prefetch(state);

        /* Double-click: enter edit mode */
        if (m->is_double) {
          tui_start_edit(state);
        }

        return true;
      }
    }
  }
  return false;
}

bool tui_handle_mouse_event(TuiState *state, const UiEvent *event) {
  if (!event || event->type != UI_EVENT_MOUSE)
    return false;

  int mouse_y = event->mouse.y;
  int mouse_x = event->mouse.x;
  bool is_double = (event->mouse.action == UI_MOUSE_DOUBLE_CLICK);
  bool is_click = (event->mouse.action == UI_MOUSE_CLICK);
  bool is_scroll_up = (event->mouse.button == UI_MOUSE_SCROLL_UP);
  bool is_scroll_down = (event->mouse.button == UI_MOUSE_SCROLL_DOWN);

  /* Determine click location. Rows come from the windows themselves: the
   * tab bar is on row 0 or 1 and the content starts below it, depending on
   * whether the header is shown. */
  int sidebar_width = state->sidebar_visible ? SIDEBAR_WIDTH : 0;
  int tab_y = state->tab_win ? getbegy(state->tab_win) : 1;
  int main_y = state->main_win ? getbegy(state->main_win) : tab_y + 1;
  MouseClick m = {mouse_x, mouse_y,      is_double,     is_scroll_up,
                  is_scroll_down, sidebar_width, tab_y, main_y};

  /* Handle scroll wheel - scroll relative to cursor */
  if (is_scroll_up || is_scroll_down) {
    return mouse_scroll(state, &m);
  }

  if (!is_click && !is_double)
    return false;

  /* Check if click is on tab bar */
  if (mouse_y == tab_y && TUI_NUM_WORKSPACES(state) > 0) {
    return mouse_click_tab_bar(state, &m);
  }

  /* Check if click is in sidebar */
  if (state->sidebar_visible && mouse_x < sidebar_width) {
    return mouse_click_sidebar(state, &m);
  }

  /* Check if click is in query tab area */
  Tab *query_click_tab = TUI_TAB(state);
  UITabState *query_click_ui = TUI_TAB_UI(state);
  if (mouse_x >= sidebar_width && query_click_tab && query_click_ui &&
      query_click_tab->type == TAB_TYPE_QUERY) {
    return mouse_click_query(state, &m, query_click_tab, query_click_ui);
  }

  /* Check if click is in filters panel (TABLE tabs only, when filters visible)
   */
  Tab *filters_click_tab = TUI_TAB(state);
  if (mouse_x >= sidebar_width && tui_filters_visible(state) && filters_click_tab &&
      filters_click_tab->type == TAB_TYPE_TABLE && mouse_click_filters(state, &m)) {
    return true;
  }

  /* Check if click is in main table area */
  if (mouse_x >= sidebar_width) {
    return mouse_click_table(state, &m);
  }

  return false;
}
