#include "app.h"
#include "ui/form_tab.h"
#include "ui/dialogs/workspaces.h"
#include "ui/query_view.h"
#include "conn/mainq.h"
#include "model/workspace.h"
#include "session.h"
#include "ui/dialogs/confirm.h"
#include "ui/dialogs/help.h"
#include "ui/grid.h"
#include "ui/input.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "ui/palette.h"
#include "ui/panels.h"
#include "ui/screen.h"
#include "ui/sidebar.h"
#include "ui/subviews.h"
#include "ui/tabstrip.h"
#include "util/mem.h"
#include "util/str.h"
#include <errno.h>
#include <locale.h>
#include <notcurses/notcurses.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_MAX 500
#define MAX_INPUT_ERRORS 100
#define STATUS_MS 5000
/* Terminal-query replies (DA1 and friends) come back as input events right
 * after init: ignore everything for this long */
#define STARTUP_DRAIN_MS 400
#define FOCUS_HOLD_MS 40

int64_t app_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void free_targets(NcApp *app) {
  for (size_t i = 0; i < app->num_targets; i++) str_secure_free(app->targets[i]); /* may hold a password */
  free(app->targets);
  app->targets = NULL;
  app->num_targets = 0;
}

NcApp *app_new(int argc, char **argv, char **error) {
  NcApp *app = safe_calloc(1, sizeof *app);
  setlocale(LC_ALL, "");
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--no-session")) {
      app->no_session = true;
    } else if (!strcmp(argv[i], "--version")) {
      printf("lace-nc\n");
      free_targets(app);
      free(app);
      exit(0);
    } else if (argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--")) {
      if (error) *error = str_printf("unknown option %s", argv[i]);
      free_targets(app);
      free(app);
      return NULL;
    } else {
      app->targets = safe_realloc(app->targets, (app->num_targets + 1) * sizeof *app->targets);
      app->targets[app->num_targets++] = str_dup(argv[i]);
    }
  }

  char *err = NULL;
  app->config = config_load(&err);
  list_use_hotkeys(app->config); /* j, k, g, G in every list */
  if (!app->config) {
    if (error) *error = err ? err : str_dup("cannot load config");
    else free(err);
    free_targets(app);
    free(app);
    return NULL;
  }
  app->config->frontend = CONFIG_FE_NOTCURSES;
  if (err) {
    app_log(app, true, "config: %s", err);
    free(err);
  }

  err = NULL;
  app->connmgr = connmgr_load(&err);
  if (err) {
    app_log(app, true, "connections: %s", err);
    free(err);
  }
  recent_load(&app->recent);
  saved_workspaces_load(&app->saved_ws);

  LaceClientConfig cc = {.mode = LACE_CONN_AUTO,
                         .spawn_mode = (LaceSpawnMode)app->config->general.daemon_spawn_mode,
                         .idle_timeout = app->config->general.daemon_idle_timeout};
  app->client = lace_client_create_with_config(&cc);
  if (!lace_client_connected(app->client)) {
    const char *why = lace_client_error(app->client);
    app_set_status(app, true, "daemon: %s", why ? why : "not reachable");
  }
  app->mainq = mainq_new();
  if (!app->mainq) {
    if (error) *error = str_dup("cannot create the completion queue");
    lace_client_destroy(app->client);
    connmgr_free(app->connmgr);
    recent_free(&app->recent);
    saved_workspaces_free(&app->saved_ws);
    config_free(app->config);
    for (size_t i = 0; i < app->num_log; i++) str_secure_free(app->log[i].line);
    free(app->log);
    free(app->status);
    free_targets(app);
    free(app);
    return NULL;
  }
  app->workspaces = safe_calloc(1, sizeof *app->workspaces);
  app->workspaces[0] = workspace_new("Workspace");
  app->num_workspaces = 1;
  return app;
}

