/*
 * Lace
 * Connecting, disconnecting and loading a connection's tables
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/session.h"
#include "core/actions.h"
#include "../../liblace/include/util/mem.h"
#include "tui_internal.h"
#include "views/config_view.h"
#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


bool tui_connect(TuiState *state, const char *connstr) {
  if (!state || !state->app || !connstr)
    return false;

  /* Use async connection with progress dialog */
  DbConnection *conn = tui_connect_with_progress(state, connstr);

  if (!conn) {
    /* Error already set by tui_connect_with_progress */
    return false;
  }

  /* Create a new connection in the app state hierarchy */
  Connection *app_conn = app_add_connection(state->app, conn, connstr);
  if (!app_conn) {
    db_disconnect(conn);
    tui_set_error(state, "Failed to create connection");
    return false;
  }

  /* Load tables first */
  if (!tui_load_tables(state, app_conn)) {
    return false;
  }

  /* Create a workspace if none exists */
  Workspace *ws = app_current_workspace(state->app);
  if (!ws) {
    ws = app_create_workspace(state->app);
    if (!ws) {
      tui_set_error(state, "Failed to create workspace");
      return false;
    }
  }

  /* Find connection index */
  size_t conn_index = app_find_connection_index(state->app, conn);

  /* Check if we should auto-open the first table */
  bool auto_open = state->app->config &&
                   state->app->config->general.auto_open_first_table &&
                   app_conn->num_tables > 0;

  if (auto_open) {
    /* Create a table tab with the first table */
    size_t table_idx = 0;
    Tab *tab = workspace_create_table_tab(ws, conn_index, app_conn->tables[table_idx]);
    if (tab) {

      /* Ensure UITabState capacity before accessing */
      size_t ws_idx = state->app->current_workspace;
      size_t tab_idx_ui = ws->current_tab;
      tui_ensure_tab_ui_capacity(state, ws_idx, tab_idx_ui);

      /* Load table data (updates Tab directly) */
      tui_load_table_data(state, app_conn->tables[table_idx]);

      /* Initialize sidebar state (global, not per-tab) */
      state->sidebar_visible = true;
      state->sidebar_focused = false;

      tui_recreate_windows(state);
      tui_set_status(state, "Connected to %s - %s", conn->database,
                     app_conn->tables[table_idx]);
      return true;
    }
  } else {
    /* Create a connection tab (don't auto-load any table) */
    Tab *tab = workspace_create_connection_tab(ws, conn_index, connstr);
    if (tab) {
      /* Ensure UITabState capacity before accessing */
      size_t ws_idx = state->app->current_workspace;
      size_t tab_idx = ws->current_tab;
      tui_ensure_tab_ui_capacity(state, ws_idx, tab_idx);

      /* Initialize per-tab UI state */
      UITabState *ui = TUI_TAB_UI(state);
      if (ui) {
        ui->filters_visible = false;
        ui->filters_focused = false;
      }

      /* Initialize sidebar state (global) - focus sidebar to select a table */
      state->sidebar_visible = true;
      state->sidebar_focused = true;
      state->sidebar_highlight = 0;
      state->sidebar_scroll = 0;

      /* Connection tab has no table data - Tab fields stay NULL */

      tui_recreate_windows(state);

      if (app_conn->num_tables == 0) {
        tui_set_status(state, "Connected to %s - No tables found",
                       conn->database);
      } else {
        tui_set_status(state, "Connected to %s - Select a table from sidebar",
                       conn->database);
      }
      return true;
    }
  }

  tui_set_error(state, "Failed to create tab");
  return false;
}

void tui_disconnect(TuiState *state) {
  if (!state || !state->app)
    return;

  /* Cancel all background operations before cleanup to prevent use-after-free
   */
  /* Iterate through all workspaces/tabs and cancel their bg ops */
  AppState *app = state->app;
  for (size_t w = 0; w < app->num_workspaces; w++) {
    Workspace *ws = &app->workspaces[w];
    for (size_t t = 0; t < ws->num_tabs; t++) {
      Tab *tab = &ws->tabs[t];
      if (tab->bg_load_op) {
        AsyncOperation *op = (AsyncOperation *)tab->bg_load_op;
        async_cancel(op);
        async_wait(op, 500); /* Wait for cancellation */
        /* Poll until done */
        while (async_poll(op) == ASYNC_STATE_RUNNING) {
          struct timespec ts = {0, 10000000L}; /* 10ms */
          nanosleep(&ts, NULL);
        }
        if (op->result) {
          db_result_free((ResultSet *)op->result);
          op->result = NULL;
        }
        async_free(op);
        free(op);
        tab->bg_load_op = NULL;
      }
    }
  }
  state->bg_loading_active = false;

  /* Cleanup is handled by app_state_cleanup() */
  app_state_cleanup(state->app);
  app_state_init(state->app);

  /* Hide sidebar when disconnected */
  if (state->sidebar_visible) {
    state->sidebar_visible = false;
    state->sidebar_focused = false;
    tui_recreate_windows(state);
  }

  state->sidebar_tables_key = NULL;

  /* Clear TUI-specific UI state */
  tui_set_filters_visible(state, false);
  tui_set_filters_focused(state, false);
  state->sidebar_highlight = 0;
  state->sidebar_scroll = 0;
  state->sidebar_filter[0] = '\0';
  state->sidebar_filter_len = 0;
  state->sidebar_filter_active = false;

  /* Clear edit state */
  state->editing = false;
  free(state->edit_buffer);
  state->edit_buffer = NULL;
  state->edit_pos = 0;
}

bool tui_load_tables(TuiState *state, Connection *conn_obj) {
  if (!state || !state->app || !conn_obj || !conn_obj->conn)
    return false;
  DbConnection *db_conn = conn_obj->conn;

  /* Free old tables */
  if (conn_obj->tables) {
    for (size_t i = 0; i < conn_obj->num_tables; i++) {
      free(conn_obj->tables[i]);
    }
    free(conn_obj->tables);
    conn_obj->tables = NULL;
    conn_obj->num_tables = 0;
  }

  /* Use async operation with progress dialog */
  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_LIST_TABLES;
  op.conn = db_conn;

  if (!async_start(&op)) {
    tui_set_error(state, "Failed to start operation");
    return false;
  }

  bool completed = tui_show_processing_dialog(state, &op, "Loading tables...");

  if (!completed || op.state == ASYNC_STATE_CANCELLED) {
    tui_set_status(state, "Operation cancelled");
    async_free(&op);
    return false;
  }

  if (op.state == ASYNC_STATE_ERROR) {
    tui_set_error(state, "Failed to list tables: %s",
                  op.error ? op.error : "Unknown error");
    async_free(&op);
    return false;
  }

  conn_obj->tables = (char **)op.result;
  conn_obj->num_tables = op.result_count;
  async_free(&op);

  return true;
}
