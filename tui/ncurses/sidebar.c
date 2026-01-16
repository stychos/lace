/*
 * Lace
 * Sidebar rendering and input handling
 *
 * Uses SidebarViewModel as source of truth for sidebar state.
 * Access via tui_ensure_sidebar_widget() to get the view model.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/config.h"
#include "viewmodel/sidebar_viewmodel.h"
#include "tui_internal.h"
#include "views/config_view.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Widget State Sync Helpers (temporary during migration)
 * ============================================================================
 */

/* Sync TuiState sidebar fields from SidebarWidget */
static void sync_state_from_widget(TuiState *state, SidebarWidget *sw) {
  if (!state || !sw)
    return;

  state->sidebar_highlight = sw->base.state.cursor_row;
  state->sidebar_scroll = sw->base.state.scroll_row;
  state->sidebar_focused = sw->base.state.focused;

  /* Copy filter */
  if (sw->filter_len > 0 && sw->filter_len < sizeof(state->sidebar_filter)) {
    memcpy(state->sidebar_filter, sw->filter, sw->filter_len + 1);
    state->sidebar_filter_len = sw->filter_len;
  } else {
    state->sidebar_filter[0] = '\0';
    state->sidebar_filter_len = 0;
  }
}

/* Sync SidebarWidget from TuiState sidebar fields */
static void sync_widget_from_state(SidebarWidget *sw, TuiState *state) {
  if (!sw || !state)
    return;

  sw->base.state.cursor_row = state->sidebar_highlight;
  sw->base.state.scroll_row = state->sidebar_scroll;
  sw->base.state.focused = state->sidebar_focused;

  /* Copy filter only if changed (to avoid resetting cursor) */
  if (state->sidebar_filter_len > 0) {
    /* Only update if filter text differs */
    if (sw->filter_len != state->sidebar_filter_len ||
        memcmp(sw->filter, state->sidebar_filter, state->sidebar_filter_len) !=
            0) {
      sidebar_widget_set_filter(sw, state->sidebar_filter);
    }
  } else if (sw->filter_len > 0) {
    sidebar_widget_filter_clear(sw);
  }
}

/*
 * Count filtered tables.
 *
 * Uses SidebarWidget when available for filtered count, with fallback
 * to TuiState for legacy compatibility.
 */
size_t tui_count_filtered_tables(TuiState *state) {
  if (!state)
    return 0;

  /* Try SidebarWidget first (new architecture) */
  SidebarWidget *sw = tui_sidebar_widget(state);
  if (sw) {
    return sidebar_widget_count(sw);
  }

  /* Fallback to TuiState-based calculation */
  char **tables = TUI_TABLES(state);
  size_t num_tables = TUI_NUM_TABLES(state);

  if (state->sidebar_filter_len == 0)
    return num_tables;

  size_t count = 0;
  for (size_t i = 0; i < num_tables; i++) {
    if (tables && tables[i] &&
        tui_str_istr(tables[i], state->sidebar_filter)) {
      count++;
    }
  }
  return count;
}

/*
 * Get actual table index from filtered index.
 *
 * Uses SidebarWidget when available, with fallback to TuiState.
 */
size_t tui_get_filtered_table_index(TuiState *state, size_t filtered_idx) {
  if (!state)
    return 0;

  /* Try SidebarWidget first (new architecture) */
  SidebarWidget *sw = tui_sidebar_widget(state);
  if (sw) {
    return sidebar_widget_original_index(sw, filtered_idx);
  }

  /* Fallback to TuiState-based calculation */
  char **tables = TUI_TABLES(state);
  size_t num_tables = TUI_NUM_TABLES(state);

  if (state->sidebar_filter_len == 0)
    return filtered_idx;

  size_t count = 0;
  for (size_t i = 0; i < num_tables; i++) {
    if (tables && tables[i] &&
        tui_str_istr(tables[i], state->sidebar_filter)) {
      if (count == filtered_idx)
        return i;
      count++;
    }
  }
  return 0;
}