void app_free(NcApp *app) {
  if (!app) return;
  app->quit = true;
  app_connect_cleanup(app);
  nc_session_restore_free(app);
  /* nc_connection_free() stops and joins each connection; the drain then
   * lets the completions still queued drop their references */
  for (size_t i = 0; i < app->num_workspaces; i++)
    for (size_t k = 0; k < app->workspaces[i]->num_tabs; k++) {
      ConnTab *t = app->workspaces[i]->tabs[k];
      if (t->owner) nc_owner_release(t->owner), t->owner = NULL;
      t->conn = NULL;
    }
  for (size_t i = 0; i < app->num_conns; i++) nc_connection_free(app->conns[i].conn);
  free(app->conns);
  app->conns = NULL;
  app->num_conns = 0;
  if (app->mainq) {
    mainq_drain(app->mainq);
  }
  overlay_free_all(app);
  free(app->header_spans.v);
  free(app->status_spans.v);
  free(app->strip_spans.v);
  free(app->strip_close_spans.v);
  for (size_t i = 0; i < app->num_workspaces; i++) workspace_free(app->workspaces[i]);
  free(app->workspaces);
  if (app->connmgr) connmgr_free(app->connmgr);
  recent_free(&app->recent);
  saved_workspaces_free(&app->saved_ws);
  if (app->config) config_free(app->config);
  for (size_t i = 0; i < app->num_log; i++) str_secure_free(app->log[i].line);
  free(app->log);
  free(app->status);
  str_secure_free(app->clipboard);
  free_targets(app);
  /* A connect thread that never answered still uses the client, the queue
   * and app itself: leave them to the exiting process */
  if (app->abandon) return;
  if (app->mainq) mainq_free(app->mainq);
  if (app->client) lace_client_destroy(app->client);
  free(app);
}

void app_log(NcApp *app, bool error, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *text = str_vprintf(fmt, ap);
  va_end(ap);
  if (!text) return;
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  char *line = str_printf("%02d:%02d:%02d  %s", tm.tm_hour, tm.tm_min, tm.tm_sec, text);
  str_secure_free(text); /* log lines may quote SQL or a connection string */
  if (!line) return;
  if (app->num_log == LOG_MAX) {
    str_secure_free(app->log[0].line);
    memmove(app->log, app->log + 1, (LOG_MAX - 1) * sizeof *app->log);
    app->num_log--;
  }
  if (app->num_log == app->log_cap) {
    app->log_cap = app->log_cap ? app->log_cap * 2 : 32;
    app->log = safe_realloc(app->log, app->log_cap * sizeof *app->log);
  }
  app->log[app->num_log++] = (LogLine){line, error, app_now_ms()};
  app->log_seq++;
}

void app_log_job(NcApp *app, const NcJob *job, const char *label) {
  const char *sql = job->sql && *job->sql ? job->sql : job->last_sql && *job->last_sql ? job->last_sql : label;
  char *flat = str_dup(sql ? sql : "");
  for (char *c = flat; *c; c++)
    if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
  size_t n = strlen(flat);
  if (n > 120) {
    n = 120;
    while (n > 0 && ((unsigned char)flat[n] & 0xc0) == 0x80) n--; /* not inside a character */
    flat[n] = '\0';
  }
  char *out;
  if (job->error) out = str_printf("error: %s", job->error);
  else if (job->result && job->result->num_columns > 0) out = str_printf("%zu %s", job->result->num_rows, job->result->num_rows == 1 ? "row" : "rows");
  else if (job->result) out = str_printf("%lld affected", (long long)job->result->rows_affected);
  else if (job->kind == NC_JOB_COUNT) out = str_printf("%lld %s", (long long)job->count, job->count == 1 ? "row" : "rows");
  else if (job->affected > 0) out = str_printf("%lld affected", (long long)job->affected);
  else out = str_dup("ok");
  for (char *c = out; *c; c++)
    if (*c == '\n' || *c == '\r') *c = ' ';
  app_log(app, job->error != NULL, "%.0f ms  %s  \xe2\x86\x92 %s", job->elapsed_ms, flat, out);
  free(flat);
  free(out);
}

void app_set_status(NcApp *app, bool error, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *msg = str_vprintf(fmt, ap);
  va_end(ap);
  free(app->status);
  app->status = msg;
  app->status_error = error;
  app->status_until_ms = msg ? app_now_ms() + STATUS_MS : 0;
  if (msg) app_log(app, error, "%s", msg);
}

bool app_busy(const NcApp *app) {
  for (size_t i = 0; i < app->num_conns; i++)
    if (nc_connection_busy(app->conns[i].conn)) return true;
  return false;
}

static void quit_confirmed(NcApp *app, bool yes, void *data) {
  (void)data;
  if (yes) app->quit = true; /* the session is saved by app_run once the screen is down */
}

/* Quitting asks first: a transaction is open or the config says so */
bool app_quit_needs_confirm(const NcApp *app) {
  for (size_t i = 0; i < app->num_conns; i++)
    if (nc_connection_tx_state(app->conns[i].conn) != NC_TX_NONE) return true;
  return app->config && app->config->general.quit_confirmation;
}

