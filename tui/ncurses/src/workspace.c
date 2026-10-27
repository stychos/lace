/*
 * Lace
 * TUI Tab management
 *
 * Core tab/workspace/connection lifecycle is in core/app_state.c.
 * This file contains TUI-specific operations that sync UI state.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "core/workspace.h"
#include "tui_internal.h"
#include "views/form_tab.h"
#include "util/mem.h"
#include <stdlib.h>
#include <string.h>

/* Restore TUI state from tab */
void tab_restore(TuiState *state) {
  /* Add-row mode belongs to the tab being left (its arrays follow that
   * tab's schema); switching by mouse bypasses the add-row keys */
  if (state && state->adding_row) {
    tui_cancel_add_row(state);
  }
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;

  /* Track layout state changes for window recreation */
  bool sidebar_was_visible = state->sidebar_visible;
  bool header_was_visible = state->header_visible;
  bool status_was_visible = state->status_visible;

  /* The new tab may use a different connection */
  tui_bind_connection(state);

  /* Rebind VmTable to new Tab so cursor/scroll reads work correctly */
  if (state->vm_table) {
    table_vm_bind(state->vm_table, tab);
  }

  /* Reset editing state when switching tabs */
  state->filters_editing = false;
  state->filters_cursor_row = 0;
  state->filters_cursor_col = 0;
  state->filters_scroll = 0;
  state->sidebar_filter_active = false;

  /* Layout visibility from app */
  if (state->app) {
    state->header_visible = state->app->header_visible;
    state->status_visible = state->app->status_visible;
  }

  /* Recreate windows if layout changed */
  if (sidebar_was_visible != state->sidebar_visible ||
      header_was_visible != state->header_visible ||
      status_was_visible != state->status_visible) {
    tui_recreate_windows(state);
  }

  /* Check if tab needs refresh due to changes in another tab */
  if (tab->needs_refresh && tab->type == TAB_TYPE_TABLE && tab->table_name) {
    tab->needs_refresh = false;
    tui_refresh_table(state);
  }
}

/* Switch to a different tab */
void tab_switch(TuiState *state, size_t index) {
  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws || index >= ws->num_tabs)
    return;
  if (index == ws->current_tab)
    return;

  /* Cancel any pending background load */
  tui_cancel_background_load(state);

  /* Switch to new tab */
  workspace_switch_tab(ws, index);

  /* Restore new tab state */
  tab_restore(state);

  /* Clear status message */
  free(state->status_msg);
  state->status_msg = NULL;
  state->status_is_error = false;
}

/* Create a new table tab */
bool tab_create(TuiState *state, size_t table_index) {
  if (!state || !state->app || table_index >= TUI_NUM_TABLES(state))
    return false;
  return tab_create_object(state, LACE_OBJ_TABLE, TUI_TABLES(state)[table_index], NULL, NULL);
}

bool tab_create_object(TuiState *state, LaceObjectKind kind, const char *name, const char *table, const char *detail) {
  if (!state || !state->app || !name)
    return false;
  size_t table_index = SIZE_MAX;
  for (size_t i = 0; kind == LACE_OBJ_TABLE && i < TUI_NUM_TABLES(state); i++)
    if (TUI_TABLES(state)[i] && !strcmp(TUI_TABLES(state)[i], name))
      table_index = i;

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

  /* Create new tab with connection reference */
  Tab *tab = workspace_create_table_tab(ws, connection_index, name);
  if (!tab) {
    tui_set_error(state, "Failed to create tab");
    return false;
  }
  tab->object = kind;
  tab->object_table = table ? str_dup(table) : NULL;
  tab->object_detail = detail ? str_dup(detail) : NULL;
  if (!tab_has_subview(tab, SUBVIEW_DATA)) tab->subview = SUBVIEW_STRUCTURE;

  /* Ensure UITabState capacity for new tab */
  tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                             ws->current_tab);

  /* Initialize UITabState for new tab */
  UITabState *ui = TUI_TAB_UI(state);
  if (ui) {
    /* New tab starts with filters closed */
    ui->filters_visible = false;
    ui->filters_focused = false;
  }

  /* Update sidebar last position (global state) */
  if (table_index != SIZE_MAX) state->sidebar_last_position = table_index;

  /* Reset TUI state for new tab - all panels start unfocused */
  state->sidebar_focused = false;
  tui_set_filters_visible(state, false);
  tui_set_filters_focused(state, false);
  state->filters_was_focused = false;
  state->filters_editing = false;
  state->filters_cursor_row = 0;
  state->filters_cursor_col = 0;
  state->filters_scroll = 0;

  /* Tab cursor/scroll/data is initialized by tui_load_table_data */

  /* Load the table data (updates Tab directly) */
  if (!tui_load_table_data(state, name)) {
    /* Failed - remove the tab */
    workspace_close_tab(ws, ws->current_tab);

    /* Restore previous tab if any */
    if (ws->num_tabs > 0) {
      tab_restore(state);
    }
    return false;
  }


  return true;
}

