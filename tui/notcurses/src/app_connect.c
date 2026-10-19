/* Connecting, connection tabs and workspaces */
#include "app.h"
#include "conn/connection.h"
#include "conn/mainq.h"
#include "core/connstr_display.h"
#include "model/workspace.h"
#include "ui/dialogs/confirm.h"
#include "ui/conn_picker.h"
#include "ui/dialogs/prompt.h"
#include "ui/overlay.h"
#include "util/connstr.h"
#include "util/mem.h"
#include "util/str.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Connecting
 * ========================================================================== */

typedef struct ConnectReq {
  struct ConnectReq *next; /* app->connects */
  NcApp *app;
  char *connstr, *saved_id, *name;
  LaceSavedConnection *model; /* to rebuild the string with a password; may be NULL */
  char *db_pw, *ssh_pw;       /* typed at a retry */
  NcDest dest;
  bool test_only;
  ConnectDoneFn done;
  void *data;
  int conn_id, rc;
  char *error;
  bool cancelled, finished, joined;
  pthread_t thread;
  atomic_int thread_done; /* set by the thread as its last act */
  Overlay *progress;
} ConnectReq;


static void req_free(ConnectReq *r) {
  str_secure_free(r->connstr);
  free(r->saved_id);
  free(r->name);
  if (r->model) lace_saved_conn_free(r->model);
  str_secure_free(r->db_pw);
  str_secure_free(r->ssh_pw);
  free(r->error);
  free(r);
}

static void req_unlink(NcApp *app, ConnectReq *r) {
  for (ConnectReq **pp = &app->connects; *pp; pp = &(*pp)->next)
    if (*pp == r) { *pp = r->next; break; }
  r->next = NULL;
}

/* The done callback, once, and not while quitting */
static void req_done(ConnectReq *r, ConnTab *tab, const char *error) {
  ConnectDoneFn fn = r->done;
  r->done = NULL;
  if (fn && !r->app->quit) fn(r->app, tab, error, r->data);
}

/* The rule of the ncurses session restore, plus ssh's own refusal */
static bool is_auth_error(const char *err) {
  if (!err) return false;
  return strstr(err, "password authentication failed") || strstr(err, "authentication failed") ||
         strstr(err, "no password supplied") || strstr(err, "FATAL:  password") ||
         strstr(err, "Access denied");
}

static void connect_finished(void *arg);

static void *connect_thread(void *arg) {
  ConnectReq *r = arg;
  int id = 0;
  r->rc = lace_connect(r->app->client, r->connstr, NULL, &id);
  r->conn_id = id;
  if (r->rc != LACE_OK) {
    const char *e = lace_client_error(r->app->client);
    r->error = str_dup(e && *e ? e : "Connection failed");
  }
  mainq_post(r->app->mainq, connect_finished, r);
  atomic_store(&r->thread_done, 1);
  return NULL;
}

static void progress_draw(Overlay *o, NcApp *app) {
  ConnectReq *r = o->data;
  char *d = connstr_display(r->connstr);
  char *msg = str_printf("%s %s\xe2\x80\xa6", r->test_only ? "Testing" : "Connecting to", d ? d : "the database");
  nc_style(o->plane, &app->theme, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
  nc_put(o->plane, 1, 2, o->cols - 2, msg);
  free(msg);
  free(d);
}

static void progress_layout(Overlay *o, NcApp *app) {
  o->cols = app->cols - 8 < 60 ? app->cols - 8 : 60;
  if (o->cols < 20) o->cols = 20;
  o->rows = 1;
}

static bool progress_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  (void)o; (void)app;
  /* Esc is the overlay default (pop -> closed -> cancel); the rest is eaten */
  return !(ev->type == UI_EVENT_KEY && ev->key.is_special && ev->key.key == UI_KEY_ESCAPE);
}

static void progress_closed(Overlay *o, NcApp *app) {
  ConnectReq *r = o->data;
  if (!r) return;
  r->progress = NULL;
  if (r->finished) return;
  r->cancelled = true;
  if (!app->quit) app_set_status(app, false, "Connection cancelled");
  req_done(r, NULL, "Connection cancelled");
}

static void start_request(ConnectReq *r) {
  NcApp *app = r->app;
  r->finished = r->cancelled = r->joined = false;
  atomic_store(&r->thread_done, 0);
  r->rc = 0;
  free(r->error);
  r->error = NULL;
  Overlay *o = overlay_new(r->test_only ? "Testing connection" : "Connecting");
  o->footer = "Esc cancel";
  o->data = r;
  o->layout = progress_layout;
  o->draw = progress_draw;
  o->key = progress_key;
  o->closed = progress_closed;
  r->progress = o;
  r->next = app->connects;
  app->connects = r;
  overlay_push(app, o);
  if (pthread_create(&r->thread, NULL, connect_thread, r) != 0) {
    r->rc = LACE_ERR_CONNECTION_FAILED;
    r->error = str_dup("cannot start the connection thread");
    r->joined = true;
    mainq_post(app->mainq, connect_finished, r);
  }
}