void app_quit(NcApp *app) {
  for (size_t i = 0; i < app->num_conns; i++) {
    NcConnection *c = app->conns[i].conn;
    if (nc_connection_tx_state(c) != NC_TX_NONE) {
      char *msg = str_printf("A transaction is open on %s. Quit and roll it back?", nc_connection_name(c));
      confirm_open(app, "Quit", msg, "Quit", true, quit_confirmed, NULL);
      free(msg);
      return;
    }
  }
  if (app->config && app->config->general.quit_confirmation) {
    confirm_open(app, "Quit", "Quit lace?", "Quit", false, quit_confirmed, NULL);
    return;
  }
  app->quit = true;
}

/* Go to a strip cell: its tab, and its view when it has one */
static void switch_to_entry(NcApp *app, const StripEntry *e) {
  app_switch_tab(app, e->tab);
  if (e->view != SIZE_MAX) app_switch_view(app, e->view);
}

/* The tab keys walk the strip: every view of every connection tab (a tab
 * without views counts as one cell), in the strip's order, wrapping at the
 * ends */
static bool cycle_strip(NcApp *app, NcWorkspace *ws, bool next) {
  StripEntry *e;
  size_t cur;
  size_t n = tabstrip_entries(ws, &e, &cur);
  if (n < 2) {
    app_set_status(app, false, n ? "Only one tab" : "No tabs open");
  } else {
    size_t i = cur == SIZE_MAX ? (next ? n - 1 : 0) : next ? (cur + 1) % n : (cur + n - 1) % n;
    switch_to_entry(app, &e[i]);
  }
  free(e);
  return true;
}

/* The connection tab N (1..9) is the strip's Nth cell: what the user sees */
static bool switch_to_strip_cell(NcApp *app, NcWorkspace *ws, size_t n) {
  StripEntry *e;
  size_t cur;
  size_t count = tabstrip_entries(ws, &e, &cur);
  bool ok = n < count;
  if (ok) switch_to_entry(app, &e[n]);
  else app_set_status(app, false, "No tab %zu", n + 1);
  free(e);
  return ok;
}

