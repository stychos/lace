#ifndef LACE_NC_APP_H
#define LACE_NC_APP_H
#include "core/chrome.h"
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
#define APP_HELD_MAX 40 /* keys kept back while they may be an escape sequence notcurses did not parse (pump_input) */
  UiEvent held[APP_HELD_MAX]; int num_held; int64_t held_since_ms;
  char **targets; size_t num_targets;      /* URLs/files from the command line */
  size_t next_target;                      /* the next one to connect to at startup */
  bool abandon;                            /* a connect thread is stuck: do not free what it uses */
  struct ConnectReq *connects;             /* connection attempts in flight */
  struct NcHeldConn { NcConnection *conn; size_t holders; } *conns; /* open connections, shared by tabs */
  size_t num_conns;
  NcConnection **closing; size_t num_closing; /* disconnecting; freed on NC_CONN_EV_CLOSED */
  char *clipboard;                         /* the app's own copy (edit_ops.c) */
  HitSpans header_spans;                   /* value 0: the database segment, 1: the view's name; set by header_draw */
  HitSpans status_spans;                   /* value: a HotkeyAction; set by status_draw */
  HitSpans strip_spans, strip_close_spans; /* value: the view index; set by tabstrip_draw */
  HitSpans subview_spans;                  /* value: a SubView; set by subviews_draw */
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
#define HINT_FOCUS_EDITOR CHROME_FOCUS_EDITOR /* core/chrome.h */
#define HINT_FOCUS_RESULTS CHROME_FOCUS_RESULTS
int64_t app_now_ms(void);
/* Dispatch one translated event: overlays first, then the content */
void app_handle_event(NcApp *app, const UiEvent *ev);
/* Run a hotkey action by its table entry (the palette and the key handlers
 * both end here); returns false when it does not apply now */
bool app_do_action(NcApp *app, HotkeyAction action);
/* Whether an action makes sense now (the palette lists only those) */
bool app_action_applies(const NcApp *app, HotkeyAction action);

/* Opening a table: switches to the open view of it (a preview of it is
 * pinned), else creates one and loads schema -> page 0 -> count */
View *app_open_table(NcApp *app, const char *table);
/* Open a table view behind the current one (the current view and the focus
 * stay); a table already open is only reported */
View *app_open_table_background(NcApp *app, const char *table);
/* The same in the preview tab: an open view of the table is switched to as
 * it is; else the view replaces the tab's preview view, or becomes it */
View *app_preview_table(NcApp *app, const char *table);
/* The same for a database view, routine or trigger (as lace_list_objects
 * names it; table: a trigger's, detail: a routine's kind), which get the
 * sub-views that fit them; a table is LACE_OBJ_TABLE */
View *app_open_object(NcApp *app, LaceObjectKind kind, const char *name, const char *table, const char *detail);
View *app_open_object_background(NcApp *app, LaceObjectKind kind, const char *name, const char *table, const char *detail);
View *app_preview_object(NcApp *app, LaceObjectKind kind, const char *name, const char *table, const char *detail);
/* Load the schema again (then page 0 and the count); the sub-views read it */
void app_view_reload_schema(NcApp *app, View *v);
View *app_find_view(NcApp *app, uint64_t uid);
ConnTab *app_view_tab(NcApp *app, const View *v); /* the tab holding the view, or NULL */
/* Sort/filter/refresh: the pages are fetched again (the old rows stay shown,
 * dimmed, until the new first page lands), then the count */
void app_view_reload(NcApp *app, View *v);
void app_view_request_page(NcApp *app, View *v, size_t page); /* the cache's fetch target */
void app_view_count(NcApp *app, View *v);
/* The page cache's fetch target for a view that has none yet (query views) */
void app_view_attach(NcApp *app, View *v);

/* ---- the sub-views' reads (app_catalog.c): asked for by the lists when
 * first drawn, answered into the view ---- */
void app_view_load_info(NcApp *app, View *v);       /* lace_table_info + the counter */
void app_view_load_deps(NcApp *app, View *v);       /* the tables referencing it */
void app_view_load_definition(NcApp *app, View *v); /* a view's, routine's or trigger's SQL */
void app_view_count_exact(NcApp *app, View *v);     /* Information's exact count */
void app_view_refresh_catalog(NcApp *app, View *v); /* drop them all and load the schema again */

/* ---- following foreign keys (app_follow.c) ---- */
/* The cursor column is the first column of a foreign key (the header's marker) */
bool view_column_is_fk(const View *v, const char *column);
void app_follow_fk(NcApp *app, View *v);      /* the referenced table, filtered by the cursor row's key */
void app_referenced_by(NcApp *app, View *v);  /* a pick list of the tables referencing this one */

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
void app_rename_saved_workspace(NcApp *app, size_t index, const char *name);

/* ---- shared connections (app_conns.c) ----
 * One database connection serves every tab, in any workspace, opened on the
 * same saved connection (by id) or the same URL (password-less string). Each
 * connected tab holds it; the last release closes it. */
/* NULL: none open. With a saved id, connstr (when given) must match too: an
 * entry edited since its connection opened is connected anew */
