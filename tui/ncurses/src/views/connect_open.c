/*
 * Lace
 * Connecting behind the progress dialog, asking for a password the server
 * refuses (the connection form tab and the session use it)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connect_open.h"
#include "../async/async.h"
#include "../tui_internal.h"
#include "prompt.h"
#include "form_tab.h"
#include "../config/recent.h"
#include "../../liblace/include/util/str.h"
#include <stdlib.h>
#include <string.h>

/* "Connection failed: <why>", unless why already says it failed */
char *tui_connect_failure_message(const char *why) {
  if (!why)
    return str_dup("Connection failed");
  if (strstr(why, "failed") && strstr(why, "failed") - why < 32)
    return str_dup(why);
  return str_printf("Connection failed: %s", why);
}

typedef enum { AUTH_NONE, AUTH_DB, AUTH_SSH } AuthNeed;

/* Did the server turn us away for want of a (right) password? */
static AuthNeed auth_need(const char *err) {
  if (!err)
    return AUTH_NONE;
  if (strstr(err, "SSH connection failed") &&
      (strstr(err, "Permission denied") || strstr(err, "authentication")))
    return AUTH_SSH;
  static const char *const db[] = {"password authentication failed",
                                   "authentication failed",
                                   "no password supplied",
                                   "FATAL:  password",
                                   "Access denied",
                                   NULL};
  for (int i = 0; db[i]; i++)
    if (strstr(err, db[i]))
      return AUTH_DB;
  return AUTH_NONE;
}

/* One attempt, on a worker behind the progress dialog (Esc cancels) */
static DbConnection *connect_once(TuiState *state, const char *connstr,
                                  char **err, bool *cancelled) {
  *err = NULL;
  *cancelled = false;
  lace_client_t *client = state->app->client;
  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_CONNECT;
  op.client = client;
  op.connstr = str_dup(connstr);
  if (!async_start(&op)) {
    str_secure_free(op.connstr);
    op.connstr = NULL;
    *err = str_dup("Could not start connecting");
    return NULL;
  }
  bool done = tui_show_processing_dialog_ex(state, &op, "Connecting...", 0);
  DbConnection *conn = NULL;
  if (done && op.state == ASYNC_STATE_COMPLETED) {
    conn = (DbConnection *)op.result;
    op.result = NULL;
  } else if (op.state == ASYNC_STATE_CANCELLED || !done) {
    *cancelled = true;
  } else {
    *err = str_dup(op.error ? op.error : "Connection failed");
  }
  str_secure_free(op.connstr); /* Holds passwords */
  op.connstr = NULL;
  async_free(&op);
  tui_refresh(state); /* The progress dialog drew over the screen */
  return conn;
}

/* Connect to model (a saved connection, or one parsed from a URL), first
 * with first_connstr; ask for a database or SSH password when the server
 * wants one and try again. On success *final_connstr is the string that
 * worked (with any password given here). */
