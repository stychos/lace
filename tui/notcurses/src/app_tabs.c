/* Connection tabs and workspaces: placing connections, switching, closing */
#include "app.h"
#include "conn/connection.h"
#include "model/workspace.h"
#include "ui/dialogs/confirm.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

NcWorkspace *app_current_workspace(NcApp *app) {
  return app->current_ws < app->num_workspaces ? app->workspaces[app->current_ws] : NULL;
}

ConnTab *app_current_tab(NcApp *app) { return workspace_current_tab(app_current_workspace(app)); }

View *app_current_view(NcApp *app) { return conntab_current_view(app_current_tab(app)); }

static size_t ws_index(const NcApp *app, const NcWorkspace *w) {
  for (size_t i = 0; w && i < app->num_workspaces; i++)
    if (app->workspaces[i] == w) return i;
  return SIZE_MAX;
}

/* The workspace and index of the tab with that uid; false when it is gone */
static bool find_tab(const NcApp *app, uint64_t uid, size_t *wsi, size_t *ti) {
  for (size_t w = 0; w < app->num_workspaces; w++) {
    size_t i = workspace_find_tab(app->workspaces[w], uid);
    if (i != SIZE_MAX) {
      *wsi = w;
      *ti = i;
      return true;
    }
  }
  return false;
}

ConnTab *app_find_tab(NcApp *app, uint64_t uid, size_t *wsi) {
  size_t w, i;
  if (!find_tab(app, uid, &w, &i)) return NULL;
  if (wsi) *wsi = w;
  return app->workspaces[w]->tabs[i];
}

ConnTab *app_place_connection(NcApp *app, NcConnection *c, NcDest dest) {
  ConnTab *t = NULL;
  size_t wsi = 0, ti = 0;
  switch (dest.kind) {
  case NC_DEST_HERE: /* the form becomes the connection, where it is */
    if (!find_tab(app, dest.tab_uid, &wsi, &ti) || !app->workspaces[wsi]->tabs[ti]->form) break;
    t = app->workspaces[wsi]->tabs[ti];
    conntab_make_connected(t, c);
    app->current_ws = wsi;
    app->workspaces[wsi]->current = ti;
    break;
  case NC_DEST_APPEND: {
    NcWorkspace *ws = app_current_workspace(app);
    if (!ws) break;
    t = workspace_add_tab(ws, c);
    break;
  }
  case NC_DEST_BACKGROUND:
    if (!find_tab(app, dest.tab_uid, &wsi, &ti)) break;
    t = workspace_insert_tab(app->workspaces[wsi], ti + 1, c, false);
    break;
  case NC_DEST_NEW_WORKSPACE:
    app_new_workspace(app, NULL, false);
    t = workspace_add_tab(app_current_workspace(app), c);
    break;
  case NC_DEST_WORKSPACE:
    wsi = ws_index(app, dest.ws);
    if (wsi == SIZE_MAX) break;
    t = workspace_add_tab(app->workspaces[wsi], c);
    app->current_ws = wsi;
    break;
  }
  if (!t) {
    app_set_status(app, true, "The tab was closed before the connection opened");
    return NULL;
  }
  app_conn_hold(app, c);
  t->auto_open = app->config->general.auto_open_first_table;
  app_auto_open(app, t);
  return t;
}

void app_auto_open(NcApp *app, ConnTab *t) {
  if (!t || !t->auto_open || !t->conn || !nc_connection_tables_loaded(t->conn)) return;
  if (t != app_current_tab(app) || t->num_views) {
    t->auto_open = t->num_views == 0 && t != app_current_tab(app); /* waits until it is current */
    return;
  }
  t->auto_open = false;
  size_t n = 0;
  char **tables = nc_connection_tables(t->conn, &n);
  if (n) app_open_table(app, tables[0]);
}

void app_switch_tab(NcApp *app, size_t i) {
  NcWorkspace *ws = app_current_workspace(app);
  View *cur = app_current_view(app);
  if (cur) cur->split_drag = false;
  if (ws && i < ws->num_tabs) ws->current = i;
}

static void close_workspace_now(NcApp *app, NcWorkspace *w);

/* A form goes; a connected tab lets its connection go and, with
 * keep_connection_tab, becomes a form in place. A workspace left with no
 * tab closes. */
static void close_tab_now(NcApp *app, NcWorkspace *ws, size_t i) {
  ConnTab *t = ws->tabs[i];
  NcConnection *c = t->conn;
  t->conn = NULL;
  if (c && app->config->general.keep_connection_tab) conntab_make_form(t);
  else workspace_remove_tab(ws, i);
  app_conn_release(app, c);
  if (!ws->num_tabs) close_workspace_now(app, ws);
}

static void close_tab_confirmed(NcApp *app, bool yes, void *data) {
  uint64_t *uid = data;
  size_t w, i;
  if (yes && find_tab(app, *uid, &w, &i)) close_tab_now(app, app->workspaces[w], i);
  free(uid);
}