bool app_do_action(NcApp *app, HotkeyAction action) {
  NcWorkspace *ws = app_current_workspace(app);
  switch (action) {
  case HOTKEY_QUIT:
    app_quit(app);
    return true;
  case HOTKEY_PALETTE:
    palette_open(app);
    return true;
  case HOTKEY_HELP:
    help_open(app);
    return true;
  case HOTKEY_CONNECT_DIALOG:
    workspaces_open(app);
    return true;
  case HOTKEY_NEW_CONNECTION: { /* a form right after the current tab */
    if (!ws) return false;
    workspace_add_form(ws, ws->num_tabs ? ws->current + 1 : 0, true);
    return true;
  }
  case HOTKEY_NEXT_CONN_TAB: /* the same walk as next_tab/prev_tab */
  case HOTKEY_PREV_CONN_TAB:
    if (!ws) return false;
    return cycle_strip(app, ws, action == HOTKEY_NEXT_CONN_TAB);
  case HOTKEY_CONN_TAB_1: case HOTKEY_CONN_TAB_2: case HOTKEY_CONN_TAB_3:
  case HOTKEY_CONN_TAB_4: case HOTKEY_CONN_TAB_5: case HOTKEY_CONN_TAB_6:
  case HOTKEY_CONN_TAB_7: case HOTKEY_CONN_TAB_8: case HOTKEY_CONN_TAB_9:
    if (!ws) return false;
    return switch_to_strip_cell(app, ws, (size_t)(action - HOTKEY_CONN_TAB_1));
  case HOTKEY_PREV_TAB:
  case HOTKEY_NEXT_TAB:
    if (!ws) return false;
    return cycle_strip(app, ws, action == HOTKEY_NEXT_TAB);
  case HOTKEY_CLOSE_TAB: { /* the current view; a tab without views closes itself */
    ConnTab *t = app_current_tab(app);
    if (!t || !ws) return false;
    if (t->num_views) app_close_view(app, t->current);
    else app_close_tab(app, ws->current);
    return true;
  }
  case HOTKEY_NEW_TAB: { /* the sidebar's table (view, routine, trigger), opened behind the current view */
    ConnTab *t = app_current_tab(app);
    LaceObjectKind kind = LACE_OBJ_TABLE;
    char *table = NULL, *detail = NULL;
    char *name = t ? sidebar_highlighted_object(t, &kind, &table, &detail) : NULL;
    if (!name) {
      app_set_status(app, false, "Pick a table in the sidebar first");
      return true;
    }
    app_open_object_background(app, kind, name, table, detail);
    free(name);
    free(table);
    free(detail);
    return true;
  }
  case HOTKEY_OPEN_QUERY:
    return app_open_query(app, NULL) != NULL;
  case HOTKEY_TOGGLE_SIDEBAR: {
    ConnTab *t = app_current_tab(app);
    if (!t) return false;
    t->sidebar = !t->sidebar;
    t->sidebar_focused = t->sidebar; /* shown: the list takes the keys */
    return true;
  }
  case HOTKEY_SIDEBAR_FILTER: { /* show the sidebar's search field (and the sidebar) */
    ConnTab *t = app_current_tab(app);
    if (!t) return false;
    t->sidebar = true;
    sidebar_show_search(t);
    return true;
  }
  case HOTKEY_TOGGLE_LOG:
    log_open(app);
    return true;
  case HOTKEY_SWITCH_DATABASE:
    app_switch_database(app);
    return true;
  case HOTKEY_NEW_WORKSPACE:
    app_new_workspace(app, NULL, true);
    return true;
  case HOTKEY_TOGGLE_HISTORY: {
    ConnTab *t = app_current_tab(app);
    if (!t || !t->conn) return false;
    history_open(app);
    return true;
  }
  case HOTKEY_CONFIG:
    prefs_open(app);
    return true;
  case HOTKEY_VIEW_SWITCHER: {
    ConnTab *t = app_current_tab(app);
    if (!t || !t->conn) return false;
    view_switcher_open(app);
    return true;
  }
  case HOTKEY_NEXT_WORKSPACE:
  case HOTKEY_PREV_WORKSPACE:
    if (app->num_workspaces < 2) {
      app_set_status(app, false, "Only one workspace");
      return true;
    }
    app_switch_workspace(app, action == HOTKEY_NEXT_WORKSPACE
                                  ? (app->current_ws + 1) % app->num_workspaces
                                  : (app->current_ws + app->num_workspaces - 1) % app->num_workspaces);
    return true;
  default: {
    if (hotkey_get_category(action) == HOTKEY_CAT_CONNECT) return form_tab_do_action(app, app_current_tab(app), action);
    View *cv = app_current_view(app);
    if (cv && cv->kind == VIEW_QUERY && query_view_do_action(app, cv, action)) return true;
    if (cv && cv->kind == VIEW_TABLE && subviews_do_action(app, cv, action)) return true;
    if (cv && cv->kind == VIEW_TABLE && cv->subview != SV_DATA) return false; /* the grid is behind a list */
    return grid_do_action(app, cv, action);
  }
  }
}

