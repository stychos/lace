/*
 * Lace
 * Sidebar rendering and input handling
 *
 * Sidebar state is stored directly in TuiState.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/config.h"
#include "tui_internal.h"
#include "util/mem.h"
#include "views/config_view.h"
#include <stdlib.h>
#include <string.h>

/* The sidebar's entries after the search text: the tables, then each
 * object section that has a match (a header, foldable, then its entries) */
static bool name_matches(TuiState *state, const char *name) {
  return name && (state->sidebar_filter_len == 0 || tui_str_istr(name, state->sidebar_filter));
}

static int section_of(LaceObjectKind k) {
  return k == LACE_OBJ_VIEW ? 1 : k == LACE_OBJ_ROUTINE ? 2 : k == LACE_OBJ_TRIGGER ? 3 : 0;
}

static const char *const SECTION_TITLE[] = {"TABLES", "VIEWS", "FUNCTIONS", "TRIGGERS"};

size_t tui_sidebar_entries(TuiState *state, SidebarEntry **out) {
  *out = NULL;
  if (!state)
    return 0;
  char **tables = TUI_TABLES(state);
  size_t num_tables = TUI_NUM_TABLES(state);
  Connection *conn = TUI_TAB_CONNECTION(state);
  size_t no = conn ? conn->num_objects : 0;
  const LaceObject *objs = conn ? conn->objects : NULL;
  SidebarEntry *e = safe_calloc(num_tables + no + 4, sizeof *e);
  size_t n = 0;
  for (size_t i = 0; i < num_tables; i++)
    if (name_matches(state, tables[i]))
      e[n++] = (SidebarEntry){SB_TABLE, i, 0};
  for (int sec = 1; sec <= 3; sec++) {
    bool any = false;
    for (size_t i = 0; i < no; i++)
      if (section_of(objs[i].kind) == sec && name_matches(state, objs[i].name))
        any = true;
    if (!any)
      continue;
    e[n++] = (SidebarEntry){SB_HEADER, 0, sec};
    if (state->sidebar_collapsed & (1u << sec))
      continue;
    for (size_t i = 0; i < no; i++)
      if (section_of(objs[i].kind) == sec && name_matches(state, objs[i].name))
        e[n++] = (SidebarEntry){SB_OBJECT, i, sec};
  }
  *out = e;
  return n;
}

bool tui_sidebar_entry_at(TuiState *state, size_t i, SidebarEntry *out) {
  SidebarEntry *e;
  size_t n = tui_sidebar_entries(state, &e);
  bool ok = i < n;
  if (ok)
    *out = e[i];
  free(e);
  return ok;
}

const char *tui_sidebar_entry_name(TuiState *state, const SidebarEntry *e) {
  if (e->kind == SB_HEADER)
    return SECTION_TITLE[e->section];
  if (e->kind == SB_TABLE) {
    char **tables = TUI_TABLES(state);
    return e->idx < TUI_NUM_TABLES(state) ? tables[e->idx] : "";
  }
  Connection *conn = TUI_TAB_CONNECTION(state);
  return conn && e->idx < conn->num_objects && conn->objects[e->idx].name ? conn->objects[e->idx].name : "";
}

/* Count the entries (tables, section headers, objects) after the search */
size_t tui_count_filtered_tables(TuiState *state) {
  SidebarEntry *e;
  size_t n = tui_sidebar_entries(state, &e);
  free(e);
  return n;
}

/* The table index of entry filtered_idx; SIZE_MAX when it is not a table */
size_t tui_get_filtered_table_index(TuiState *state, size_t filtered_idx) {
  SidebarEntry e;
  if (!tui_sidebar_entry_at(state, filtered_idx, &e) || e.kind != SB_TABLE)
    return SIZE_MAX;
  return e.idx;
}