/* Get sidebar highlight for a table index */
size_t tui_get_sidebar_highlight_for_table(TuiState *state, size_t table_idx) {
  char **tables = TUI_TABLES(state);
  size_t num_tables = TUI_NUM_TABLES(state);

  if (state->sidebar_filter_len == 0)
    return table_idx;

  size_t count = 0;
  for (size_t i = 0; i < num_tables; i++) {
    if (tables && tables[i] &&
        tui_str_istr(tables[i], state->sidebar_filter)) {
      if (i == table_idx)
        return count;
      count++;
    }
  }
  return 0; /* Table not in filtered list, default to first */
}

/* Update sidebar name scroll animation */
void tui_update_sidebar_scroll_animation(TuiState *state) {
  char **tables = TUI_TABLES(state);
  size_t num_tables = TUI_NUM_TABLES(state);

  if (!state || !state->sidebar_focused || !tables)
    return;

  /* Reset scroll when highlight changes */
  if (state->sidebar_highlight != state->sidebar_last_highlight) {
    state->sidebar_name_scroll = 0;
    state->sidebar_name_scroll_dir = 1;
    state->sidebar_name_scroll_delay = 3; /* Initial pause */
    state->sidebar_last_highlight = state->sidebar_highlight;
    return;
  }

  /* Get highlighted table name */
  size_t actual_idx =
      tui_get_filtered_table_index(state, state->sidebar_highlight);
  if (actual_idx >= num_tables)
    return;

  const char *name = tables[actual_idx];
  if (!name)
    return;

  int max_name_len = SIDEBAR_WIDTH - 4;
  if (max_name_len < 1)
    max_name_len = 1;
  int name_len = (int)strlen(name);

  /* Only animate if name is truncated */
  if (name_len <= max_name_len) {
    state->sidebar_name_scroll = 0;
    return;
  }

  int max_scroll = name_len - max_name_len;

  /* Handle pause at ends */
  if (state->sidebar_name_scroll_delay > 0) {
    state->sidebar_name_scroll_delay--;
    return;
  }

  /* Update scroll position */
  if (state->sidebar_name_scroll_dir > 0) {
    /* Scrolling right (showing more of the end) */
    if ((int)state->sidebar_name_scroll < max_scroll) {
      state->sidebar_name_scroll++;
    } else {
      /* Reached end, pause and reverse */
      state->sidebar_name_scroll_dir = -1;
      state->sidebar_name_scroll_delay = 5;
    }
  } else {
    /* Scrolling left (back to start) */
    if (state->sidebar_name_scroll > 0) {
      state->sidebar_name_scroll--;
    } else {
      /* Reached start, pause and reverse */
      state->sidebar_name_scroll_dir = 1;
      state->sidebar_name_scroll_delay = 5;
    }
  }
}