bool app_action_applies(const NcApp *app, HotkeyAction action) {
  const ConnTab *t = app->num_workspaces ? workspace_current_tab(app->workspaces[app->current_ws]) : NULL;
  if (hotkey_get_category(action) == HOTKEY_CAT_CONNECT) return t && t->form; /* the form's own keys */
  switch (action) {
  case HOTKEY_ADD_FILTER: { /* its keys are the add-row keys: the panel takes them when it has the focus */
    const View *v = t ? conntab_current_view(t) : NULL;
    return v && v->kind == VIEW_TABLE && v->filters_visible && v->fp.focused;
  }
  case HOTKEY_REMOVE_FILTER: {
    const View *v = t ? conntab_current_view(t) : NULL;
    return v && v->kind == VIEW_TABLE && v->filters_visible && v->fp.focused && v->num_filters > 0;
  }
  case HOTKEY_OPEN_QUERY: case HOTKEY_SIDEBAR_FILTER:
  case HOTKEY_TOGGLE_SIDEBAR: case HOTKEY_TOGGLE_HISTORY: case HOTKEY_VIEW_SWITCHER:
    return t != NULL && t->conn != NULL;
  case HOTKEY_SWITCH_DATABASE: /* a server's databases; a SQLite file is one */
    return t != NULL && t->conn != NULL && nc_connection_driver(t->conn) != LACE_DRIVER_SQLITE;
  case HOTKEY_FOLLOW_FK: case HOTKEY_REFERENCED_BY: { /* a table's rows: Data, or results that are one table's */
    const View *v = t ? conntab_current_view(t) : NULL;
    return v && (v->kind == VIEW_TABLE ? v->subview == SV_DATA && view_has_subview(v, SV_DATA) : v->source_table != NULL);
  }
  case HOTKEY_EXECUTE_QUERY: case HOTKEY_EXECUTE_ALL: case HOTKEY_EXECUTE_TRANSACTION:
  case HOTKEY_SPLIT_UP: case HOTKEY_SPLIT_DOWN: {
    const View *v = t ? conntab_current_view(t) : NULL;
    return v && v->kind == VIEW_QUERY;
  }
  case HOTKEY_NEW_TAB: /* the sidebar's highlighted table: only while the sidebar has the keys
                        * (its keys are the add-row keys, which the grid takes otherwise) */
    return t != NULL && t->conn != NULL && t->sidebar_focused && sidebar_width(app) > 0;
  case HOTKEY_PREV_TAB: case HOTKEY_NEXT_TAB:
  case HOTKEY_CLOSE_TAB: case HOTKEY_NEXT_CONN_TAB: case HOTKEY_PREV_CONN_TAB:
  case HOTKEY_CONN_TAB_1: case HOTKEY_CONN_TAB_2: case HOTKEY_CONN_TAB_3:
  case HOTKEY_CONN_TAB_4: case HOTKEY_CONN_TAB_5: case HOTKEY_CONN_TAB_6:
  case HOTKEY_CONN_TAB_7: case HOTKEY_CONN_TAB_8: case HOTKEY_CONN_TAB_9:
    return t != NULL;
  default: break;
  }
  switch (hotkey_get_category(action)) {
  case HOTKEY_CAT_TABLE:
  case HOTKEY_CAT_NAVIGATION:
  case HOTKEY_CAT_FILTERS: {
    const View *v = t ? conntab_current_view(t) : NULL;
    return v && v->kind == VIEW_TABLE && subview_action_applies(v, action);
  }
  case HOTKEY_CAT_QUERY:
  case HOTKEY_CAT_EDITOR:
    return t && conntab_current_view(t) != NULL;
  default:
    return true;
  }
}

void app_close_view(NcApp *app, size_t index) {
  NcWorkspace *ws = app_current_workspace(app);
  ConnTab *t = app_current_tab(app);
  if (!t) return;
  if (t->num_views > 1 && index < t->num_views) conntab_remove_view(t, index);
  else if (t->num_views <= 1) app_close_tab(app, ws->current); /* the last view, or none: the tab goes */
}

void app_switch_view(NcApp *app, size_t index) {
  ConnTab *t = app_current_tab(app);
  View *cur = t ? conntab_current_view(t) : NULL;
  if (cur) cur->split_drag = false;
  if (t && index < t->num_views) {
    t->current = index;
    t->sidebar_focused = false;
  }
}

/* A key the focused sidebar passes on may not act on the grid or the editor
 * behind it: only the general keys run there (and the two that leave it) */
static bool sidebar_keeps(HotkeyAction act) {
  if (act == HOTKEY_TOGGLE_SIDEBAR || act == HOTKEY_OPEN_QUERY) return false;
  switch (hotkey_get_category(act)) {
  case HOTKEY_CAT_TABLE: case HOTKEY_CAT_NAVIGATION: case HOTKEY_CAT_FILTERS:
  case HOTKEY_CAT_QUERY: case HOTKEY_CAT_EDITOR:
    return true;
  default:
    return false;
  }
}

/* Every action a key press can run (the palette lists the rest) */
static bool dispatch_hotkeys(NcApp *app, const UiEvent *ev) {
  const ConnTab *t = app_current_tab(app);
  bool in_sidebar = t && t->sidebar_focused && sidebar_width(app);
  for (int a = 0; a < HOTKEY_COUNT; a++) {
    HotkeyAction act = (HotkeyAction)a;
    if (!(hotkey_action_frontends(act) & CONFIG_FE_NOTCURSES)) continue;
    if (in_sidebar && sidebar_keeps(act)) continue;
    if (hotkey_get_category(act) == HOTKEY_CAT_CONNECT) continue; /* the form takes its own keys */
    if (!hotkey_matches(app->config, ev, act) || !app_action_applies(app, act)) continue;
    if (app_do_action(app, act)) return true;
  }
  return false;
}

/* A mouse event with no overlay open: a left press on the header's tab names
 * switches tabs, on a status hint runs its action; the rest goes to the
 * content */
