#ifndef LACE_NC_APP_H
#define LACE_NC_APP_H
#include "config/config.h"
#include "config/connections.h"
#include "config/recent.h"
#include "config/saved_workspaces.h"
#include "conn/connection.h"
#include "core/ui_types.h"
#include "ui/theme.h"
#include <lace.h>
#include <stdbool.h>
#include <stdint.h>

struct notcurses; struct ncplane;
typedef struct MainQueue MainQueue;
typedef struct NcWorkspace NcWorkspace;
typedef struct Overlay Overlay;
typedef struct NcConnection NcConnection;
typedef struct ConnTab ConnTab;
typedef struct View View;
struct ConnectReq;
struct NcRestore;

typedef struct { char *line; bool error; int64_t at_ms; } LogLine;
/* A clickable stretch [x0, x1) of the header or status line, recorded by the draw */
typedef struct { int x0, x1, value; } HitSpan;
typedef struct { HitSpan *v; size_t n, cap; } HitSpans;
void hit_add(HitSpans *s, int x0, int x1, int value);
/* The index of the span holding column x, or -1 */
int hit_find(const HitSpans *s, int x);

typedef struct NcApp {
  struct notcurses *nc;
  struct ncplane *std;       /* the standard plane: header, content, status */
  int rows, cols;
  Theme theme;
  lace_client_t *client;
  Config *config;
  ConnectionManager *connmgr;
  RecentList recent;
  SavedWorkspaces saved_ws;
  NcWorkspace **workspaces;
  size_t num_workspaces, current_ws;
  MainQueue *mainq;
  Overlay **overlays;        /* bottom .. top */
  size_t num_overlays;
  LogLine *log; size_t num_log, log_cap;   /* ring of 500 */
  uint64_t log_seq;                        /* bumped by every app_log: the count stops moving once the ring is full */
  char *status; bool status_error; int64_t status_until_ms;
  int spinner;               /* frame counter while busy */
  bool quit;
  bool no_session;
  int input_errors;          /* consecutive notcurses_get failures */
  char **targets; size_t num_targets;      /* URLs/files from the command line */
  size_t next_target;                      /* the next one to connect to at startup */
  bool abandon;                            /* a connect thread is stuck: do not free what it uses */
  struct ConnectReq *connects;             /* connection attempts in flight */
  NcConnection **closing; size_t num_closing; /* disconnecting; freed on NC_CONN_EV_CLOSED */
  char *clipboard;                         /* the app's own copy (edit_ops.c) */
  void *welcome;                           /* ui/welcome.c state */
  HitSpans header_spans;                   /* value: the tab index; set by header_draw */
  HitSpans status_spans;                   /* value: a HotkeyAction; set by status_draw */
  HitSpans strip_spans, strip_close_spans; /* value: the view index; set by tabstrip_draw */
  struct NcRestore *restore;               /* session restores in flight (session.c) */
} NcApp;

NcApp *app_new(int argc, char **argv, char **error);
int app_run(NcApp *app);
void app_free(NcApp *app);
void app_set_status(NcApp *app, bool error, const char *fmt, ...);
void app_log(NcApp *app, bool error, const char *fmt, ...);
/* One log line for a finished job: "<elapsed> ms  <sql, 120 chars>  -> N rows | N affected | error";
 * call it before the result is stolen. label stands in when the job has no SQL. */
typedef struct NcJob NcJob;
void app_log_job(NcApp *app, const NcJob *job, const char *label);
bool app_busy(const NcApp *app);
void app_quit(NcApp *app);
bool app_quit_needs_confirm(const NcApp *app);
/* Status-hint actions that are not hotkeys (HitSpan values past HOTKEY_COUNT) */
#define HINT_FOCUS_EDITOR (HOTKEY_COUNT + 1)
#define HINT_FOCUS_RESULTS (HOTKEY_COUNT + 2)
int64_t app_now_ms(void);
/* Dispatch one translated event: overlays first, then the content */
void app_handle_event(NcApp *app, const UiEvent *ev);
/* Run a hotkey action by its table entry (the palette and the key handlers
 * both end here); returns false when it does not apply now */
bool app_do_action(NcApp *app, HotkeyAction action);
/* Whether an action makes sense now (the palette lists only those) */
bool app_action_applies(const NcApp *app, HotkeyAction action);
void connect_open(NcApp *app); /* the picker overlay (ui/dialogs/connect.c) */

/* Opening a table: switches to the open view of it (a preview of it is
 * pinned), else creates one and loads schema -> page 0 -> count */
View *app_open_table(NcApp *app, const char *table);
/* Open a table view behind the current one (the current view and the focus
 * stay); a table already open is only reported */
View *app_open_table_background(NcApp *app, const char *table);
/* The same in the preview tab: an open view of the table is switched to as
 * it is; else the view replaces the tab's preview view, or becomes it */