/* Handle sidebar input when focused */
bool tui_handle_sidebar_input(TuiState *state, const UiEvent *event) {
  if (!state->sidebar_focused || !event || event->type != UI_EVENT_KEY)
    return false;

  int key_char = render_event_get_char(event);

  /* Get/create sidebar widget - use it as source of truth */
  SidebarWidget *sw = tui_ensure_sidebar_widget(state);
  if (sw) {
    /* Sync widget from TuiState in case it was modified elsewhere */
    sync_widget_from_state(sw, state);
  }

  /* Handle filter input mode */
  if (state->sidebar_filter_active) {
    if (render_event_is_special(event, UI_KEY_ESCAPE)) {
      state->sidebar_filter_active = false;
    } else if (render_event_is_special(event, UI_KEY_ENTER) ||
               render_event_is_special(event, UI_KEY_DOWN)) {
      state->sidebar_filter_active = false;
      if (sw) {
        sw->base.state.cursor_row = 0;
        sw->base.state.scroll_row = 0;
      }
      state->sidebar_highlight = 0;
      state->sidebar_scroll = 0;
    } else if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
      if (sw && sw->filter_len > 0) {
        sidebar_widget_filter_backspace(sw);
        sw->base.state.cursor_row = 0;
        sw->base.state.scroll_row = 0;
        sync_state_from_widget(state, sw);
      } else if (state->sidebar_filter_len > 0) {
        state->sidebar_filter[--state->sidebar_filter_len] = '\0';
        state->sidebar_highlight = 0;
        state->sidebar_scroll = 0;
      }
    } else if (render_event_is_ctrl(event, 'K')) {
      /* Ctrl+K - copy filter text to clipboard */
      const char *filter = sw ? sw->filter : state->sidebar_filter;
      size_t filter_len = sw ? sw->filter_len : state->sidebar_filter_len;
      if (filter_len > 0) {
        tui_clipboard_copy(state, filter);
        tui_set_status(state, "Copied to clipboard");
      }
    } else if (render_event_is_ctrl(event, 'U')) {
      /* Ctrl+U - paste from clipboard */
      char *paste_text = tui_clipboard_read(state);
      if (paste_text && paste_text[0]) {
        /* Strip newlines for single-line filter field */
        for (char *p = paste_text; *p; p++) {
          if (*p == '\n' || *p == '\r')
            *p = ' ';
        }
        if (sw) {
          /* Append to widget filter */
          for (const char *p = paste_text; *p; p++) {
            sidebar_widget_filter_append(sw, *p);
          }
          sw->base.state.cursor_row = 0;
          sw->base.state.scroll_row = 0;
          sync_state_from_widget(state, sw);
        } else {
          size_t paste_len = strlen(paste_text);
          size_t space_left =
              sizeof(state->sidebar_filter) - 1 - state->sidebar_filter_len;
          if (paste_len > space_left)
            paste_len = space_left;
          if (paste_len > 0) {
            memcpy(state->sidebar_filter + state->sidebar_filter_len, paste_text,
                   paste_len);
            state->sidebar_filter_len += paste_len;
            state->sidebar_filter[state->sidebar_filter_len] = '\0';
            state->sidebar_highlight = 0;
            state->sidebar_scroll = 0;
          }
        }
      }
      free(paste_text);
    } else if (render_event_is_char(event)) {
      /* Printable character - add to filter */
      if (sw) {
        sidebar_widget_filter_append(sw, (char)key_char);
        sw->base.state.cursor_row = 0;
        sw->base.state.scroll_row = 0;
        sync_state_from_widget(state, sw);
      } else if (state->sidebar_filter_len < sizeof(state->sidebar_filter) - 1) {
        state->sidebar_filter[state->sidebar_filter_len++] = (char)key_char;
        state->sidebar_filter[state->sidebar_filter_len] = '\0';
        state->sidebar_highlight = 0;
        state->sidebar_scroll = 0;
      }
    }
    /* Sync focus state to Tab so it persists across tab switches */
    tab_sync_focus(state);
    return true;
  }

  size_t filtered_count = tui_count_filtered_tables(state);
  const Config *cfg = state->app ? state->app->config : NULL;

  /* Navigation - update widget and sync to TuiState */
  if (hotkey_matches(cfg, event, HOTKEY_MOVE_UP)) {
    size_t highlight = sw ? sw->base.state.cursor_row : state->sidebar_highlight;
    if (highlight > 0) {
      if (sw) {
        widget_move_cursor(&sw->base, -1, 0);
        sync_state_from_widget(state, sw);
      } else {
        state->sidebar_highlight--;
      }
    } else {
      /* At top of list, move to filter field */
      state->sidebar_filter_active = true;
    }
  } else if (hotkey_matches(cfg, event, HOTKEY_MOVE_DOWN)) {
    size_t highlight = sw ? sw->base.state.cursor_row : state->sidebar_highlight;
    if (filtered_count > 0 && highlight < filtered_count - 1) {
      if (sw) {
        widget_move_cursor(&sw->base, 1, 0);
        sync_state_from_widget(state, sw);
      } else {
        state->sidebar_highlight++;
      }
    }
  } else if (hotkey_matches(cfg, event, HOTKEY_MOVE_RIGHT)) {
    /* Connection tab requires sidebar to stay focused */
    Tab *right_tab = TUI_TAB(state);
    if (right_tab && right_tab->type == TAB_TYPE_CONNECTION) {
      return true; /* Consume event but don't unfocus */
    }
    /* Save sidebar position and move focus back to filters or table view */
    state->sidebar_last_position = tui_sidebar_highlight(state);
    tui_set_sidebar_focused(state, false);
    if (tui_filters_visible(state) && state->filters_was_focused) {
      tui_set_filters_focused(state, true);
    }
  }
  /* Select table */
  else if (render_event_is_special(event, UI_KEY_ENTER)) {
    /* Select the highlighted table - open in current tab or create first tab */
    if (filtered_count > 0 && state->sidebar_highlight < filtered_count) {
      size_t actual_idx =
          tui_get_filtered_table_index(state, state->sidebar_highlight);

      Workspace *ws = TUI_WORKSPACE(state);
      if (!ws || ws->num_tabs == 0) {
        /* No tabs yet - create first one */
        tab_create(state, actual_idx);
      } else {
        Tab *tab = TUI_TAB(state);
        if (tab && tab->type == TAB_TYPE_QUERY) {
          /* Query tab active - check if table tab already exists for same
           * connection */
          bool found = false;
          size_t current_conn = tab->connection_index;
          for (size_t i = 0; i < ws->num_tabs; i++) {
            Tab *t = &ws->tabs[i];
            if (t->type == TAB_TYPE_TABLE && t->table_index == actual_idx &&
                t->connection_index == current_conn) {
              /* Found existing tab for same table and connection - switch to it
               */
              tab_switch(state, i);
              found = true;
              break;
            }
          }
          if (!found) {
            /* No existing tab - create new one */
            tab_create(state, actual_idx);
          }
        } else if (tab && tab->type == TAB_TYPE_CONNECTION) {
          /* Connection tab active - convert to table tab */
          char **tables = TUI_TABLES(state);
          free(tab->table_name);
          tab->table_name = tables ? str_dup(tables[actual_idx]) : NULL;
          tab->type = TAB_TYPE_TABLE;
          tab->table_index = actual_idx;

          /* Load table - tui_load_table_data handles Tab data */
          if (tables)
            tui_load_table_data(state, tables[actual_idx]);

          /* Initialize widgets for the converted tab */
          UITabState *ui = TUI_TAB_UI(state);
          if (ui) {
            tui_init_table_tab_widgets(state, ui, tab);
          }
        } else if (tab && actual_idx != tab->table_index) {
          /* Replace current tab's table */
          char **tables = TUI_TABLES(state);
          /* Copy new name first to avoid use-after-free if aliased */
          char *new_name = tables ? str_dup(tables[actual_idx]) : NULL;

          /* Free old data - NULL out pointers first to avoid dangling refs */
          ResultSet *old_data = tab->data;
          TableSchema *old_schema = tab->schema;
          tab->data = NULL;
          tab->schema = NULL;
          db_result_free(old_data);
          db_schema_free(old_schema);
          free(tab->col_widths);
          tab->col_widths = NULL;
          free(tab->table_name);

          /* Update tab */
          tab->table_index = actual_idx;
          tab->table_name = new_name;

          /* Clear filters when switching to a different table */
          filters_clear(&tab->filters);

          /* Load new table - tui_load_table_data handles Tab data */
          if (tables)
            tui_load_table_data(state, tables[actual_idx]);
          /* Note: tui_load_table_data rebinds FiltersWidget with new schema */
        }
      }
      /* Save position before leaving sidebar */
      state->sidebar_last_position = tui_sidebar_highlight(state);
      tui_set_sidebar_focused(state, false);
    }
  }
  /* Open in new tab */
  else if (hotkey_matches(cfg, event, HOTKEY_NEW_TAB)) {
    if (filtered_count > 0 && tui_sidebar_highlight(state) < filtered_count) {
      size_t actual_idx =
          tui_get_filtered_table_index(state, tui_sidebar_highlight(state));
      tab_create(state, actual_idx);
      /* Save position before leaving sidebar */
      state->sidebar_last_position = tui_sidebar_highlight(state);
      tui_set_sidebar_focused(state, false);
    }
  }
  /* Activate filter */
  else if (hotkey_matches(cfg, event, HOTKEY_SIDEBAR_FILTER)) {
    state->sidebar_filter_active = true;
  }
  /* c - copy sidebar filter to clipboard */
  else if (hotkey_matches(cfg, event, HOTKEY_CELL_COPY)) {
    if (state->sidebar_filter_len > 0) {
      tui_clipboard_copy(state, state->sidebar_filter);
      tui_set_status(state, "Copied to clipboard");
    }
  }
  /* v - paste into sidebar filter */
  else if (hotkey_matches(cfg, event, HOTKEY_CELL_PASTE)) {
    char *paste_text = tui_clipboard_read(state);
    if (paste_text && paste_text[0]) {
      /* Strip newlines for single-line filter */
      for (char *p = paste_text; *p; p++) {
        if (*p == '\n' || *p == '\r')
          *p = ' ';
      }
      size_t paste_len = strlen(paste_text);
      if (paste_len >= sizeof(state->sidebar_filter))
        paste_len = sizeof(state->sidebar_filter) - 1;
      memcpy(state->sidebar_filter, paste_text, paste_len);
      state->sidebar_filter[paste_len] = '\0';
      state->sidebar_filter_len = paste_len;
      tui_set_sidebar_highlight(state, 0);
      tui_set_sidebar_scroll(state, 0);
      tui_set_status(state, "Pasted from clipboard");
    }
    free(paste_text);
  }
  /* Escape - clear filter or unfocus */
  else if (render_event_is_special(event, UI_KEY_ESCAPE)) {
    size_t filter_len = sw ? sw->filter_len : state->sidebar_filter_len;
    if (filter_len > 0) {
      /* Clear filter but keep highlight position */
      if (sw) {
        sidebar_widget_filter_clear(sw);
        sync_state_from_widget(state, sw);
      } else {
        state->sidebar_filter[0] = '\0';
        state->sidebar_filter_len = 0;
      }
      /* Don't reset highlight - user may want to stay at current position */
    } else {
      /* Connection tab requires sidebar to stay focused */
      Tab *esc_tab = TUI_TAB(state);
      if (esc_tab && esc_tab->type == TAB_TYPE_CONNECTION) {
        return true; /* Consume event but don't unfocus */
      }
      /* Save position before leaving sidebar */
      state->sidebar_last_position = tui_sidebar_highlight(state);
      tui_set_sidebar_focused(state, false);
    }
  }
  /* Toggle sidebar (close) */
  else if (hotkey_matches(cfg, event, HOTKEY_TOGGLE_SIDEBAR)) {
    /* Connection tab requires sidebar to stay visible */
    Tab *toggle_tab = TUI_TAB(state);
    if (toggle_tab && toggle_tab->type == TAB_TYPE_CONNECTION) {
      return true; /* Consume event but don't hide */
    }
    tui_set_sidebar_visible(state, false);
    tui_set_sidebar_focused(state, false);
    state->sidebar_filter[0] = '\0';
    state->sidebar_filter_len = 0;
    tui_recreate_windows(state);
    tui_calculate_column_widths(state);
  }
  /* Help opens config dialog on hotkeys tab */
  else if (hotkey_matches(cfg, event, HOTKEY_HELP)) {
    config_view_show_tab(state, CONFIG_TAB_HOTKEYS);
    tui_refresh(state);
  }
  /* Pass through global hotkeys to main handler */
  else if (hotkey_matches(cfg, event, HOTKEY_QUIT) ||
           hotkey_matches(cfg, event, HOTKEY_OPEN_QUERY) ||
           hotkey_matches(cfg, event, HOTKEY_REFRESH) ||
           hotkey_matches(cfg, event, HOTKEY_PREV_TAB) ||
           hotkey_matches(cfg, event, HOTKEY_NEXT_TAB) ||
           hotkey_matches(cfg, event, HOTKEY_PREV_WORKSPACE) ||
           hotkey_matches(cfg, event, HOTKEY_NEXT_WORKSPACE) ||
           hotkey_matches(cfg, event, HOTKEY_CLOSE_TAB) ||
           hotkey_matches(cfg, event, HOTKEY_SHOW_SCHEMA) ||
           hotkey_matches(cfg, event, HOTKEY_CONNECT_DIALOG) ||
           hotkey_matches(cfg, event, HOTKEY_TOGGLE_HEADER) ||
           hotkey_matches(cfg, event, HOTKEY_TOGGLE_STATUS) ||
           hotkey_matches(cfg, event, HOTKEY_GOTO_ROW) ||
           hotkey_matches(cfg, event, HOTKEY_CONFIG)) {
    return false;
  }

  /* Sync focus state to Tab so it persists across tab switches */
  tab_sync_focus(state);
  return true;
}