static void handle_mouse(NcApp *app, const UiEvent *ev) {
  View *cv = app_current_view(app);
  bool press = ev->mouse.action == UI_MOUSE_PRESS && ev->mouse.button == UI_MOUSE_LEFT;
  if (cv && cv->split_drag) { /* a divider drag owns the mouse until the release, wherever it goes */
    query_view_handle_mouse(app, cv, ev, grid_content_rect(app));
    return;
  }
  if (!(cv && cv->editing)) {
    if (tabstrip_handle_mouse(app, ev)) return;
    if (ev->mouse.y == 0) { /* the header: its database segment opens the switch */
      if (press && hit_find(&app->header_spans, ev->mouse.x) == 0) app_switch_database(app);
      return;
    }
    if (ev->mouse.y == app->rows - 1) {
      int act = press ? hit_find(&app->status_spans, ev->mouse.x) : -1;
      if (act == HINT_FOCUS_EDITOR || act == HINT_FOCUS_RESULTS) {
        if (cv && cv->kind == VIEW_QUERY) cv->focus_results = act == HINT_FOCUS_RESULTS;
      } else if (act >= 0 && act < HOTKEY_COUNT && app_action_applies(app, (HotkeyAction)act)) {
        app_do_action(app, (HotkeyAction)act);
      }
      return;
    }
  }
  content_handle_mouse(app, ev);
}

void app_handle_event(NcApp *app, const UiEvent *ev) {
  if (ev->type == UI_EVENT_RESIZE) {
    screen_resized(app);
    return;
  }
  if (ev->type == UI_EVENT_QUIT) {
    app_quit(app);
    return;
  }
  if (screen_too_small(app)) { /* only "Terminal too small" is shown: q quits, nothing else listens */
    if (ev->type == UI_EVENT_KEY && !ev->key.is_special && ev->key.key == 'q' &&
        !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
      if (app_quit_needs_confirm(app)) /* the confirmation cannot be shown */
        app_set_status(app, true, "Enlarge the terminal to quit (a confirmation is needed)");
      else
        app_quit(app);
    }
    return;
  }
  if (app->num_overlays > 0 && overlay_handle_event(app, ev)) return;
  if (ev->type == UI_EVENT_MOUSE) {
    handle_mouse(app, ev);
    return;
  }
  if (ev->type == UI_EVENT_KEY) {
    View *cv = app_current_view(app);
    if (cv) cv->split_drag = false; /* a key ends a drag whose release was lost */
    if (cv && cv->editing && grid_edit_key(app, cv, ev)) return; /* the field owns every key */
    /* Esc stops the statement that is running; the rows stay */
    if (ev->key.is_special && ev->key.key == UI_KEY_ESCAPE && app_cancel_running(app)) return;
    if (content_handle_key(app, ev)) return;
    dispatch_hotkeys(app, ev);
  }
}

static void flush_held(NcApp *app) {
  int n = app->num_held;
  app->num_held = 0;
  for (int i = 0; i < n; i++) app_handle_event(app, &app->held[i]);
}

/* Escape sequences notcurses 3.0 does not know arrive as typed text: a focus
 * report (`CSI I` / `CSI O`, mode 1004, which this app never asks for but a
 * program before it may have left on) and an SGR mouse report with the
 * pointer outside the pane (`CSI < 0 ; -6 ; -6 m`, which some emulators send
 * when a click lands on another pane and notcurses cannot parse) came as a
 * bare Esc, `[` and the bytes of the sequence, so one click elsewhere closed
 * overlays, switched tabs (`[`) and closed views (`-`). An Esc is kept back
 * for FOCUS_HOLD_MS; a `[` after it, then parameter and intermediate bytes
 * (0x20..0x3f), extend the hold, and a final byte (0x40..0x7e) ends the
 * sequence, which is dropped (a mouse release report becomes a real release,
 * so a divider drag still ends). Anything else, or the time, lets the held
 * keys through in order. */
static bool plain_char_between(const UiEvent *ev, int lo, int hi) {
  return ev->type == UI_EVENT_KEY && !ev->key.is_special && ev->key.mods == UI_MOD_NONE &&
         ev->key.key >= lo && ev->key.key <= hi;
}

static void hold(NcApp *app, const UiEvent *ev) {
  app->held[app->num_held++] = *ev;
  app->held_since_ms = app_now_ms(); /* every byte gets its own FOCUS_HOLD_MS */
}