NcConnection *app_conn_lookup(NcApp *app, const char *saved_id, const char *connstr);
void app_conn_hold(NcApp *app, NcConnection *c); /* registers it on the first hold */
void app_conn_release(NcApp *app, NcConnection *c); /* the last release detaches (closes) it */
size_t app_conn_holders(const NcApp *app, const NcConnection *c);
/* "1, 3": the (1-based) workspaces whose tabs hold the saved connection; NULL when none */
char *app_conn_open_in(const NcApp *app, const char *saved_id);

/* Where a connection opens. APPEND: a new tab at the end of the current
 * workspace, current. HERE: the tab tab_uid becomes connected. BACKGROUND: a
 * new tab right after tab tab_uid, the current tab stays. NEW_WORKSPACE: a new
 * workspace holding only it. WORKSPACE: a new tab at the end of ws, which
 * becomes current. */
typedef enum { NC_DEST_APPEND, NC_DEST_HERE, NC_DEST_BACKGROUND, NC_DEST_NEW_WORKSPACE, NC_DEST_WORKSPACE } NcDestKind;
typedef struct { NcDestKind kind; uint64_t tab_uid; const NcWorkspace *ws; } NcDest;
/* Put an open connection where dest says and hold it for the tab; NULL (not
 * held, status set) when the destination is gone */
ConnTab *app_place_connection(NcApp *app, NcConnection *c, NcDest dest);
/* A tab waiting for its first table (auto_open): opened when it is current,
 * has no view and the table list is known */
void app_auto_open(NcApp *app, ConnTab *t);
/* The tab with that uid in any workspace (*wsi gets the workspace's index); NULL when gone */
ConnTab *app_find_tab(NcApp *app, uint64_t uid, size_t *wsi);

/* ---- connecting (app_connect.c) ---- */
/* tab is where the connection landed; NULL when the attempt failed, was
 * cancelled (error says why), was a test (error NULL when it worked) or its
 * destination was gone */
typedef void (*ConnectDoneFn)(NcApp *app, ConnTab *tab, const char *error, void *data);
/* A connection already open with the same key is placed at once and done runs
 * before these return; else lace_connect runs on a short-lived thread behind a
 * cancellable progress overlay, and an authentication refusal asks for the
 * password and retries. */
void app_connect_url(NcApp *app, const char *connstr, const char *saved_id, const char *name,
                     NcDest dest, ConnectDoneFn done, void *data);
void app_connect_saved(NcApp *app, const char *saved_id, NcDest dest, ConnectDoneFn done, void *data);
/* Connect and disconnect again, reporting in the status line (done gets NULL).
 * model (may be NULL) lets a refused password be asked for. */
void app_test_connection(NcApp *app, const char *connstr, const LaceSavedConnection *model,
                         ConnectDoneFn done, void *data);
/* Startup: connect to the command-line targets one after another */
void app_connect_targets(NcApp *app);
/* The connection leaves its tab's care: a running statement is cancelled, the
 * disconnect runs on its worker and app_conn_event frees it when done */
void app_detach_connection(NcApp *app, NcConnection *c);
void app_conn_event(NcConnection *c, NcConnEvent ev, const char *msg, void *data); /* the listener */
void app_connect_cleanup(NcApp *app); /* app_free: join attempts, free closing connections */

/* ---- the table's actions (app_table_menu.c): rename, truncate, drop, new
 * table/view/function, edit in a query view; the shared table_menu action,
 * the palette, a click on the header's view segment ---- */
void app_table_menu(NcApp *app);
/* The dependents a drop waited for are in (app_catalog.c): the confirmation */
void app_table_drop_continue(NcApp *app, View *v);

/* ---- the header's database segment (app_database.c) ---- */
/* List the server's databases and switch the current tab to the picked one
 * (the shared switch_database action, the palette, a click on the segment) */
void app_switch_database(NcApp *app);

/* ---- tabs and workspaces (app_tabs.c) ---- */
NcWorkspace *app_current_workspace(NcApp *app);
ConnTab *app_current_tab(NcApp *app);
View *app_current_view(NcApp *app);
void app_switch_workspace(NcApp *app, size_t i);
/* NULL: "Workspace N"; becomes current; with_form: it opens with a connection form */
void app_new_workspace(NcApp *app, const char *name, bool with_form);
void app_close_workspace(NcApp *app, size_t i);       /* confirms when a transaction is open */
void app_rename_workspace(NcApp *app, size_t i, const char *name);
void app_switch_tab(NcApp *app, size_t i);
/* Releases the tab's connection; confirms when that closes an open transaction */
void app_close_tab(NcApp *app, size_t i);

/* ui/header.c, ui/status.c */
void header_draw(NcApp *app);
void status_draw(NcApp *app);
void content_draw(NcApp *app); /* the form, or the sidebar and the current view's grid or editor */
/* The lock banner on the content's first row while a statement the user
 * asked for runs on the tab's connection; true when drawn */
bool content_banner_draw(NcApp *app, ConnTab *t);
/* Its text: the statement's first line cut to fit, the elapsed time, "Esc cancels" (allocated) */
char *content_banner_text(const char *what, int64_t elapsed_ms, int width);
bool content_handle_key(NcApp *app, const UiEvent *ev);
bool content_handle_mouse(NcApp *app, const UiEvent *ev);
#endif