/* Longest tab label shown, in columns (longer names are clipped) */
#define TAB_LABEL_MAX_COLS 30

/* What a tab is called: its table, or a form's label; caller frees */
static char *tab_label(const Tab *tab) {
  if (tab->type == TAB_TYPE_FORM)
    return form_tab_label(tab);
  return str_dup(tab->table_name ? tab->table_name : "?");
}

int tui_tab_width(const Tab *tab) {
  char *name = tab_label(tab);
  int cols = utf8_width(name, strlen(name));
  free(name);
  if (cols > TAB_LABEL_MAX_COLS)
    cols = TAB_LABEL_MAX_COLS;
  return cols + 4; /* " name " plus the separator column */
}

/* Draw tab bar */
void tui_draw_tabs(TuiState *state) {
  if (!state || !state->tab_win)
    return;

  werase(state->tab_win);
  wbkgd(state->tab_win, COLOR_PAIR(COLOR_BORDER));

  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws) {
    wrefresh(state->tab_win);
    return;
  }

  int x = 0;

  for (size_t i = 0; i < ws->num_tabs; i++) {
    Tab *tab = &ws->tabs[i];
    if (!tab->active)
      continue;

    char *name = tab_label(tab);
    int tab_width = tui_tab_width(tab); /* Same widths as the mouse hit test */

    if (x + tab_width > state->term_cols) {
      free(name);
      break;
    }

    /* " name " in display columns, clipped to the label width; a form's
     * label in italics */
    attr_t attr = i == ws->current_tab ? COLOR_PAIR(COLOR_SELECTED) | A_BOLD
                                       : A_NORMAL;
    if (tab->type == TAB_TYPE_FORM)
      attr |= A_ITALIC;
    wattron(state->tab_win, attr);
    mvwaddch(state->tab_win, 0, x, ' ');
    tui_print_fit(state->tab_win, 0, x + 1, tab_width - 3, name);
    waddch(state->tab_win, ' ');
    wattroff(state->tab_win, attr);
    free(name);

    x += tab_width;

    /* Tab separator */
    if (i < ws->num_tabs - 1 && x < state->term_cols) {
      wattron(state->tab_win, COLOR_PAIR(COLOR_BORDER));
      mvwaddch(state->tab_win, 0, x - 1, ACS_VLINE);
      wattroff(state->tab_win, COLOR_PAIR(COLOR_BORDER));
    }
  }

  /* Show hint for new tab if sidebar visible */
  if (state->sidebar_focused) {
    const char *hint = "[+] New tab";
    int hint_len = (int)strlen(hint);
    if (state->term_cols - x > hint_len + 2) {
      wattron(state->tab_win, A_DIM);
      mvwprintw(state->tab_win, 0, state->term_cols - hint_len - 1, "%s", hint);
      wattroff(state->tab_win, A_DIM);
    }
  }

  wrefresh(state->tab_win);
}

/* Shift the UITabState entries of workspace ws_idx down over tab_idx */
static void tab_ui_remove(TuiState *state, size_t ws_idx, size_t tab_idx, size_t old_num_tabs) {
  if (old_num_tabs > 0 && state->tab_ui && ws_idx < state->tab_ui_ws_capacity &&
      state->tab_ui[ws_idx] && old_num_tabs <= state->tab_ui_capacity[ws_idx]) {
    for (size_t i = tab_idx; i < old_num_tabs - 1; i++)
      state->tab_ui[ws_idx][i] = state->tab_ui[ws_idx][i + 1];
    memset(&state->tab_ui[ws_idx][old_num_tabs - 1], 0, sizeof(UITabState));
  }
}