/* The held `CSI <text> <final>` is complete */
static void csi_finish(NcApp *app, int final) {
  char text[APP_HELD_MAX + 1];
  int n = 0;
  for (int i = 2; i < app->num_held; i++) text[n++] = (char)app->held[i].key.key;
  text[n] = 0;
  app->num_held = 0;
  if ((final == 'I' || final == 'O') && n == 0) return; /* a focus report */
  if (final == 'm' && text[0] == '<') { /* an SGR mouse release: < b ; x ; y, 1-based, clamped into the pane */
    long b = 0, x = 0, y = 0;
    if (sscanf(text, "<%ld;%ld;%ld", &b, &x, &y) == 3) {
      UiEvent ev = {0};
      ev.type = UI_EVENT_MOUSE;
      ev.mouse.action = UI_MOUSE_RELEASE;
      ev.mouse.button = (b & 3) == 1 ? UI_MOUSE_MIDDLE : (b & 3) == 2 ? UI_MOUSE_RIGHT : UI_MOUSE_LEFT;
      ev.mouse.x = x < 1 ? 0 : x > app->cols ? app->cols - 1 : (int)x - 1;
      ev.mouse.y = y < 1 ? 0 : y > app->rows ? app->rows - 1 : (int)y - 1;
      app_handle_event(app, &ev);
      return;
    }
  }
  app_log(app, false, "dropped a terminal sequence: ESC [ %s %c", text, final);
}

static void handle_or_hold(NcApp *app, const UiEvent *ev) {
  if (app->num_held == 0) {
    if (ev->type == UI_EVENT_KEY && ev->key.is_special && ev->key.key == UI_KEY_ESCAPE && ev->key.mods == UI_MOD_NONE) {
      hold(app, ev);
      return;
    }
  } else if (app->num_held == 1) {
    if (plain_char_between(ev, '[', '[')) {
      hold(app, ev);
      return;
    }
  } else {
    if (plain_char_between(ev, 0x20, 0x3f) && app->num_held < APP_HELD_MAX) { /* parameters, intermediates */
      hold(app, ev);
      return;
    }
    if (plain_char_between(ev, 0x40, 0x7e)) { /* the final byte */
      csi_finish(app, ev->key.key);
      return;
    }
  }
  flush_held(app);
  app_handle_event(app, ev);
}

/* Milliseconds before a held Esc stands for itself; the poll timeout while one is held */
static int app_input_hold_ms(const NcApp *app) {
  if (!app->num_held) return -1;
  int64_t left = FOCUS_HOLD_MS - (app_now_ms() - app->held_since_ms);
  return left > 0 ? (int)left : 0;
}

/* Read and translate every pending input event; with dispatch false they are
 * discarded. Returns false when notcurses keeps failing to read. */
/* LACE_NC_INPUT_TRACE=<file>: every event notcurses delivers, one line
 * each, appended, so a terminal whose keys arrive strangely can be looked at:
 *   <ms> id=<id> evtype=<0..3> mods=0x<hex> eff="<text>" -> <translated>
 * mods are notcurses' (1 Shift, 2 Alt, 4 Ctrl, 8 Super, 16 Hyper, 32 Meta,
 * 64 Caps, 128 Num); evtype 1 press, 2 repeat, 3 release; the translation
 * is "key <code> special=<0/1> mods=<ui>", "mouse", "resize" or "dropped". */
static void trace_input(const struct ncinput *ni, uint32_t id, bool ok, const UiEvent *ev) {
  static FILE *f;
  static bool looked;
  if (!looked) {
    looked = true;
    const char *path = getenv("LACE_NC_INPUT_TRACE");
    if (path && *path) f = fopen(path, "a");
  }
  if (!f) return;
  char eff[NCINPUT_MAX_EFF_TEXT_CODEPOINTS * 4 + 1] = "";
  size_t n = 0;
  while (n < NCINPUT_MAX_EFF_TEXT_CODEPOINTS && ni->eff_text[n]) n++;
  if (n) {
    int len = notcurses_ucs32_to_utf8(ni->eff_text, n, (unsigned char *)eff, sizeof eff - 1);
    eff[len > 0 ? len : 0] = 0;
  }
  fprintf(f, "%lld id=%u evtype=%d mods=0x%x eff=\"%s\" -> ", (long long)app_now_ms(), (unsigned)id,
          (int)ni->evtype, ni->modifiers, eff);
  if (!ok) fputs("dropped\n", f);
  else if (ev->type == UI_EVENT_KEY)
    fprintf(f, "key %d special=%d mods=%u\n", ev->key.key, ev->key.is_special, ev->key.mods);
  else if (ev->type == UI_EVENT_MOUSE) fprintf(f, "mouse x=%d y=%d\n", ev->mouse.x, ev->mouse.y);
  else fprintf(f, "event %d\n", (int)ev->type);
  fflush(f);
}

