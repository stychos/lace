/*
 * Lace notcurses frontend: the header's database segment
 *
 * A server connection sits in one database (PostgreSQL current_database(),
 * MySQL DATABASE()), which the header shows after the connection name. The
 * shared switch_database action, the palette and a click on the segment list
 * the databases the server lets this user into and switch the tab to the
 * picked one: the tab lets its connection go (a form takes its place, showing
 * the target) and connects there with the same credentials and tunnel, so a
 * connection to that database already open elsewhere is shared as usual.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "app.h"
#include "model/workspace.h"
#include "ui/dialogs/confirm.h"
#include "ui/dialogs/pick.h"
#include "ui/form_tab.h"
#include "util/mem.h"
#include "util/str.h"
#include <config/saved_connection.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  NcApp *app;
  uint64_t tab_uid;
  char **names; /* the server's databases: the pick list's rows */
  size_t n;
  char *connstr; /* the target, once one is picked */
  char *database;
  bool kept; /* handed on to an overlay: the job's destroy leaves it alone */
} DbSwitch;

static void db_switch_free(DbSwitch *s) {
  if (!s) return;
  lace_tables_free(s->names, s->n);
  str_secure_free(s->connstr);
  free(s->database);
  free(s);
}

static void db_switch_destroy(void *p) {
  DbSwitch *s = p;
  if (!s->kept) db_switch_free(s);
}

static void switch_done(NcApp *app, ConnTab *tab, const char *error, void *data) {
  DbSwitch *s = data;
  if (tab) app_set_status(app, false, "Switched to %s", s->database);
  else if (error) app_set_status(app, true, "%s", error);
  db_switch_free(s);
}

/* The tab lets its connection go, a form in its place showing the target,
 * and connects to the new database there */
static void switch_now(NcApp *app, DbSwitch *s) {
  ConnTab *t = app_find_tab(app, s->tab_uid, NULL);
  if (!t || !t->conn) {
    db_switch_free(s);
    return;
  }
  NcConnection *c = t->conn;
  t->conn = NULL;
  conntab_make_form(t);
  form_tab_set_connstr(t->form, s->connstr);
  app_conn_release(app, c);
  app_connect_url(app, s->connstr, NULL, NULL, (NcDest){NC_DEST_HERE, t->uid, NULL}, switch_done, s);
}

static void switch_confirmed(NcApp *app, bool yes, void *data) {
  if (yes) switch_now(app, data);
  else db_switch_free(data);
}

static void picked(NcApp *app, size_t index, void *data) {
  DbSwitch *s = data;
  ConnTab *t = index < s->n ? app_find_tab(app, s->tab_uid, NULL) : NULL;
  if (!t || !t->conn) {
    db_switch_free(s);
    return;
  }
  const char *cur = nc_connection_database(t->conn);
  if (cur && strcmp(cur, s->names[index]) == 0) {
    app_set_status(app, false, "Already in %s", cur);
    db_switch_free(s);
    return;
  }
  /* The same server, user, passwords and tunnel, another database */
  LaceSavedConnection *model = lace_saved_conn_from_connstr(nc_connection_connstr(t->conn), NULL);
  if (model) {
    free(model->database);
    model->database = str_dup(s->names[index]);
    s->connstr = lace_saved_conn_build_connstr(model, NULL, NULL);
    lace_saved_conn_free(model);
  }
  if (!s->connstr) {
    app_set_status(app, true, "Cannot build a connection string for %s", s->names[index]);
    db_switch_free(s);
    return;
  }
  s->database = str_dup(s->names[index]);
  if (app_conn_holders(app, t->conn) <= 1 && nc_connection_tx_state(t->conn) != NC_TX_NONE) {
    char *msg = str_printf("A transaction is open on %s. Switch to %s and roll back?",
                           nc_connection_name(t->conn), s->database);
    confirm_open(app, "Switch database", msg, "Switch", true, switch_confirmed, s);
    free(msg);
    return;
  }
  switch_now(app, s);
}

static void databases_done(NcJob *job, void *data) {
  DbSwitch *s = data;
  NcApp *app = s->app;
  ConnTab *t = app_find_tab(app, s->tab_uid, NULL);
  if (job->error) {
    app_set_status(app, true, "Databases: %s", job->error);
    return;
  }
  if (!t || !t->conn) return;
  if (!job->num_tables) {
    app_set_status(app, false, "No database to switch to");
    return;
  }
  s->names = job->tables; /* taken over */
  s->n = job->num_tables;
  job->tables = NULL;
  job->num_tables = 0;
  const char *cur = nc_connection_database(t->conn);
  char **rows = safe_calloc(s->n, sizeof *rows);
  for (size_t i = 0; i < s->n; i++)
    rows[i] = cur && strcmp(cur, s->names[i]) == 0 ? str_printf("%s  (current)", s->names[i]) : str_dup(s->names[i]);
  s->kept = true;
  pick_open(app, "Switch database", rows, s->n, picked, s);
  for (size_t i = 0; i < s->n; i++) free(rows[i]);
  free(rows);
}

void app_switch_database(NcApp *app) {
  ConnTab *t = app_current_tab(app);
  if (!t || !t->conn) return;
  if (nc_connection_driver(t->conn) == LACE_DRIVER_SQLITE) {
    app_set_status(app, false, "A SQLite file is one database");
    return;
  }
  DbSwitch *s = safe_calloc(1, sizeof *s);
  s->app = app;
  s->tab_uid = t->uid;
  nc_connection_submit_full(t->conn, nc_job_databases(), t->owner, databases_done, s, db_switch_destroy);
}