/* The open transaction a close would roll back: only the last holder closes it */
static bool closes_transaction(const NcApp *app, const NcConnection *c) {
  return c && app_conn_holders(app, c) <= 1 && nc_connection_tx_state(c) != NC_TX_NONE;
}

/* Closing the last workspace ends lace (on_last_workspace_close "quit") */
static bool last_close_quits(const NcApp *app) {
  return app->num_workspaces == 1 && app->config->general.last_workspace_close == CONFIG_LAST_WS_QUIT;
}

void app_close_tab(NcApp *app, size_t i) {
  NcWorkspace *ws = app_current_workspace(app);
  if (!ws || i >= ws->num_tabs) return;
  ConnTab *t = ws->tabs[i];
  NcConnection *c = t->conn;
  bool goes = !c || !app->config->general.keep_connection_tab; /* the tab itself goes */
  bool quits = goes && ws->num_tabs == 1 && last_close_quits(app);
  char *msg = NULL;
  if (closes_transaction(app, c))
    msg = str_printf("A transaction is open on %s. Close and roll back%s?", nc_connection_name(c),
                     quits ? " and quit" : "");
  else if (quits && app->config->general.quit_confirmation)
    msg = str_dup("Quit lace?");
  if (msg) {
    uint64_t *uid = safe_malloc(sizeof *uid);
    *uid = t->uid;
    confirm_open(app, quits ? "Quit" : "Close connection", msg, quits ? "Quit" : "Close", true,
                 close_tab_confirmed, uid);
    free(msg);
    return;
  }
  close_tab_now(app, ws, i);
}

void app_switch_workspace(NcApp *app, size_t i) {
  if (i < app->num_workspaces) app->current_ws = i;
}

void app_new_workspace(NcApp *app, const char *name, bool with_form) {
  char *n = name && *name ? str_dup(name)
            : app->num_workspaces ? str_printf("Workspace %zu", app->num_workspaces + 1)
                                  : str_dup("Workspace");
  app->workspaces = safe_realloc(app->workspaces, (app->num_workspaces + 1) * sizeof *app->workspaces);
  app->workspaces[app->num_workspaces] = workspace_new(n);
  app->current_ws = app->num_workspaces++;
  free(n);
  if (with_form) workspace_add_form(app->workspaces[app->current_ws], SIZE_MAX, true);
}

void app_rename_workspace(NcApp *app, size_t i, const char *name) {
  if (i >= app->num_workspaces || !name || !*name) return;
  free(app->workspaces[i]->name);
  app->workspaces[i]->name = str_dup(name);
}

/* Every tab's connection is let go and the workspace freed; the last one
 * follows on_last_workspace_close */
static void close_workspace_now(NcApp *app, NcWorkspace *w) {
  size_t i = ws_index(app, w);
  if (i == SIZE_MAX) return;
  for (size_t k = 0; k < w->num_tabs; k++) {
    app_conn_release(app, w->tabs[k]->conn);
    w->tabs[k]->conn = NULL;
  }
  workspace_free(w);
  memmove(app->workspaces + i, app->workspaces + i + 1, (app->num_workspaces - i - 1) * sizeof *app->workspaces);
  app->num_workspaces--;
  if (!app->num_workspaces) {
    bool quit = app->config->general.last_workspace_close == CONFIG_LAST_WS_QUIT;
    /* quitting: an empty workspace keeps the app's invariants until the loop ends */
    app_new_workspace(app, NULL, !quit);
    if (quit) app->quit = true;
  } else if (app->current_ws > i || app->current_ws >= app->num_workspaces) {
    app->current_ws = app->current_ws ? app->current_ws - 1 : 0;
  }
}

static void close_workspace_confirmed(NcApp *app, bool yes, void *data) {
  if (yes) close_workspace_now(app, data);
}

/* A connection whose every holder is a tab of w, with a transaction open */
static NcConnection *workspace_closes_transaction(const NcApp *app, const NcWorkspace *w) {
  for (size_t k = 0; k < w->num_tabs; k++) {
    NcConnection *c = w->tabs[k]->conn;
    if (!c || nc_connection_tx_state(c) == NC_TX_NONE) continue;
    size_t here = 0;
    for (size_t j = 0; j < w->num_tabs; j++) here += w->tabs[j]->conn == c;
    if (here >= app_conn_holders(app, c)) return c;
  }
  return NULL;
}

void app_close_workspace(NcApp *app, size_t i) {
  if (i >= app->num_workspaces) return;
  NcWorkspace *w = app->workspaces[i];
  NcConnection *c = workspace_closes_transaction(app, w);
  bool quits = last_close_quits(app);
  char *msg = NULL;
  if (c)
    msg = str_printf("A transaction is open on %s. Close the workspace and roll back%s?", nc_connection_name(c),
                     quits ? " and quit" : "");
  else if (quits && app->config->general.quit_confirmation)
    msg = str_dup("Quit lace?");
  if (msg) {
    confirm_open(app, quits ? "Quit" : "Close workspace", msg, quits ? "Quit" : "Close", true,
                 close_workspace_confirmed, w);
    free(msg);
    return;
  }
  close_workspace_now(app, w);
}
