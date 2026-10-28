/*
 * Lace
 * Switch database (switch_database, D; the palette; a click on the
 * header's database segment), as the notcurses TUI's: the server's
 * databases in a pick list, the current one marked; picking another makes
 * the connection's tabs here one connection form showing the target and
 * connects there with the same user, passwords and tunnel, so a connection
 * to that database already open is shared as usual
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "tui_internal.h"
#include "views/connect_open.h"
#include "views/form_tab.h"
#include "views/list_picker.h"
#include "../../liblace/include/config/saved_connection.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include <stdlib.h>
#include <string.h>

/* The connection's tabs in this workspace become one form, in the current
 * tab's place, showing connstr; the connection closes when no tab uses it */
static size_t make_form_here(TuiState *state, size_t conn_idx, const char *connstr) {
  Workspace *ws = TUI_WORKSPACE(state);
  AppState *app = state->app;
  size_t ws_idx = app->current_workspace;
  size_t keep = ws->current_tab;
  for (size_t i = ws->num_tabs; i-- > 0;) {
    Tab *t = &ws->tabs[i];
    if (i == keep || t->type == TAB_TYPE_FORM || t->connection_index != conn_idx)
      continue;
    tui_close_tab_at(state, ws_idx, i);
    if (i < keep)
      keep--;
  }
  ws->current_tab = keep;
  Tab *tab = &ws->tabs[keep];
  UITabState *ui = tui_get_tab_ui(state, ws_idx, keep);
  if (ui) {
    free(ui->query_result_edit_buf);
    memset(ui, 0, sizeof *ui);
  }
  tab_make_form(tab);
  form_tab_set_connstr(state, tab, connstr);
  if (app_connection_users(app, conn_idx) == 0) {
    state->sidebar_tables_key = NULL;
    app_close_connection(app, conn_idx);
  }
  state->sidebar_focused = false;
  tab_restore(state);
  return keep;
}

void tui_switch_database(TuiState *state) {
  Tab *tab = state ? TUI_TAB(state) : NULL;
  if (!tab || tab->type == TAB_TYPE_FORM)
    return;
  AppState *app = state->app;
  size_t conn_idx = tab->connection_index;
  Connection *c = app_get_connection(app, conn_idx);
  if (!c || !c->conn)
    return;
  if (c->conn->driver_type == LACE_DRIVER_SQLITE) {
    tui_set_status(state, "A SQLite file is one database");
    return;
  }
  char **names = NULL, *err = NULL;
  size_t n = 0;
  if (!db_list_databases(c->conn, &names, &n, &err)) {
    tui_set_error(state, "Databases: %s", err ? err : "cannot list them");
    free(err);
    return;
  }
  if (!n) {
    tui_set_status(state, "No database to switch to");
    return;
  }
  const char *cur = c->database;
  char **rows = safe_calloc(n, sizeof *rows);
  size_t at = 0;
  for (size_t i = 0; i < n; i++) {
    bool here = cur && strcmp(cur, names[i]) == 0;
    if (here)
      at = i;
    rows[i] = here ? str_printf("%s  (current)", names[i]) : str_dup(names[i]);
  }
  int sr = getmaxy(stdscr);
  int pick = tui_pick_from_list(app->config, "Switch database", (const char *const *)rows, n, at, -1, -1, 30, 60,
                                sr - 6);
  for (size_t i = 0; i < n; i++)
    free(rows[i]);
  free(rows);
  tui_refresh(state);
  char *target = pick >= 0 ? str_dup(names[pick]) : NULL;
  lace_tables_free(names, n);
  if (!target)
    return;
  if (cur && strcmp(cur, target) == 0) {
    tui_set_status(state, "Already in %s", cur);
    free(target);
    return;
  }
  /* The same server, user, passwords and tunnel, another database */
  char *connstr = NULL;
  LaceSavedConnection *model = lace_saved_conn_from_connstr(c->connstr, NULL);
  if (model) {
    free(model->database);
    model->database = str_dup(target);
    connstr = lace_saved_conn_build_connstr(model, NULL, NULL);
  }
  if (!connstr) {
    tui_set_error(state, "Cannot build a connection string for %s", target);
    lace_saved_conn_free(model);
    free(target);
    return;
  }
  /* Asked by the last holder of an open transaction */
  size_t here = 0;
  Workspace *ws = TUI_WORKSPACE(state);
  for (size_t i = 0; i < ws->num_tabs; i++)
    here += ws->tabs[i].type != TAB_TYPE_FORM && ws->tabs[i].connection_index == conn_idx;
  bool tx = c->conn->in_transaction || c->conn->transaction_failed || c->conn->transaction_lost;
  if (tx && app_connection_users(app, conn_idx) <= here) {
    char *name = tui_connstr_display(c->connstr);
    char *msg = str_printf("A transaction is open on %s. Switch to %s and roll back?", name ? name : "the connection",
                           target);
    bool yes = tui_confirm(state, "Switch database", msg, "Switch", true);
    free(name);
    free(msg);
    if (!yes) {
      str_secure_free(connstr);
      lace_saved_conn_free(model);
      free(target);
      return;
    }
  }
  size_t tab_idx = make_form_here(state, conn_idx, connstr);
  char *msg = NULL;
  bool cancelled = false;
  if (tui_connect_and_place(state, model, NULL, connstr, PLACE_HERE, app->current_workspace, tab_idx, &msg,
                            &cancelled))
    tui_set_status(state, "Switched to %s", target);
  else if (msg)
    tui_set_error(state, "%s", msg);
  free(msg);
  str_secure_free(connstr);
  lace_saved_conn_free(model);
  free(target);
  tui_refresh(state);
}