/* Draw sidebar */
void tui_draw_sidebar(TuiState *state) {
  if (!state || !state->sidebar_win || !state->sidebar_visible)
    return;

  /* Get sidebar widget for reading state */
  SidebarWidget *sw = tui_sidebar_widget(state);

  /* Use widget state if available, otherwise fall back to TuiState */
  const char *filter_text = sw ? sw->filter : state->sidebar_filter;
  size_t filter_len = sw ? sw->filter_len : state->sidebar_filter_len;
  size_t highlight = sw ? sw->base.state.cursor_row : state->sidebar_highlight;
  size_t scroll = sw ? sw->base.state.scroll_row : state->sidebar_scroll;

  werase(state->sidebar_win);

  /* Draw border and title */
  DRAW_BOX(state->sidebar_win, COLOR_BORDER);
  WITH_ATTR(state->sidebar_win, A_BOLD,
            mvwprintw(state->sidebar_win, 0, 2, " Tables "));

  int y = 1;
  int max_name_len = SIDEBAR_WIDTH - 4;

  /* Draw filter input - use COLOR_EDIT for active filter (black on yellow) */
  if (state->sidebar_filter_active) {
    wattron(state->sidebar_win, COLOR_PAIR(COLOR_EDIT));
  }
  mvwprintw(state->sidebar_win, y, 1, "/%-*.*s", max_name_len, max_name_len,
            filter_len > 0 ? filter_text : "");
  if (state->sidebar_filter_active) {
    wattroff(state->sidebar_win, COLOR_PAIR(COLOR_EDIT));
  }
  int filter_y = y; /* Remember filter line position for cursor */
  y++;

  /* Separator with T-junctions connecting to borders */
  wattron(state->sidebar_win, COLOR_PAIR(COLOR_BORDER));
  mvwaddch(state->sidebar_win, y, 0, ACS_LTEE);
  mvwhline(state->sidebar_win, y, 1, ACS_HLINE, SIDEBAR_WIDTH - 2);
  mvwaddch(state->sidebar_win, y, SIDEBAR_WIDTH - 1, ACS_RTEE);
  wattroff(state->sidebar_win, COLOR_PAIR(COLOR_BORDER));
  y++;

  char **tables = TUI_TABLES(state);
  size_t num_tables = TUI_NUM_TABLES(state);
  Tab *current_tab = TUI_TAB(state);

  if (!tables || num_tables == 0) {
    mvwprintw(state->sidebar_win, y, 2, "(no tables)");
    /* Position cursor in filter field if active */
    if (state->sidebar_filter_active) {
      curs_set(1);
      wmove(state->sidebar_win, filter_y, 2 + (int)filter_len);
    }
    wrefresh(state->sidebar_win);
    return;
  }

  /* Get actual sidebar window height */
  int win_height, win_width;
  getmaxyx(state->sidebar_win, win_height, win_width);
  (void)win_width;

  /* Content area height: window height minus top border(1), filter(1),
   * separator(1), bottom border(1) */
  int list_height = win_height - 4;

  /* Count filtered tables - use widget if available */
  size_t filtered_count = sw ? sidebar_widget_count(sw) : 0;
  if (!sw) {
    for (size_t i = 0; i < num_tables; i++) {
      if (tables[i] &&
          (filter_len == 0 || tui_str_istr(tables[i], filter_text))) {
        filtered_count++;
      }
    }
  }

  if (filtered_count == 0) {
    mvwprintw(state->sidebar_win, y, 2, "(no matches)");
    /* Position cursor in filter field if active */
    if (state->sidebar_filter_active) {
      curs_set(1);
      wmove(state->sidebar_win, filter_y, 2 + (int)filter_len);
    }
    wrefresh(state->sidebar_win);
    return;
  }

  /* Adjust scroll if highlight is out of view (update widget if present) */
  size_t draw_scroll = scroll;
  if (highlight < draw_scroll) {
    draw_scroll = highlight;
  } else if (highlight >= draw_scroll + (size_t)list_height) {
    draw_scroll = highlight - (size_t)list_height + 1;
  }
  /* Sync adjusted scroll back to state/widget for next frame */
  if (draw_scroll != scroll) {
    if (sw) {
      sw->base.state.scroll_row = draw_scroll;
    }
    state->sidebar_scroll = draw_scroll;
  }

  /* Draw filtered tables */
  size_t filtered_idx = 0;
  size_t current_table_idx = current_tab ? current_tab->table_index : SIZE_MAX;
  for (size_t i = 0; i < num_tables && y < win_height - 1; i++) {
    const char *name = tables[i];
    if (!name)
      continue;

    /* Apply filter */
    if (filter_len > 0 && !tui_str_istr(name, filter_text)) {
      continue;
    }

    /* Skip items before scroll offset */
    if (filtered_idx < draw_scroll) {
      filtered_idx++;
      continue;
    }

    bool is_highlighted = (filtered_idx == highlight);
    bool is_current = (i == current_table_idx);

    if (is_highlighted && state->sidebar_focused &&
        !state->sidebar_filter_active) {
      wattron(state->sidebar_win, A_REVERSE);
    }

    /* Truncate name if too long */
    char display_name[SIDEBAR_WIDTH];
    int name_len = (int)strlen(name);
    if (name_len > max_name_len) {
      if (is_highlighted && state->sidebar_focused &&
          !state->sidebar_filter_active) {
        /* Apply scroll animation for highlighted item */
        size_t name_scroll = state->sidebar_name_scroll;
        if (name_scroll > (size_t)(name_len - max_name_len)) {
          name_scroll = (size_t)(name_len - max_name_len);
        }
        snprintf(display_name, sizeof(display_name), "%.*s", max_name_len,
                 name + name_scroll);
      } else {
        snprintf(display_name, sizeof(display_name), "%.*s..", max_name_len - 2,
                 name);
      }
    } else {
      snprintf(display_name, sizeof(display_name), "%s", name);
    }

    if (is_current) {
      wattron(state->sidebar_win, A_BOLD);
    }

    mvwprintw(state->sidebar_win, y, 2, "%-*s", max_name_len, display_name);

    if (is_current) {
      wattroff(state->sidebar_win, A_BOLD);
    }
    if (is_highlighted && state->sidebar_focused &&
        !state->sidebar_filter_active) {
      wattroff(state->sidebar_win, A_REVERSE);
    }

    y++;
    filtered_idx++;
  }

  /* Position cursor in filter field if active, before refresh */
  if (state->sidebar_filter_active) {
    curs_set(1);
    wmove(state->sidebar_win, filter_y, 2 + (int)filter_len);
  }

  wrefresh(state->sidebar_win);
}