typedef struct { ConnectReq *r; bool ssh; } PwCtx;

static void start_request(ConnectReq *r);

static void password_answered(NcApp *app, char *text, void *data) {
  PwCtx *ctx = data;
  ConnectReq *r = ctx->r;
  bool ssh = ctx->ssh;
  free(ctx);
  if (!text || app->quit) {
    if (text) str_secure_free(text);
    if (!app->quit) app_set_status(app, false, "Connection cancelled");
    req_done(r, NULL, "Connection cancelled");
    req_free(r);
    return;
  }
  if (ssh) {
    str_secure_free(r->ssh_pw);
    r->ssh_pw = text;
  } else {
    str_secure_free(r->db_pw);
    r->db_pw = text;
  }
  char *cs = lace_saved_conn_build_connstr(r->model, r->db_pw, r->ssh_pw);
  if (!cs) {
    app_set_status(app, true, "cannot rebuild the connection string");
    req_done(r, NULL, "cannot rebuild the connection string");
    req_free(r);
    return;
  }
  str_secure_free(r->connstr);
  r->connstr = cs;
  start_request(r);
}

static void ask_password(ConnectReq *r, bool ssh) {
  char *d = connstr_display(r->connstr);
  char *label = str_printf("%s for %s", ssh ? "SSH password" : "Password", d ? d : "the database");
  free(d);
  PwCtx *ctx = safe_calloc(1, sizeof *ctx);
  ctx->r = r;
  ctx->ssh = ssh;
  prompt_open(r->app, ssh ? "SSH password" : "Password", label, "", true, password_answered, ctx);
  free(label);
}

static void connect_succeeded(ConnectReq *r) {
  NcApp *app = r->app;
  if (r->test_only) {
    lace_disconnect(app->client, r->conn_id);
    app_set_status(app, false, "Connection OK");
    req_done(r, NULL, NULL);
    return;
  }
  NcConnection *c = nc_connection_new(app->client, app->mainq, r->conn_id, r->connstr, r->saved_id,
                                      r->name, app->config->general.history_mode,
                                      app->config->general.history_max_size);
  nc_connection_set_listener(c, app_conn_event, app);
  nc_connection_refresh_tables(c);
  ConnTab *tab = app_place_connection(app, c, r->dest);
  if (!tab) { /* nobody holds it: closed again */
    app_detach_connection(app, c);
    req_done(r, NULL, "The tab was closed before the connection opened");
    return;
  }
  if (!r->saved_id && recent_add(&app->recent, r->connstr)) {
    char *err = NULL;
    if (!recent_save(&app->recent, &err)) app_log(app, true, "recent: %s", err ? err : "cannot save");
    free(err);
    cp_changed();
  }
  req_done(r, tab, NULL);
}

/* True: a password prompt took the request over */
static bool connect_failed(ConnectReq *r) {
  NcApp *app = r->app;
  const char *err = r->error ? r->error : "Connection failed";
  app_set_status(app, true, "%s", err);
  if (!r->model) {
    char *perr = NULL;
    r->model = lace_saved_conn_from_connstr(r->connstr, &perr);
    free(perr);
  }
  bool network = r->model && r->model->driver && strcmp(r->model->driver, "sqlite") != 0;
  /* ssh's own refusal asks for the SSH password, and only for a +ssh string */
  bool ssh_refused = r->model && r->model->ssh && strstr(err, "Permission denied") != NULL;
  if (network && (is_auth_error(err) || ssh_refused)) {
    ask_password(r, ssh_refused);
    return true;
  }
  req_done(r, NULL, err);
  return false;
}

static void connect_finished(void *arg) {
  ConnectReq *r = arg;
  NcApp *app = r->app;
  if (!r->joined) {
    pthread_join(r->thread, NULL);
    r->joined = true;
  }
  req_unlink(app, r);
  if (r->cancelled || app->quit) { /* the answer is not wanted */
    if (r->progress) r->progress->data = NULL; /* the overlay outlives us at exit */
    if (r->rc == LACE_OK) lace_disconnect(app->client, r->conn_id);
    req_free(r);
    return;
  }
  r->finished = true;
  if (r->progress) overlay_pop_this(app, r->progress);
  if (r->rc == LACE_OK) {
    connect_succeeded(r);
  } else if (connect_failed(r)) {
    return;
  }
  req_free(r);
}

static ConnectReq *req_new(NcApp *app, const char *connstr, const char *saved_id, const char *name,
                           NcDest dest, ConnectDoneFn done, void *data) {
  ConnectReq *r = safe_calloc(1, sizeof *r);
  r->app = app;
  r->connstr = str_dup(connstr);
  r->saved_id = saved_id && *saved_id ? str_dup(saved_id) : NULL;
  r->name = name && *name ? str_dup(name) : NULL;
  r->dest = dest;
  r->done = done;
  r->data = data;
  return r;
}