/* The entry of a table index (0 when it is not listed) */
size_t tui_get_sidebar_highlight_for_table(TuiState *state, size_t table_idx) {
  SidebarEntry *e;
  size_t n = tui_sidebar_entries(state, &e);
  size_t at = 0;
  for (size_t i = 0; i < n; i++)
    if (e[i].kind == SB_TABLE && e[i].idx == table_idx) {
      at = i;
      break;
    }
  free(e);
  return at;
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
  int name_len = utf8_width(name, strlen(name)); /* In columns */

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
/* Open table actual_idx (index into the connection's tables) from the
 * sidebar, for both Enter and a mouse click: switch to its tab from a query
 * tab, turn a connection tab into a table tab, or replace the current tab's
 * table. Filters and sort refer to columns by index, so they are cleared when
 * the table changes. */
/* The current tab shows the object (a table, view, routine or trigger) in
 * place: a query tab switches to an open tab of it or creates one, a
 * connection tab becomes it, a table tab replaces what it shows */
static void open_in_current(TuiState *state, LaceObjectKind kind, const char *name, const char *table,
                            const char *detail) {
  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws || ws->num_tabs == 0) {
    tab_create_object(state, kind, name, table, detail);
    return;
  }
  Tab *tab = TUI_TAB(state);
  bool relation = kind == LACE_OBJ_TABLE || kind == LACE_OBJ_VIEW;
  if (tab && tab->type == TAB_TYPE_QUERY) {
    /* Query tab active: an open tab of the same object on this connection, else a new one */
    size_t current_conn = tab->connection_index;
    for (size_t i = 0; i < ws->num_tabs; i++) {
      Tab *t = &ws->tabs[i];
      bool t_relation = t->object == LACE_OBJ_TABLE || t->object == LACE_OBJ_VIEW;
      if (t->type == TAB_TYPE_TABLE && t->table_name && str_eq(t->table_name, name) &&
          (relation ? t_relation : t->object == kind) && t->connection_index == current_conn) {
        tab_switch(state, i);
        return;
      }
    }
    tab_create_object(state, kind, name, table, detail);
    return;
  }
  if (!tab)
    return;
  bool same = tab->type == TAB_TYPE_TABLE && tab->table_name && str_eq(tab->table_name, name) &&
              (relation ? (tab->object == LACE_OBJ_TABLE || tab->object == LACE_OBJ_VIEW) : tab->object == kind);
  if (same)
    return;
  char *new_name = str_dup(name); /* copies first: name may live in what is freed below */
  char *new_table = table ? str_dup(table) : NULL, *new_detail = detail ? str_dup(detail) : NULL;
  undo_stack_clear(&tab->undo); /* another object: its own undo */
  if (tab->type == TAB_TYPE_CONNECTION) {
    tab->type = TAB_TYPE_TABLE;
  } else {
    /* Free old data - NULL out pointers first to avoid dangling refs */
    ResultSet *old_data = tab->data;
    TableSchema *old_schema = tab->schema;
    tab->data = NULL;
    tab->schema = NULL;
    tab_data_replaced(tab);
    db_result_free(old_data);
    db_schema_free(old_schema);
    free(tab->col_widths);
    tab->col_widths = NULL;
    /* Clear filters and sort when switching to a different table:
     * both refer to columns of the old table by index */
    filters_clear(&tab->filters);
    tab->num_sort_entries = 0;
  }
  free(tab->table_name);
  free(tab->object_table);
  free(tab->object_detail);
  tab->table_name = new_name;
  tab->object = kind;
  tab->object_table = new_table;
  tab->object_detail = new_detail;
  tab->subview = relation ? SUBVIEW_DATA : SUBVIEW_STRUCTURE;
  memset(tab->sv_cursor, 0, sizeof tab->sv_cursor);
  memset(tab->sv_scroll, 0, sizeof tab->sv_scroll);
  tui_load_table_data(state, new_name);
}

void sidebar_open_entry(TuiState *state, const SidebarEntry *e, bool new_tab) {
  if (e->kind == SB_HEADER) { /* fold or unfold the section */
    state->sidebar_collapsed ^= 1u << e->section;
    return;
  }
  if (e->kind == SB_TABLE) {
    char **tables = TUI_TABLES(state);
    if (!tables || e->idx >= TUI_NUM_TABLES(state))
      return;
    char *name = str_dup(tables[e->idx]); /* a refresh may replace the list */
    if (new_tab)
      tab_create_object(state, LACE_OBJ_TABLE, name, NULL, NULL);
    else
      open_in_current(state, LACE_OBJ_TABLE, name, NULL, NULL);
    free(name);
    return;
  }
  Connection *conn = TUI_TAB_CONNECTION(state);
  if (!conn || e->idx >= conn->num_objects)
    return;
  const LaceObject *o = &conn->objects[e->idx];
  LaceObjectKind kind = o->kind;
  char *name = str_dup(o->name), *table = o->table ? str_dup(o->table) : NULL;
  char *detail = o->detail ? str_dup(o->detail) : NULL;
  if (new_tab)
    tab_create_object(state, kind, name, table, detail);
  else
    open_in_current(state, kind, name, table, detail);
  free(name);
  free(table);
  free(detail);
}

void sidebar_open_table(TuiState *state, size_t actual_idx) {
  SidebarEntry e = {SB_TABLE, actual_idx, 0};
  sidebar_open_entry(state, &e, false);
}

