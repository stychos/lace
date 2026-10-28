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
#include "views/form_tab.h"
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
          m->main_y + query_editor_height(scroll_tab, getmaxy(state->main_win)) + 1;

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

/* Sidebar: open a table (double click: in a new tab) */
static bool mouse_click_sidebar(TuiState *state, const MouseClick *m) {
  /* If currently editing, save the edit first */
  if (state->editing) {
    tui_confirm_edit(state);
  }

  /* Sidebar layout (inside sidebar_win, which starts at screen row
     m->main_y): the search field while it is used, TABLES, then the list
     from its scroll position (a note first when no table shows) */
  int sidebar_row = m->y - m->main_y; /* Convert to sidebar_win coordinates */
  if (sidebar_row < 0)
    return true;
  bool search = state->sidebar_filter_active || state->sidebar_filter_len > 0;
  if (search && sidebar_row == 0) { /* the search field */
    tui_set_sidebar_focused(state, true);
    state->sidebar_filter_active = true;
    tui_set_filters_focused(state, false);
    return true;
  }

  /* Clicking elsewhere in sidebar deactivates filter */
  state->sidebar_filter_active = false;
  tui_set_filters_focused(state, false);

  /* the list's line under the click; a note is its first line */
  int list_row = sidebar_row - tui_sidebar_list_top(state);
  long line = list_row >= 0 ? (long)tui_sidebar_scroll(state) + list_row : -1;
  if (tui_sidebar_has_note(state))
    line--;

  if (line >= 0) {
    size_t target_idx = (size_t)line;
    SidebarEntry e;
    if (tui_sidebar_entry_at(state, target_idx, &e)) {
      /* Update sidebar highlight */
      tui_set_sidebar_highlight(state, target_idx);
      tui_set_sidebar_focused(state, true);
      if (e.kind == SB_HEADER) { /* a click folds and unfolds the section */
        sidebar_open_entry(state, &e, false);
      } else if (m->is_double) {
        /* A double click opens for keeps (the preview, if it is one, kept) */
        sidebar_open_entry(state, &e, false);
        tui_set_sidebar_focused(state, false);
      } else {
        /* A single click: the preview tab */
        sidebar_preview_entry(state, &e);
        state->sidebar_last_position = target_idx;
        tui_set_sidebar_focused(state, false);
      }
      return true;
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
  int editor_height = query_editor_height(query_click_tab, win_rows);
  /* The results start under the divider (window row editor_height + 1):
   * the grid's header row, then the data */
  int results_start_y = m->main_y + editor_height + 1; /* screen coords */
  int results_data_y = results_start_y + 1; /* after the grid's header row */

  if (m->y < results_start_y - 1) {
    /* A click in the editor puts the cursor there (as the notcurses TUI):
     * the line under it, the display column after the line numbers */
    query_click_ui->query_focus_results = false;
    query_click_ui->query_unfocused = false;
    QueryLineInfo *lines = NULL;
    size_t nlines = 0;
    query_rebuild_line_cache(query_click_tab, &lines, &nlines);
    if (nlines > 0) {
      size_t d = nlines, gw = 1;
      while (d >= 10) {
        d /= 10;
        gw++;
      }
      gw++; /* the gap after the numbers */
      size_t line = query_click_tab->query_scroll_line + (size_t)(m->y - m->main_y);
      if (line >= nlines)
        line = nlines - 1;
      int dx = m->x - m->sidebar_width - (int)gw;
      query_click_tab->query_cursor =
          query_line_col_to_cursor(query_click_tab, line, (size_t)(dx > 0 ? dx : 0), lines, nlines);
    }
    free(lines);
  } else if (m->y == results_start_y - 1) {
    /* the divider: the results */
    query_click_ui->query_focus_results = query_click_tab->query_results != NULL;
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
      int x_pos = 0;
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
  int rel_y = m->y - m->main_y - tui_subview_strip_rows(state); /* the panel sits under the strip */
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

  /* Data rows start under the sub-view strip, the filters panel and the
   * grid's header row */
  int data_start_y = tui_get_layout_info(state).data_start_y;

  int clicked_data_row = rel_y - data_start_y;

  if (clicked_data_row >= 0) {
    /* Get current scroll position */
    size_t scroll_row, scroll_col;
    table_vm_get_scroll(click_vm, &scroll_row, &scroll_col);

    /* Calculate which row was clicked */
    size_t target_row = scroll_row + (size_t)clicked_data_row;

    if (target_row < loaded_rows) {
      /* Calculate which column was clicked */
      int x_pos = 0; /* the grid starts at the window's edge */
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
  int sidebar_width = state->sidebar_win ? tui_sidebar_width(state) : 0;
  int tab_y = state->tab_win ? getbegy(state->tab_win) : 1;
  int main_y = state->main_win ? getbegy(state->main_win) : tab_y + 1;
  MouseClick m = {mouse_x, mouse_y,      is_double,     is_scroll_up,
                  is_scroll_down, sidebar_width, tab_y, main_y};

  /* The header, the tabs strip and the status line (chrome.c) */
  if (tui_chrome_mouse(state, event))
    return true;

  /* A connection form takes the clicks and the wheel below the tab bar */
  Tab *form_click_tab = TUI_TAB(state);
  if (form_click_tab && form_click_tab->type == TAB_TYPE_FORM && mouse_y >= main_y) {
    int wheel = is_scroll_up ? -1 : is_scroll_down ? 1 : 0;
    if (!wheel && !is_click && !is_double)
      return false;
    return form_tab_handle_mouse(state, form_click_tab, mouse_y - main_y, mouse_x, is_double, wheel);
  }

  /* Handle scroll wheel - scroll relative to cursor */
  if (is_scroll_up || is_scroll_down) {
    return mouse_scroll(state, &m);
  }

  if (!is_click && !is_double)
    return false;

  /* Check if click is in sidebar */
  if (state->sidebar_win && mouse_x < sidebar_width) {
    return mouse_click_sidebar(state, &m);
  }

  /* Check if click is in query tab area */
  Tab *query_click_tab = TUI_TAB(state);
  UITabState *query_click_ui = TUI_TAB_UI(state);
  if (mouse_x >= sidebar_width && query_click_tab && query_click_ui &&
      query_click_tab->type == TAB_TYPE_QUERY) {
    return mouse_click_query(state, &m, query_click_tab, query_click_ui);
  }

  /* The sub-view strip, or a sub-view's list (TABLE tabs) */
  if (mouse_x >= sidebar_width && query_click_tab && query_click_tab->type == TAB_TYPE_TABLE) {
    if (state->editing)
      tui_confirm_edit(state);
    if (tui_subview_click(state, mouse_x - sidebar_width, mouse_y - main_y)) {
      state->sidebar_filter_active = false;
      tui_set_sidebar_focused(state, false);
      tui_set_filters_focused(state, false);
      return true;
    }
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