View *app_preview_table(NcApp *app, const char *table);
View *app_find_view(NcApp *app, uint64_t uid);
ConnTab *app_view_tab(NcApp *app, const View *v); /* the tab holding the view, or NULL */
/* Sort/filter/refresh: the pages are fetched again (the old rows stay shown,
 * dimmed, until the new first page lands), then the count */
void app_view_reload(NcApp *app, View *v);
void app_view_request_page(NcApp *app, View *v, size_t page); /* the cache's fetch target */
void app_view_count(NcApp *app, View *v);
/* The page cache's fetch target for a view that has none yet (query views) */
void app_view_attach(NcApp *app, View *v);

/* ---- query views (app_query.c) ---- */
View *app_open_query(NcApp *app, const char *text); /* a new query tab on the current connection */
/* One statement: paged when it only reads (page 0, then the count), else run
 * once; if the paged form fails it runs as written */
void app_query_run(NcApp *app, View *v, const char *sql);
/* Every statement of the editor text in turn, stopping at the first error;
 * transaction: BEGIN first, COMMIT after the last, ROLLBACK on error or cancel */
void app_query_run_all(NcApp *app, View *v, bool transaction);
/* Hooks of the page pipeline (app_table.c) for page 0 of a paged statement */
void app_query_first_page_ok(NcApp *app, View *v);
void app_query_first_page_failed(NcApp *app, View *v);
/* Esc: cancel the statement running on the current connection; false when idle */
bool app_cancel_running(NcApp *app);
void app_switch_view(NcApp *app, size_t index);
void app_close_view(NcApp *app, size_t index); /* the last view closes the connection tab */
/* Saved workspaces (session.c): the nested shape in workspaces.json; connections
 * without a saved id are left out and counted in the message */
void app_save_workspace_as(NcApp *app, size_t ws, const char *name);
void app_open_saved_workspace(NcApp *app, size_t index); /* restores it as a workspace and switches to it */
void app_delete_saved_workspace(NcApp *app, size_t index); /* asks first */

/* ---- connecting (app_connect.c) ---- */
/* conn is NULL when the attempt failed or was cancelled (error says why; NULL
 * for a finished test); the connection
 * belongs to its tab, done must not keep it */
typedef void (*ConnectDoneFn)(NcApp *app, NcConnection *conn, const char *error, void *data);
/* lace_connect runs on a short-lived thread behind a cancellable progress
 * overlay; an authentication refusal asks for the password and retries.
 * as_tab false: a new workspace when the current one already has tabs. */
void app_connect_url(NcApp *app, const char *connstr, const char *saved_id, const char *name,
                     bool as_tab, ConnectDoneFn done, void *data);
void app_connect_saved(NcApp *app, const char *saved_id, bool as_tab, ConnectDoneFn done, void *data);
/* Connect and disconnect again, reporting in the status line (done gets NULL).
 * model (may be NULL) lets a refused password be asked for. */
void app_test_connection(NcApp *app, const char *connstr, const LaceSavedConnection *model,
                         ConnectDoneFn done, void *data);
/* What the user typed as a target: a URL as it is, a SQLite file as a URL;
 * allocated, NULL (with *error) when it is neither */
char *app_target_to_connstr(const char *text, char **error);
/* Startup: connect to the command-line targets one after another */
void app_connect_targets(NcApp *app);
/* The connection leaves its tab's care: a running statement is cancelled, the
 * disconnect runs on its worker and app_conn_event frees it when done */
void app_detach_connection(NcApp *app, NcConnection *c);
void app_conn_event(NcConnection *c, NcConnEvent ev, const char *msg, void *data); /* the listener */
void app_connect_cleanup(NcApp *app); /* app_free: join attempts, free closing connections */

/* ---- tabs and workspaces ---- */
NcWorkspace *app_current_workspace(NcApp *app);
ConnTab *app_current_tab(NcApp *app);
View *app_current_view(NcApp *app);
void app_switch_workspace(NcApp *app, size_t i);
void app_new_workspace(NcApp *app, const char *name); /* NULL: "Workspace N"; becomes current */
void app_close_workspace(NcApp *app, size_t i);       /* confirms when a transaction is open */
void app_rename_workspace(NcApp *app, size_t i, const char *name);
void app_switch_tab(NcApp *app, size_t i);
void app_close_tab(NcApp *app, size_t i);             /* disconnects; confirms when a transaction is open */

/* ui/header.c, ui/status.c, ui/welcome.c */
void header_draw(NcApp *app);
void status_draw(NcApp *app);
void welcome_draw(NcApp *app);
bool welcome_handle_key(NcApp *app, const UiEvent *ev);
bool welcome_handle_mouse(NcApp *app, const UiEvent *ev);
void welcome_free(NcApp *app);
void content_draw(NcApp *app); /* the sidebar and the current view's grid or editor */
bool content_handle_key(NcApp *app, const UiEvent *ev);
bool content_handle_mouse(NcApp *app, const UiEvent *ev);
#endif
