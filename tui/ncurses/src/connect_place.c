/*
 * Lace
 * Where an open connection goes: into a connection form in place, a tab
 * after it, a new workspace, or another workspace; and the rule that a
 * workspace is never left without a tab
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/config.h"
#include "core/history.h"
#include "tui_internal.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

/* Persistent history of a saved connection, read from its file */
static void load_connection_history(TuiState *state, Connection *conn) {
  if (!state || !state->app || !conn || !conn->saved_conn_id)
    return;
  Config *config = state->app->config;
  if (!config || config->general.history_mode != HISTORY_MODE_PERSISTENT)
    return;
  if (conn->history) {
    if (!conn->history->connection_id)
      conn->history->connection_id = str_dup(conn->saved_conn_id);
  } else {
    conn->history = history_create(conn->saved_conn_id);
    if (!conn->history)
      return;
  }
  char *err = NULL;
  if (!history_load(conn->history, &err))
    free(err); /* an empty history is fine */
}

size_t tui_add_open_connection(TuiState *state, DbConnection *db, const char *connstr,
                               const char *saved_id) {
  Connection *c = app_add_connection(state->app, db, connstr);
  if (!c) {
    db_disconnect(db);
    tui_set_error(state, "Failed to add connection");
    return SIZE_MAX;
  }
  size_t idx = state->app->num_connections - 1;
  c->saved_conn_id = saved_id && *saved_id ? str_dup(saved_id) : NULL;
  load_connection_history(state, c);
  tui_load_tables(state, c);
  return idx;
}

void tui_tab_ui_insert(TuiState *state, size_t ws_idx, size_t at) {
  Workspace *ws = ws_idx < state->app->num_workspaces ? &state->app->workspaces[ws_idx] : NULL;
  if (!ws || !ws->num_tabs || !tui_ensure_tab_ui_capacity(state, ws_idx, ws->num_tabs))
    return;
  UITabState *ui = state->tab_ui[ws_idx];
  size_t old = ws->num_tabs - 1; /* the tabs before the insert */
  if (at < old)
    memmove(&ui[at + 1], &ui[at], (old - at) * sizeof(UITabState));
  if (at <= old)
    memset(&ui[at], 0, sizeof(UITabState));
}

/* The UI after the current tab became a connected one */
static void show_connected(TuiState *state, Connection *conn, bool table_open) {
  state->sidebar_visible = true;
  state->sidebar_focused = !table_open;
  state->sidebar_highlight = 0;
  state->sidebar_scroll = 0;
  tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                             TUI_WORKSPACE(state)->current_tab);
  tui_recreate_windows(state);
  tab_restore(state);
  const char *db = conn->conn && conn->conn->database ? conn->conn->database : "the database";
  if (table_open)
    tui_set_status(state, "Connected to %s", db);
  else if (conn->num_tables == 0)
    tui_set_status(state, "Connected to %s - No tables found", db);
  else
    tui_set_status(state, "Connected to %s - Select a table from the sidebar", db);
}

/* The current tab, now a CONNECTION tab of conn_idx: with
 * auto_open_first_table and tables, it shows the first one instead */
static bool fill_current(TuiState *state, size_t conn_idx) {
  Connection *conn = app_get_connection(state->app, conn_idx);
  Tab *tab = TUI_TAB(state);
  if (!conn || !tab)
    return false;
  tab_make_connection(tab, conn_idx, conn->connstr);
  bool open = state->app->config && state->app->config->general.auto_open_first_table &&
              conn->num_tables > 0;
  if (open) {
    tab->type = TAB_TYPE_TABLE;
    free(tab->table_name);
    tab->table_name = str_dup(conn->tables[0]);
    if (!tui_load_table_data(state, conn->tables[0])) {
      tab_make_connection(tab, conn_idx, conn->connstr); /* the sidebar to pick another */
      open = false;
    }
  }
  show_connected(state, conn, open);
  return true;
}

bool tui_place_connection(TuiState *state, size_t conn_idx, PlaceKind kind, size_t ws_idx,
                          size_t tab_idx) {
  AppState *app = state->app;
  Connection *conn = app_get_connection(app, conn_idx);
  if (!conn)
    return false;
  switch (kind) {
  case PLACE_HERE: {
    if (ws_idx >= app->num_workspaces || tab_idx >= app->workspaces[ws_idx].num_tabs)
      return false;
    if (ws_idx != app->current_workspace)
      tui_workspace_switch(state, ws_idx);
    app->workspaces[ws_idx].current_tab = tab_idx;
    return fill_current(state, conn_idx);
  }
  case PLACE_BACKGROUND: {
    if (ws_idx >= app->num_workspaces)
      return false;
    Workspace *ws = &app->workspaces[ws_idx];
    size_t keep = ws->current_tab;
    size_t at = tab_idx + 1;
    Tab *tab = workspace_insert_form_tab(ws, at);
    if (!tab)
      return false;
    tab_make_connection(tab, conn_idx, conn->connstr);
    tui_tab_ui_insert(state, ws_idx, at);
    ws->current_tab = keep >= at ? keep + 1 : keep;
    tui_set_status(state, "Opened %s in a tab after this one", tab->table_name);
    return true;
  }
  case PLACE_NEW_WORKSPACE: {
    tui_sync_to_workspace(state);
    Workspace *ws = app_create_workspace(app); /* becomes current */
    if (!ws || !workspace_insert_form_tab(ws, 0)) {
      tui_set_error(state, "Failed to create workspace");
      return false;
    }
    tui_sync_from_app(state);
    return fill_current(state, conn_idx);
  }
  case PLACE_WORKSPACE: {
    if (ws_idx >= app->num_workspaces)
      return false;
    Workspace *ws = &app->workspaces[ws_idx];
    if (!workspace_insert_form_tab(ws, ws->num_tabs))
      return false;
    size_t cur = ws->current_tab;
    if (ws_idx != app->current_workspace)
      tui_workspace_switch(state, ws_idx);
    ws->current_tab = cur;
    return fill_current(state, conn_idx);
  }
  }
  return false;
}

/* A new workspace (named after its place) holding one form, made current */
static void add_form_workspace(TuiState *state) {
  if (state->app->num_workspaces)
    tui_sync_to_workspace(state);
  Workspace *ws = app_create_workspace(state->app);
  if (!ws)
    return;
  if (state->app->num_workspaces == 1)
    snprintf(ws->name, sizeof(ws->name), "Workspace");
  workspace_insert_form_tab(ws, 0);
  state->sidebar_focused = false;
  tui_sync_from_app(state);
  tui_recreate_windows(state);
  tab_restore(state);
}

void tui_new_workspace_form(TuiState *state) { add_form_workspace(state); }

void tui_ensure_tab(TuiState *state) {
  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws) {
    add_form_workspace(state);
    return;
  }
  if (ws->num_tabs == 0) {
    workspace_insert_form_tab(ws, 0);
    state->sidebar_focused = false;
    tui_sync_from_app(state);
    tui_recreate_windows(state);
    tab_restore(state);
  }
}

void tui_new_connection_tab(TuiState *state) {
  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws) {
    add_form_workspace(state);
    return;
  }
  tui_cancel_background_load(state);
  size_t at = ws->num_tabs ? ws->current_tab + 1 : 0;
  if (!workspace_insert_form_tab(ws, at))
    return;
  tui_tab_ui_insert(state, state->app->current_workspace, at);
  state->sidebar_focused = false;
  tab_restore(state);
}