bool tui_handle_sidebar_input(TuiState *state, const UiEvent *event) {
  if (!state->sidebar_focused || !event || event->type != UI_EVENT_KEY)
    return false;

  int key_char = render_event_get_char(event);

  /* Handle filter input mode */
  if (state->sidebar_filter_active) {
    if (render_event_is_special(event, UI_KEY_ESCAPE)) {
      state->sidebar_filter_active = false;
    } else if (render_event_is_special(event, UI_KEY_ENTER) ||
               render_event_is_special(event, UI_KEY_DOWN)) {
      state->sidebar_filter_active = false;
      state->sidebar_highlight = 0;
      state->sidebar_scroll = 0;
    } else if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
      if (state->sidebar_filter_len > 0) {
        state->sidebar_filter_len =
            utf8_prev(state->sidebar_filter, state->sidebar_filter_len);
        state->sidebar_filter[state->sidebar_filter_len] = '\0';
        state->sidebar_highlight = 0;
        state->sidebar_scroll = 0;
      }
    } else if (render_event_is_ctrl(event, 'K')) {
      /* Ctrl+K - copy filter text to clipboard */
      if (state->sidebar_filter_len > 0) {
        tui_clipboard_copy(state, state->sidebar_filter);
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
        size_t paste_len = strlen(paste_text);
        size_t space_left =
            sizeof(state->sidebar_filter) - 1 - state->sidebar_filter_len;
        if (paste_len > space_left)
          paste_len = utf8_truncate(paste_text, space_left);
        if (paste_len > 0) {
          memcpy(state->sidebar_filter + state->sidebar_filter_len, paste_text,
                 paste_len);
          state->sidebar_filter_len += paste_len;
          state->sidebar_filter[state->sidebar_filter_len] = '\0';
          state->sidebar_highlight = 0;
          state->sidebar_scroll = 0;
        }
      }
      free(paste_text);
    } else if (render_event_is_char(event)) {
      /* Printable character - add to filter */
      /* A lead byte needs room for its whole character */
      size_t need = utf8_seq_len((unsigned char)key_char);
      if (state->sidebar_filter_len + need < sizeof(state->sidebar_filter)) {
        state->sidebar_filter[state->sidebar_filter_len++] = (char)key_char;
        state->sidebar_filter[state->sidebar_filter_len] = '\0';
        state->sidebar_highlight = 0;
        state->sidebar_scroll = 0;
      }
    }
    return true;
  }

  size_t filtered_count = tui_count_filtered_tables(state);
  const Config *cfg = state->app ? state->app->config : NULL;

  /* Navigation */
  if (hotkey_matches(cfg, event, HOTKEY_MOVE_UP)) {
    if (state->sidebar_highlight > 0) {
      state->sidebar_highlight--;
    } else {
      /* At top of list, move to filter field */
      state->sidebar_filter_active = true;
    }
  } else if (hotkey_matches(cfg, event, HOTKEY_MOVE_DOWN)) {
    if (filtered_count > 0 && state->sidebar_highlight < filtered_count - 1) {
      state->sidebar_highlight++;
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
  /* Select the entry: open in the current tab, create the first tab; a
   * section header folds */
  else if (render_event_is_special(event, UI_KEY_ENTER)) {
    SidebarEntry e;
    if (tui_sidebar_entry_at(state, state->sidebar_highlight, &e)) {
      sidebar_open_entry(state, &e, false);
      if (e.kind != SB_HEADER) {
        /* Save position before leaving sidebar */
        state->sidebar_last_position = tui_sidebar_highlight(state);
        tui_set_sidebar_focused(state, false);
      }
    }
  }
  /* Open in new tab */
  else if (hotkey_matches(cfg, event, HOTKEY_NEW_TAB)) {
    SidebarEntry e;
    if (tui_sidebar_entry_at(state, tui_sidebar_highlight(state), &e) && e.kind != SB_HEADER) {
      sidebar_open_entry(state, &e, true);
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
        paste_len =
            utf8_truncate(paste_text, sizeof(state->sidebar_filter) - 1);
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
    if (state->sidebar_filter_len > 0) {
      /* Clear filter but keep highlight position */
      state->sidebar_filter[0] = '\0';
      state->sidebar_filter_len = 0;
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
           hotkey_matches(cfg, event, HOTKEY_CONNECT_DIALOG) ||
           hotkey_matches(cfg, event, HOTKEY_NEW_CONNECTION) ||
           hotkey_matches(cfg, event, HOTKEY_TOGGLE_HEADER) ||
           hotkey_matches(cfg, event, HOTKEY_TOGGLE_STATUS) ||
           hotkey_matches(cfg, event, HOTKEY_GOTO_ROW) ||
           hotkey_matches(cfg, event, HOTKEY_CONFIG)) {
    return false;
  }

  return true;
}

/* Draw sidebar */
void tui_draw_sidebar(TuiState *state) {
  if (!state || !state->sidebar_win || !state->sidebar_visible)
    return;

  const char *filter_text = state->sidebar_filter;
  size_t filter_len = state->sidebar_filter_len;
  size_t highlight = state->sidebar_highlight;
  size_t scroll = state->sidebar_scroll;

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
  mvwaddch(state->sidebar_win, y, 1, '/');
  tui_print_fit(state->sidebar_win, y, 2, max_name_len,
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

  Tab *current_tab = TUI_TAB(state);
  SidebarEntry *entries;
  size_t n = tui_sidebar_entries(state, &entries);

  if (TUI_NUM_TABLES(state) == 0 && n == 0) {
    mvwprintw(state->sidebar_win, y, 2, "(no tables)");
    /* Position cursor in filter field if active */
    if (state->sidebar_filter_active) {
      curs_set(1);
      wmove(state->sidebar_win, filter_y,
            2 + utf8_width(filter_text, filter_len));
    }
    free(entries);
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

  if (n == 0) {
    mvwprintw(state->sidebar_win, y, 2, "(no matches)");
    /* Position cursor in filter field if active */
    if (state->sidebar_filter_active) {
      curs_set(1);
      wmove(state->sidebar_win, filter_y,
            2 + utf8_width(filter_text, filter_len));
    }
    free(entries);
    wrefresh(state->sidebar_win);
    return;
  }

  /* Adjust scroll if highlight is out of view */
  size_t draw_scroll = scroll;
  if (highlight < draw_scroll) {
    draw_scroll = highlight;
  } else if (highlight >= draw_scroll + (size_t)list_height) {
    draw_scroll = highlight - (size_t)list_height + 1;
  }
  /* Sync adjusted scroll back to state for next frame */
  if (draw_scroll != scroll) {
    state->sidebar_scroll = draw_scroll;
  }

  /* Draw the entries from the scroll position */
  for (size_t i = draw_scroll; i < n && y < win_height - 1; i++) {
    const SidebarEntry *e = &entries[i];
    bool is_highlighted = (i == highlight);
    bool focused_hl = is_highlighted && state->sidebar_focused && !state->sidebar_filter_active;
    if (e->kind == SB_HEADER) { /* a section: "▾ VIEWS", "▸ VIEWS" when folded */
      char *title = str_printf("%s %s", state->sidebar_collapsed & (1u << e->section) ? "\xe2\x96\xb8" : "\xe2\x96\xbe",
                               tui_sidebar_entry_name(state, e));
      attr_t attr = A_BOLD | (focused_hl ? A_REVERSE : A_DIM);
      wattron(state->sidebar_win, attr);
      tui_print_fit(state->sidebar_win, y, 1, max_name_len + 1, title);
      wattroff(state->sidebar_win, attr);
      free(title);
      y++;
      continue;
    }
    const char *name = tui_sidebar_entry_name(state, e);
    /* the current tab's entry: the same object */
    bool is_current = current_tab && current_tab->type == TAB_TYPE_TABLE && current_tab->table_name &&
                      str_eq(current_tab->table_name, name) &&
                      (e->kind == SB_TABLE ? current_tab->object == LACE_OBJ_TABLE || current_tab->object == LACE_OBJ_VIEW
                                           : section_of(current_tab->object) == e->section);

    if (focused_hl) {
      wattron(state->sidebar_win, A_REVERSE);
    }

    /* Truncate name if too long (by display columns, never mid-character) */
    size_t name_bytes = strlen(name);
    int name_len = utf8_width(name, name_bytes);
    char *display_name = NULL;
    if (name_len > max_name_len) {
      if (focused_hl) {
        /* Apply scroll animation for highlighted item (scroll is columns) */
        size_t name_scroll = state->sidebar_name_scroll;
        if (name_scroll > (size_t)(name_len - max_name_len)) {
          name_scroll = (size_t)(name_len - max_name_len);
        }
        display_name =
            str_dup(name + utf8_offset_at_col(name, name_bytes, (int)name_scroll));
      } else {
        int cols = 0;
        size_t keep = utf8_fit(name, name_bytes, max_name_len - 2, &cols);
        display_name = str_printf("%.*s..", (int)keep, name);
      }
    } else {
      display_name = str_dup(name);
    }

    if (is_current) {
      wattron(state->sidebar_win, A_BOLD);
    }

    tui_print_fit(state->sidebar_win, y, 2, max_name_len, display_name);
    free(display_name);

    if (is_current) {
      wattroff(state->sidebar_win, A_BOLD);
    }
    if (focused_hl) {
      wattroff(state->sidebar_win, A_REVERSE);
    }

    y++;
  }
  free(entries);

  /* Position cursor in filter field if active, before refresh */
  if (state->sidebar_filter_active) {
    curs_set(1);
    wmove(state->sidebar_win, filter_y,
            2 + utf8_width(filter_text, filter_len));
  }

  wrefresh(state->sidebar_win);
}