/* What a connection is called in a question */
static char *conn_label(Connection *c) {
  char *d = c ? tui_connstr_display(c->connstr) : NULL;
  return d ? d : str_dup("the connection");
}

static bool conn_in_transaction(const Connection *c) {
  return c && c->conn && (c->conn->in_transaction || c->conn->transaction_failed);
}

/* Closing the last workspace ends lace (on_last_workspace_close "quit") */
static bool last_close_quits(TuiState *state) {
  return state->app->num_workspaces == 1 &&
         state->app->config->general.last_workspace_close == CONFIG_LAST_WS_QUIT;
}

/* Close current tab. A form goes. The last tab of its connection in this
 * workspace lets the connection go and, with keep_connection_tab, becomes a
 * form in its place; the connection closes when no tab anywhere uses it. A
 * workspace left without tabs closes (workspace_close_current). */
void tab_close(TuiState *state) {
  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws || ws->num_tabs == 0)
    return;
  AppState *app = state->app;
  size_t ws_idx = app->current_workspace;
  size_t tab_idx = ws->current_tab;
  size_t old_num_tabs = ws->num_tabs;
  Tab *tab = &ws->tabs[tab_idx];
  bool form = tab->type == TAB_TYPE_FORM;
  size_t conn_idx = tab->connection_index;

  size_t same = 0; /* other tabs of this connection here */
  for (size_t i = 0; !form && i < ws->num_tabs; i++)
    same += i != tab_idx && ws->tabs[i].type != TAB_TYPE_FORM && ws->tabs[i].connection_index == conn_idx;
  bool last_here = !form && same == 0;
  bool keep = last_here && app->config->general.keep_connection_tab;
  bool conn_ends = last_here && app_connection_users(app, conn_idx) <= 1;
  bool quits = !keep && ws->num_tabs == 1 && last_close_quits(state);

  /* One question, before anything closes */
  Connection *c = app_get_connection(app, conn_idx);
  char *msg = NULL;
  if (conn_ends && conn_in_transaction(c)) {
    char *name = conn_label(c);
    msg = str_printf("A transaction is open on %s. Close, roll back%s?", name, quits ? " and quit" : "");
    free(name);
  } else if (quits && app->config->general.quit_confirmation) {
    msg = str_dup("Quit lace?");
  }
  if (msg) {
    bool yes = tui_show_confirm_dialog(state, msg);
    free(msg);
    if (!yes) {
      tui_refresh(state);
      return;
    }
  }

  /* Cancel any pending background load */
  tui_cancel_background_load(state);
  UITabState *ui = tui_get_tab_ui(state, ws_idx, tab_idx);
  if (ui) {
    free(ui->query_result_edit_buf);
    memset(ui, 0, sizeof *ui);
  }

  if (keep) {
    tab_make_form(tab); /* in its place */
  } else {
    workspace_close_tab(ws, tab_idx);
    tab_ui_remove(state, ws_idx, tab_idx, old_num_tabs);
  }
  if (conn_ends && c) {
    state->sidebar_tables_key = NULL;
    app_close_connection(app, conn_idx);
  }
  state->sidebar_focused = false;

  if (ws->num_tabs == 0)
    workspace_close(state);
  else
    tab_restore(state);
}

/* Close the current workspace: the connections only it used close too;
 * with none left, on_last_workspace_close says whether lace quits or a
 * fresh workspace with a form appears */
