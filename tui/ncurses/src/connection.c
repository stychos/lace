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
  if (!conn)
    return false; /* Error already set by tui_connect_with_progress */
  size_t idx = tui_add_open_connection(state, conn, connstr, NULL);
  if (idx == SIZE_MAX)
    return false;
  /* Into a form of the current workspace (one is made when it has none) */
  tui_ensure_tab(state);
  Workspace *ws = TUI_WORKSPACE(state);
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_FORM) {
    tui_new_connection_tab(state);
    tab = TUI_TAB(state);
  }
  return tui_place_connection(state, idx, PLACE_HERE, state->app->current_workspace, ws->current_tab);
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

  /* The sidebar's other sections: views, routines and triggers (a catalog an
   * older server cannot answer leaves them empty, the tables still list) */
  lace_objects_free(conn_obj->objects, conn_obj->num_objects);
  conn_obj->objects = NULL;
  conn_obj->num_objects = 0;
  AsyncOperation oop;
  async_init(&oop);
  oop.op_type = ASYNC_OP_LIST_OBJECTS;
  oop.conn = db_conn;
  if (async_start(&oop)) {
    bool done = tui_show_processing_dialog(state, &oop, "Loading objects...");
    if (done && oop.state == ASYNC_STATE_COMPLETED) {
      conn_obj->objects = (LaceObject *)oop.result;
      conn_obj->num_objects = oop.result_count;
      oop.result = NULL;
      oop.result_count = 0;
    }
    async_free(&oop);
  }

  /* The header names the database (a quick read; a failure leaves it out) */
  free(conn_obj->database);
  conn_obj->database = NULL;
  db_current_database(db_conn, &conn_obj->database, NULL);
  return true;
}