static bool pump_input(NcApp *app, bool dispatch) {
  struct ncinput ni;
  struct timespec zero = {0, 0};
  uint32_t id;
  for (;;) {
    errno = 0; /* a stale EINTR must not hide a persistent failure */
    id = notcurses_get(app->nc, &zero, &ni);
    if (id == 0) break;
    if (id == (uint32_t)-1) {
      if (errno == EINTR) return true;
      return ++app->input_errors < MAX_INPUT_ERRORS;
    }
    app->input_errors = 0;
    UiEvent ev;
    bool ok = input_translate(&ni, id, &ev);
    trace_input(&ni, id, ok, &ev);
    if (dispatch && ok) handle_or_hold(app, &ev);
  }
  if (app->num_held && app_input_hold_ms(app) == 0) flush_held(app); /* nothing followed: a real Esc */
  return true;
}

int app_run(NcApp *app) {
  char *err = NULL;
  if (!screen_init(app, &err)) {
    fprintf(stderr, "lace-nc: %s\n", err ? err : "cannot start notcurses");
    free(err);
    return 1;
  }
  app_connect_targets(app);
  if (!app->num_targets && !app->no_session && app->config->general.restore_session) {
    char *serr = NULL;
    cJSON *root = nc_session_load(&serr);
    if (root) {
      nc_session_restore(app, root, NULL);
      cJSON_Delete(root);
    } else if (serr) {
      app_set_status(app, true, "%s", serr);
    }
    free(serr);
  }
  /* Nothing to restore or connect to: the app opens on a connection form */
  NcWorkspace *first = app_current_workspace(app);
  if (!app->num_targets && !app->restore && first && !first->num_tabs) workspace_add_form(first, SIZE_MAX, true);
  int64_t start = app_now_ms();
  bool draining = true;
  bool input_failed = false;
  while (!app->quit) {
    /* Draw nothing until the startup replies are drained, so whoever waits
     * for the first screen (a test) cannot type into the discard window */
    if (app_now_ms() - start >= STARTUP_DRAIN_MS) screen_render(app);
    struct pollfd fds[2] = {{notcurses_inputready_fd(app->nc), POLLIN, 0}, {mainq_fd(app->mainq), POLLIN, 0}};
    int timeout = app_busy(app) || app->status_until_ms || sidebar_animating() ? 100 : -1;
    if (app_now_ms() - start < STARTUP_DRAIN_MS) timeout = 50;
    int hold = app_input_hold_ms(app);
    if (hold >= 0 && (timeout < 0 || hold < timeout)) timeout = hold;
    int r = poll(fds, 2, timeout);
    if (r < 0 && errno != EINTR) break;
    bool done = app_now_ms() - start >= STARTUP_DRAIN_MS;
    if (done && draining) { /* resizes during the window were discarded */
      draining = false;
      screen_resized(app);
    }
    mainq_drain(app->mainq);
    if (!pump_input(app, done)) {
      app_log(app, true, "input: notcurses_get keeps failing");
      input_failed = true;
      break;
    }
    app_auto_open(app, app_current_tab(app)); /* a tab that waited until it became current */
    if (app_busy(app)) app->spinner++;
    if (app->status_until_ms && app_now_ms() > app->status_until_ms) {
      free(app->status);
      app->status = NULL;
      app->status_until_ms = 0;
    }
  }
  screen_stop(app);
  /* Saved only when it is restored next time (as the ncurses TUI); a restore
   * still running holds only part of the session: keep the file */
  if (app->config->general.restore_session && !app->restore) {
    char *serr = NULL;
    if (!nc_session_save(app, &serr)) fprintf(stderr, "lace-nc: %s\n", serr ? serr : "cannot save the session");
    free(serr);
  }
  {
    char *rerr = NULL;
    if (!recent_save(&app->recent, &rerr)) fprintf(stderr, "lace-nc: recent: %s\n", rerr ? rerr : "cannot save");
    free(rerr);
  }
  if (input_failed) {
    fprintf(stderr, "lace-nc: cannot read input\n");
    return 1;
  }
  return 0;
}
