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
#include "views/theme.h"
#include "core/chrome.h"
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

static const char *const SECTION_TITLE[] = {"TABLES", "VIEWS", "FUNCTIONS", "TRIGGERS", "QUERIES"};

#define SIDEBAR_MAX 28
#define SIDEBAR_MIN 10

int tui_sidebar_width(TuiState *state) {
  if (!state || !tui_sidebar_shown(state))
    return 0;
  int w = state->term_cols / 3 < SIDEBAR_MAX ? state->term_cols / 3 : SIDEBAR_MAX;
  return w < SIDEBAR_MIN ? 0 : w;
}

/* A query tab's title (valid until the next call) */
static const char *query_title(const Tab *t) {
  static char buf[256];
  return chrome_query_title(t->query_text, buf, sizeof buf);
}

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
  /* QUERIES: the connection's query tabs here */
  Workspace *ws = TUI_WORKSPACE(state);
  Tab *cur = TUI_TAB(state);
  size_t nq = 0;
  for (size_t i = 0; ws && cur && i < ws->num_tabs; i++) {
    Tab *t = &ws->tabs[i];
    if (!t->active || t->type != TAB_TYPE_QUERY || t->connection_index != cur->connection_index ||
        !name_matches(state, query_title(t)))
      continue;
    if (!nq++) {
      e = safe_realloc(e, (n + 1 + ws->num_tabs + 1) * sizeof *e);
      e[n++] = (SidebarEntry){SB_HEADER, 0, 4};
    }
    if (!(state->sidebar_collapsed & (1u << 4)))
      e[n++] = (SidebarEntry){SB_QUERY, i, 4};
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
  if (e->kind == SB_QUERY) {
    Workspace *ws = TUI_WORKSPACE(state);
    return ws && e->idx < ws->num_tabs ? query_title(&ws->tabs[e->idx]) : "";
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

  int max_name_len = tui_sidebar_width(state) - 2;
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
    tab_clear_user_widths(tab); /* other columns */
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

/* The workspace's tab showing the object on the current connection
 * (tables and database views share a namespace); SIZE_MAX: none */
static size_t object_tab(TuiState *state, LaceObjectKind kind, const char *name) {
  Workspace *ws = TUI_WORKSPACE(state);
  Tab *cur = TUI_TAB(state);
  if (!ws || !cur)
    return SIZE_MAX;
  bool relation = kind == LACE_OBJ_TABLE || kind == LACE_OBJ_VIEW;
  for (size_t i = 0; i < ws->num_tabs; i++) {
    Tab *t = &ws->tabs[i];
    bool t_relation = t->object == LACE_OBJ_TABLE || t->object == LACE_OBJ_VIEW;
    if (t->active && t->type == TAB_TYPE_TABLE && t->table_name && str_eq(t->table_name, name) &&
        (relation ? t_relation : t->object == kind) && t->connection_index == cur->connection_index)
      return i;
  }
  return SIZE_MAX;
}

/* The connection's preview tab here; SIZE_MAX: none */
static size_t preview_tab(TuiState *state) {
  Workspace *ws = TUI_WORKSPACE(state);
  Tab *cur = TUI_TAB(state);
  for (size_t i = 0; ws && cur && i < ws->num_tabs; i++)
    if (ws->tabs[i].active && ws->tabs[i].preview && ws->tabs[i].type == TAB_TYPE_TABLE &&
        ws->tabs[i].connection_index == cur->connection_index)
      return i;
  return SIZE_MAX;
}

/* Open for keeps, as the notcurses TUI (Enter, a double click, the
 * palette): an open tab of the object is switched to and kept, the
 * connection's own tab takes its first view, else a new tab */
static void open_for_keeps(TuiState *state, LaceObjectKind kind, const char *name, const char *table,
                           const char *detail) {
  Workspace *ws = TUI_WORKSPACE(state);
  size_t i = object_tab(state, kind, name);
  if (i != SIZE_MAX) {
    if (i != ws->current_tab)
      tab_switch(state, i);
    ws->tabs[i].preview = false;
    return;
  }
  Tab *cur = TUI_TAB(state);
  if (!ws || ws->num_tabs == 0 || (cur && cur->type == TAB_TYPE_CONNECTION)) {
    open_in_current(state, kind, name, table, detail);
    return;
  }
  tab_create_object(state, kind, name, table, detail);
}

/* Show the object in the connection's preview tab (a single click): an open
 * tab of it is switched to as it is; the preview tab shows it instead of
 * what it showed; else the connection's own tab or a new tab becomes the
 * preview */
static void open_preview(TuiState *state, LaceObjectKind kind, const char *name, const char *table,
                         const char *detail) {
  Workspace *ws = TUI_WORKSPACE(state);
  size_t i = object_tab(state, kind, name);
  if (i != SIZE_MAX) {
    if (i != ws->current_tab)
      tab_switch(state, i);
    return;
  }
  size_t p = preview_tab(state);
  if (p != SIZE_MAX) {
    if (p != ws->current_tab)
      tab_switch(state, p);
    open_in_current(state, kind, name, table, detail);
    return;
  }
  Tab *cur = TUI_TAB(state);
  if (cur && cur->type == TAB_TYPE_CONNECTION)
    open_in_current(state, kind, name, table, detail);
  else
    tab_create_object(state, kind, name, table, detail);
  if (TUI_TAB(state))
    TUI_TAB(state)->preview = true;
}

/* What an entry opens; false for a header (folded) or a query tab (switched to) */
static bool entry_target(TuiState *state, const SidebarEntry *e, LaceObjectKind *kind, char **name, char **table,
                         char **detail) {
  if (e->kind == SB_HEADER) { /* fold or unfold the section (QUERIES does not) */
    if (e->section != 4)
      state->sidebar_collapsed ^= 1u << e->section;
    return false;
  }
  if (e->kind == SB_QUERY) { /* a query tab: switch to it */
    Workspace *ws = TUI_WORKSPACE(state);
    if (ws && e->idx < ws->num_tabs && e->idx != ws->current_tab)
      tab_switch(state, e->idx);
    return false;
  }
  *table = *detail = NULL;
  if (e->kind == SB_TABLE) {
    char **tables = TUI_TABLES(state);
    if (!tables || e->idx >= TUI_NUM_TABLES(state))
      return false;
    *kind = LACE_OBJ_TABLE;
    *name = str_dup(tables[e->idx]); /* a refresh may replace the list */
    return true;
  }
  Connection *conn = TUI_TAB_CONNECTION(state);
  if (!conn || e->idx >= conn->num_objects)
    return false;
  const LaceObject *o = &conn->objects[e->idx];
  *kind = o->kind;
  *name = str_dup(o->name);
  *table = o->table ? str_dup(o->table) : NULL;
  *detail = o->detail ? str_dup(o->detail) : NULL;
  return true;
}

void sidebar_open_entry(TuiState *state, const SidebarEntry *e, bool new_tab) {
  LaceObjectKind kind;
  char *name, *table, *detail;
  if (!entry_target(state, e, &kind, &name, &table, &detail))
    return;
  /* the connection's own tab (no view yet) takes the first view itself */
  Tab *cur = TUI_TAB(state);
  if (new_tab && cur && cur->type != TAB_TYPE_CONNECTION)
    tab_create_object(state, kind, name, table, detail);
  else
    open_for_keeps(state, kind, name, table, detail);
  free(name);
  free(table);
  free(detail);
}

void sidebar_preview_entry(TuiState *state, const SidebarEntry *e) {
  LaceObjectKind kind;
  char *name, *table, *detail;
  if (!entry_target(state, e, &kind, &name, &table, &detail))
    return;
  open_preview(state, kind, name, table, detail);
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
           hotkey_matches(cfg, event, HOTKEY_PALETTE) ||
           hotkey_matches(cfg, event, HOTKEY_TOGGLE_HISTORY) ||
           hotkey_matches(cfg, event, HOTKEY_CONFIG)) {
    return false;
  }

  return true;
}

/* The entry the current tab shows (SIZE_MAX: none) */
static size_t open_entry(TuiState *state, const SidebarEntry *e, size_t n) {
  Tab *t = TUI_TAB(state);
  Workspace *ws = TUI_WORKSPACE(state);
  if (!t || !ws)
    return SIZE_MAX;
  for (size_t i = 0; i < n; i++) {
    if (t->type == TAB_TYPE_QUERY && e[i].kind == SB_QUERY && e[i].idx == ws->current_tab)
      return i;
    if (t->type != TAB_TYPE_TABLE || !t->table_name || (e[i].kind != SB_TABLE && e[i].kind != SB_OBJECT))
      continue;
    bool kind = e[i].kind == SB_TABLE ? t->object == LACE_OBJ_TABLE || t->object == LACE_OBJ_VIEW
                                      : section_of(t->object) == e[i].section;
    if (kind && str_eq(tui_sidebar_entry_name(state, &e[i]), t->table_name))
      return i;
  }
  return SIZE_MAX;
}

/* Whether the search field shows: while it is used or holds text */
static bool search_shown(TuiState *state) { return state->sidebar_filter_active || state->sidebar_filter_len > 0; }

int tui_sidebar_list_top(TuiState *state) { return (search_shown(state) ? 1 : 0) + 1; }

/* A note right under TABLES while no table shows ("loading…", "none", "no match") */
static const char *note_of(TuiState *state, const SidebarEntry *e, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (e[i].kind == SB_TABLE)
      return NULL;
  if (!TUI_TABLES(state) && TUI_NUM_TABLES(state) == 0 && !TUI_TAB_CONNECTION(state))
    return "loading\xe2\x80\xa6";
  return state->sidebar_filter_len ? "no match" : "none";
}

bool tui_sidebar_has_note(TuiState *state) {
  SidebarEntry *e;
  size_t n = tui_sidebar_entries(state, &e);
  bool note = note_of(state, e, n) != NULL;
  free(e);
  return note;
}

/* Draw the sidebar as the notcurses TUI's: no frame, a rule on the right,
 * the search field only while it is used, TABLES pinned at the top and the
 * rest scrolling under it, the current tab's entry in the accent colour,
 * the highlight while the sidebar has the keys */
void tui_draw_sidebar(TuiState *state) {
  if (!state || !state->sidebar_win || !state->sidebar_visible)
    return;
  WINDOW *w = state->sidebar_win;
  int rows, width;
  getmaxyx(w, rows, width);
  int cw = width - 1;
  wbkgd(w, tui_theme_attr(TC_FG, TC_BG) | ' ');
  werase(w);
  tui_style(w, TC_DIM, TC_BG, 0);
  for (int y = 0; y < rows; y++)
    mvwaddstr(w, y, cw, "\xe2\x94\x82");
  if (cw < 4 || rows < 3) {
    wrefresh(w);
    return;
  }
  SidebarEntry *e;
  size_t n = tui_sidebar_entries(state, &e);
  /* the highlight follows the current tab when it changes */
  Tab *cur_tab = TUI_TAB(state);
  size_t open = open_entry(state, e, n);
  uint64_t uid = cur_tab ? cur_tab->uid : 0;
  if (uid != state->sidebar_view_uid) {
    state->sidebar_view_uid = uid;
    if (open != SIZE_MAX)
      state->sidebar_highlight = open;
  }
  if (n && state->sidebar_highlight >= n)
    state->sidebar_highlight = n - 1;

  int top = 0, cursor_y = -1, cursor_x = 0;
  if (search_shown(state)) {
    ThemeColor bg = state->sidebar_filter_active ? TC_EDIT_BG : TC_BG;
    tui_style(w, TC_FG, bg, 0);
    tui_print_fit(w, 0, 0, cw, "");
    tui_style(w, TC_DIM, bg, 0);
    mvwaddstr(w, 0, 1, "/");
    tui_style(w, TC_FG, bg, 0);
    tui_print_fit(w, 0, 2, cw - 2, state->sidebar_filter);
    cursor_y = 0;
    int fw = utf8_width(state->sidebar_filter, state->sidebar_filter_len);
    cursor_x = 2 + (fw < cw - 3 ? fw : cw - 3);
    top = 1;
  }
  tui_style(w, TC_DIM, TC_BG, A_BOLD);
  tui_print_fit(w, top, 1, cw - 1, SECTION_TITLE[0]);

  const char *note = note_of(state, e, n);
  size_t body = n + (note ? 1 : 0);
  int vis = rows - 1 - top;
  size_t cur_body = (n ? state->sidebar_highlight : 0) + (note ? 1 : 0);
  if (cur_body < state->sidebar_scroll)
    state->sidebar_scroll = cur_body;
  if (vis > 0 && cur_body >= state->sidebar_scroll + (size_t)vis)
    state->sidebar_scroll = cur_body - (size_t)vis + 1;
  if (state->sidebar_scroll > 0 && body <= (size_t)vis)
    state->sidebar_scroll = 0;
  bool driven = state->sidebar_focused && !state->sidebar_filter_active;
  for (int row = 0; row < vis; row++) {
    size_t line = state->sidebar_scroll + (size_t)row;
    if (line >= body)
      break;
    int y = top + 1 + row;
    if (note && line == 0) {
      tui_style(w, TC_DIM, TC_BG, 0);
      tui_print_fit(w, y, 1, cw - 1, note);
      continue;
    }
    size_t i = note ? line - 1 : line;
    const SidebarEntry *en = &e[i];
    bool cur = i == state->sidebar_highlight && (driven || !cur_tab || cur_tab->type == TAB_TYPE_CONNECTION);
    if (en->kind == SB_HEADER) {
      tui_style(w, cur ? TC_CURSOR_FG : TC_DIM, cur ? TC_CURSOR_BG : TC_BG, A_BOLD);
      tui_print_fit(w, y, 0, cw, "");
      char *title = en->section == 4 /* QUERIES does not fold */
                        ? str_dup(SECTION_TITLE[4])
                        : str_printf("%s %s", state->sidebar_collapsed & (1u << en->section) ? "\xe2\x96\xb8" : "\xe2\x96\xbe",
                                     SECTION_TITLE[en->section]);
      tui_print_fit(w, y, 1, cw - 1, title);
      free(title);
      continue;
    }
    const char *name = tui_sidebar_entry_name(state, en);
    if (i == open && !cur)
      tui_style(w, TC_ACCENT, TC_BG, A_BOLD);
    else
      tui_style(w, cur ? TC_CURSOR_FG : TC_FG, cur ? TC_CURSOR_BG : TC_BG, i == open ? A_BOLD : 0);
    tui_print_fit(w, y, 0, cw, "");
    /* a long name scrolls in place on the highlighted entry */
    size_t nb = strlen(name);
    int nw = utf8_width(name, nb);
    if (nw > cw - 1 && cur && driven) {
      size_t sc = state->sidebar_name_scroll;
      if (sc > (size_t)(nw - (cw - 1)))
        sc = (size_t)(nw - (cw - 1));
      name += utf8_offset_at_col(name, nb, (int)sc);
    }
    tui_print_fit(w, y, 1, cw - 1, name);
  }
  free(e);
  tui_style(w, TC_FG, TC_BG, 0);
  if (state->sidebar_filter_active && cursor_y >= 0) {
    curs_set(1);
    wmove(w, cursor_y, cursor_x);
  }
  wrefresh(w);
}