/* An open connection with the same key takes the request: placed at once */
static bool share_open(NcApp *app, const char *saved_id, const char *connstr, NcDest dest, ConnectDoneFn done,
                       void *data) {
  NcConnection *c = app_conn_lookup(app, saved_id, connstr);
  if (!c) return false;
  ConnTab *tab = app_place_connection(app, c, dest);
  if (done) done(app, tab, tab ? NULL : "The tab was closed before the connection opened", data);
  return true;
}

void app_connect_url(NcApp *app, const char *connstr, const char *saved_id, const char *name,
                     NcDest dest, ConnectDoneFn done, void *data) {
  if (share_open(app, saved_id, connstr, dest, done, data)) return;
  start_request(req_new(app, connstr, saved_id, name, dest, done, data));
}

void app_connect_saved(NcApp *app, const char *saved_id, NcDest dest, ConnectDoneFn done, void *data) {
  ConnectionItem *it = app->connmgr && saved_id ? connmgr_find_by_id(app->connmgr, saved_id) : NULL;
  if (!it || !connmgr_is_connection(it)) {
    app_set_status(app, true, "Saved connection not found");
    if (done) done(app, NULL, "Saved connection not found", data);
    return;
  }
  char *cs = lace_saved_conn_build_connstr(&it->connection, NULL, NULL);
  if (!cs) {
    app_set_status(app, true, "Cannot build the connection string");
    if (done) done(app, NULL, "Cannot build the connection string", data);
    return;
  }
  if (share_open(app, saved_id, cs, dest, done, data)) {
    str_secure_free(cs);
    return;
  }
  ConnectReq *r = req_new(app, cs, saved_id, it->connection.name, dest, done, data);
  r->model = lace_saved_conn_copy(&it->connection);
  str_secure_free(cs);
  start_request(r);
}

void app_test_connection(NcApp *app, const char *connstr, const LaceSavedConnection *model,
                         ConnectDoneFn done, void *data) {
  ConnectReq *r = req_new(app, connstr, NULL, NULL, (NcDest){NC_DEST_APPEND, 0, NULL}, done, data);
  r->test_only = true;
  if (model) r->model = lace_saved_conn_copy(model);
  start_request(r);
}

char *app_target_to_connstr(const char *text, char **error) {
  while (text && (*text == ' ' || *text == '\t')) text++;
  if (!text || !*text) {
    if (error) *error = str_dup("Nothing to connect to");
    return NULL;
  }
  char *t = str_dup(text);
  size_t n = strlen(t);
  while (n && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\n')) t[--n] = 0;
  if (strstr(t, "://") || strncmp(t, "sqlite:", 7) == 0) return t;
  if (connstr_is_sqlite_file(t)) {
    char *cs = connstr_from_path(t, error);
    str_secure_free(t);
    return cs;
  }
  if (error) *error = str_dup("Not a connection URL or an existing SQLite file");
  str_secure_free(t);
  return NULL;
}

static void target_done(NcApp *app, ConnTab *tab, const char *error, void *data) {
  (void)tab; (void)error; (void)data;
  app_connect_targets(app);
}

void app_connect_targets(NcApp *app) {
  while (app->next_target < app->num_targets) {
    char *err = NULL;
    char *cs = app_target_to_connstr(app->targets[app->next_target++], &err);
    if (!cs) {
      app_set_status(app, true, "%s", err ? err : "Not a connection");
      free(err);
      continue;
    }
    app_connect_url(app, cs, NULL, NULL, (NcDest){NC_DEST_APPEND, 0, NULL}, target_done, NULL);
    str_secure_free(cs);
    return;
  }
  /* The last one is done: a workspace none of them reached gets a form */
  NcWorkspace *ws = app_current_workspace(app);
  if (app->num_targets && ws && !ws->num_tabs) workspace_add_form(ws, SIZE_MAX, true);
}

static void free_closing(NcApp *app) {
  for (size_t i = 0; i < app->num_closing; i++) nc_connection_free(app->closing[i]);
  free(app->closing);
  app->closing = NULL;
  app->num_closing = 0;
}

void app_connect_cleanup(NcApp *app) {
  /* An attempt in flight must end before the client and the queue go; one
   * that does not end soon (a server that never answers) is abandoned: its
   * thread keeps the client, the queue and its request, which are then not
   * freed, since the process is about to exit */
  struct timespec step = {0, 10 * 1000000};
  for (int waited = 0; waited < 150; waited++) {
    bool running = false;
    for (ConnectReq *r = app->connects; r; r = r->next)
      if (!r->joined && !atomic_load(&r->thread_done)) running = true;
    if (!running) break;
    nanosleep(&step, NULL);
  }
  for (ConnectReq *r = app->connects; r; r = r->next) {
    if (r->joined) continue;
    if (atomic_load(&r->thread_done)) {
      pthread_join(r->thread, NULL);
      r->joined = true;
    } else {
      app->abandon = true;
    }
  }
  free_closing(app);
}