static void workspace_close_current(TuiState *state) {
  if (!state || !state->app)
    return;
  AppState *app = state->app;
  size_t ws_idx = app->current_workspace;

  /* Cancel any pending background load */
  tui_cancel_background_load(state);

  /* Free UITabState resources for all tabs in this workspace */
  if (ws_idx < state->tab_ui_ws_capacity && state->tab_ui[ws_idx]) {
    for (size_t i = 0; i < state->tab_ui_capacity[ws_idx]; i++) {
      free(state->tab_ui[ws_idx][i].query_result_edit_buf);
      state->tab_ui[ws_idx][i].query_result_edit_buf = NULL;
    }
    free(state->tab_ui[ws_idx]);
    state->tab_ui[ws_idx] = NULL;
    state->tab_ui_capacity[ws_idx] = 0;

    /* Shift UITabState workspace entries down */
    for (size_t i = ws_idx; i < state->tab_ui_ws_capacity - 1; i++) {
      state->tab_ui[i] = state->tab_ui[i + 1];
      state->tab_ui_capacity[i] = state->tab_ui_capacity[i + 1];
    }
    if (state->tab_ui_ws_capacity > 0) {
      state->tab_ui[state->tab_ui_ws_capacity - 1] = NULL;
      state->tab_ui_capacity[state->tab_ui_ws_capacity - 1] = 0;
    }
  }

  /* The connections its tabs use, to close the ones nobody else uses */
  Workspace *ws = &app->workspaces[ws_idx];
  bool *used = safe_calloc(app->num_connections + 1, sizeof(bool));
  for (size_t t = 0; t < ws->num_tabs; t++)
    if (ws->tabs[t].type != TAB_TYPE_FORM && ws->tabs[t].connection_index < app->num_connections)
      used[ws->tabs[t].connection_index] = true;

  /* Close workspace in app state (frees tabs and data) */
  app_close_workspace(app, ws_idx);
  for (size_t i = app->num_connections; i-- > 0;)
    if (used[i] && app_connection_users(app, i) == 0)
      app_close_connection(app, i);
  free(used);
  state->sidebar_tables_key = NULL;

  /* Reset UI state */
  state->sidebar_focused = false;
  state->sidebar_highlight = 0;
  state->sidebar_scroll = 0;
  state->sidebar_filter[0] = '\0';
  state->sidebar_filter_len = 0;
  state->sidebar_filter_active = false;
  tui_set_filters_visible(state, false);
  tui_set_filters_focused(state, false);
  state->filters_was_focused = false;
  state->filters_editing = false;
  state->filters_cursor_row = 0;
  state->filters_cursor_col = 0;
  state->filters_scroll = 0;

  if (app->num_workspaces > 0) {
    /* Switch to the new current workspace (adjusted by app_close_workspace) */
    tui_sync_from_app(state);
    tui_recreate_windows(state);
    tab_restore(state);
  } else if (app->config->general.last_workspace_close == CONFIG_LAST_WS_NEW) {
    tui_new_workspace_form(state);
  } else {
    state->running = false;
    app->running = false;
  }
}

void workspace_close(TuiState *state) { workspace_close_current(state); }

void tui_workspace_switch(TuiState *state, size_t index) {
  if (!state || !state->app || index >= state->app->num_workspaces)
    return;
  if (index != state->app->current_workspace) {
    tui_sync_to_workspace(state);
    app_switch_workspace(state->app, index);
  }
  tui_sync_from_app(state);
  tui_recreate_windows(state);
  if (TUI_WORKSPACE(state) && TUI_WORKSPACE(state)->num_tabs > 0)
    tab_restore(state);
}

char *tui_workspace_close_question(TuiState *state, size_t index) {
  AppState *app = state->app;
  if (index >= app->num_workspaces)
    return NULL;
  Workspace *ws = &app->workspaces[index];
  bool quits = last_close_quits(state);
  for (size_t t = 0; t < ws->num_tabs; t++) {
    size_t ci = ws->tabs[t].connection_index;
    Connection *c = ws->tabs[t].type == TAB_TYPE_FORM ? NULL : app_get_connection(app, ci);
    if (!conn_in_transaction(c))
      continue;
    size_t here = 0;
    for (size_t j = 0; j < ws->num_tabs; j++)
      here += ws->tabs[j].type != TAB_TYPE_FORM && ws->tabs[j].connection_index == ci;
    if (here < app_connection_users(app, ci))
      continue; /* another workspace keeps it open */
    char *name = conn_label(c);
    char *q = str_printf("A transaction is open on %s. Close the workspace, roll back%s?", name,
                         quits ? " and quit" : "");
    free(name);
    return q;
  }
  return quits ? str_dup("Close the last workspace and quit lace?") : NULL;
}

void tui_workspace_close_at(TuiState *state, size_t index) {
  if (!state || !state->app || index >= state->app->num_workspaces)
    return;
  size_t back = state->app->current_workspace;
  tui_workspace_switch(state, index);
  workspace_close_current(state);
  /* Back to the workspace that was current, where it moved to */
  if (back != index && state->app->num_workspaces > 0)
    tui_workspace_switch(state, back > index ? back - 1 : back);
}