DbConnection *tui_open_connection(TuiState *state, const SavedConnection *model,
                                     const char *first_connstr,
                                     char **final_connstr, char **err_out,
                                     bool *cancelled) {
  *final_connstr = NULL;
  *err_out = NULL;
  char *connstr = str_dup(first_connstr);
  char *db_pw = NULL, *ssh_pw = NULL;
  bool sqlite = model && model->driver && str_eq(model->driver, "sqlite");
  DbConnection *conn = NULL;
  for (int attempt = 0; attempt < 4 && connstr; attempt++) {
    char *err = NULL;
    conn = connect_once(state, connstr, &err, cancelled);
    if (conn || *cancelled) {
      free(err);
      break;
    }
    AuthNeed need = sqlite || !model ? AUTH_NONE : auth_need(err);
    if (need == AUTH_NONE || attempt == 3) {
      *err_out = err;
      break;
    }
    /* Ask for the password the server refused */
    const char *who_user = need == AUTH_SSH ? model->ssh_user : model->user;
    const char *who_host = need == AUTH_SSH ? model->ssh_host : model->host;
    char *label = str_printf("%s password%s%s%s%s:",
                             need == AUTH_SSH ? "SSH" : "Database",
                             (who_user && who_user[0]) || (who_host && who_host[0]) ? " for " : "",
                             who_user && who_user[0] ? who_user : "",
                             who_user && who_user[0] && who_host && who_host[0] ? "@" : "",
                             who_host && who_host[0] ? who_host : "");
    PromptOptions opts = {attempt ? "Wrong password? Try again" : "Password required",
                          label, NULL, NULL, true, true};
    char *pw = tui_prompt(&opts);
    free(label);
    tui_refresh(state);
    if (!pw) {
      *err_out = err;
      break;
    }
    free(err);
    if (need == AUTH_SSH) {
      str_secure_free(ssh_pw);
      ssh_pw = pw;
    } else {
      str_secure_free(db_pw);
      db_pw = pw;
    }
    str_secure_free(connstr);
    /* The saved-connection builder keeps an SSH tunnel and URL-encodes */
    connstr = lace_saved_conn_build_connstr(model, db_pw, ssh_pw);
  }
  str_secure_free(db_pw);
  str_secure_free(ssh_pw);
  if (conn)
    *final_connstr = connstr;
  else
    str_secure_free(connstr);
  return conn;
}

/* The first attempt's string for a saved connection: passwords typed this
 * session count even when they are not saved */
char *tui_saved_connstr(const SavedConnection *c) {
  return lace_saved_conn_build_connstr(
      c, c->password && c->password[0] ? c->password : NULL,
      c->ssh_password && c->ssh_password[0] ? c->ssh_password : NULL);
}

bool tui_test_connection(TuiState *state, const SavedConnection *conn, const char *first_connstr, char **msg) {
  char *connstr = first_connstr ? str_dup(first_connstr) : tui_saved_connstr(conn);
  if (!connstr) {
    *msg = str_dup("These fields do not make a valid connection");
    return false;
  }
  char *final = NULL, *err = NULL;
  bool cancelled = false;
  DbConnection *db = tui_open_connection(state, conn, connstr, &final, &err, &cancelled);
  str_secure_free(connstr);
  str_secure_free(final);
  if (db) {
    db_disconnect(db);
    *msg = str_dup("Connection works");
    return true;
  }
  *msg = cancelled ? str_dup("Test cancelled") : tui_connect_failure_message(err);
  free(err);
  return false;
}


bool tui_connect_and_place(TuiState *state, const SavedConnection *model, const char *saved_id,
                           const char *connstr, int kind, size_t ws_idx, size_t tab_idx, char **message,
                           bool *cancelled) {
  AppState *app = state->app;
  *message = NULL;
  *cancelled = false;
  size_t idx = app_find_open_connection(app, saved_id, connstr);
  if (idx == SIZE_MAX) {
    char *final = NULL, *err = NULL;
    DbConnection *db = tui_open_connection(state, model, connstr, &final, &err, cancelled);
    if (!db) {
      if (!*cancelled)
        *message = tui_connect_failure_message(err);
      free(err);
      return false;
    }
    free(err);
    idx = tui_add_open_connection(state, db, final, saved_id);
    if (!saved_id) {
      RecentList *rl = tui_recent(state);
      if (recent_add(rl, final)) {
        char *rerr = NULL;
        if (!recent_save(rl, &rerr))
          tui_set_error(state, "recent.json: %s", rerr ? rerr : "cannot save");
        free(rerr);
      }
    }
    str_secure_free(final);
    if (idx == SIZE_MAX) {
      *message = str_dup("The connection could not be added");
      return false;
    }
  } else if (!saved_id) {
    RecentList *rl = tui_recent(state);
    if (recent_add(rl, connstr))
      recent_save(rl, NULL);
  }
  /* PLACE_HERE turns the form at (ws_idx, tab_idx) into this tab */
  tui_place_connection(state, idx, (PlaceKind)kind, ws_idx, tab_idx);
  return true;
}
