/* The open connections and who holds them */
#include "app.h"
#include "core/conn_fields.h"
#include "config/recent.h"
#include "model/workspace.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

static size_t find(const NcApp *app, const NcConnection *c) {
  for (size_t i = 0; i < app->num_conns; i++)
    if (app->conns[i].conn == c) return i;
  return SIZE_MAX;
}

NcConnection *app_conn_lookup(NcApp *app, const char *saved_id, const char *connstr) {
  for (size_t i = 0; i < app->num_conns; i++) {
    NcConnection *c = app->conns[i].conn;
    if (nc_connection_closed(c)) continue;
    const char *sid = nc_connection_saved_id(c);
    if (saved_id && *saved_id) {
      /* the same entry, still pointing where the open one does (it may have been edited since) */
      if (sid && !strcmp(sid, saved_id) &&
          (!connstr || conn_key_matches(NULL, connstr, NULL, nc_connection_connstr(c))))
        return c;
    } else if (conn_key_matches(NULL, connstr, sid, nc_connection_connstr(c))) {
      return c;
    }
  }
  return NULL;
}

void app_conn_hold(NcApp *app, NcConnection *c) {
  if (!c) return;
  size_t i = find(app, c);
  if (i == SIZE_MAX) {
    app->conns = safe_realloc(app->conns, (app->num_conns + 1) * sizeof *app->conns);
    app->conns[app->num_conns++] = (struct NcHeldConn){c, 1};
  } else {
    app->conns[i].holders++;
  }
}

void app_conn_release(NcApp *app, NcConnection *c) {
  size_t i = c ? find(app, c) : SIZE_MAX;
  if (i == SIZE_MAX) return;
  if (--app->conns[i].holders) return;
  memmove(app->conns + i, app->conns + i + 1, (app->num_conns - i - 1) * sizeof *app->conns);
  app->num_conns--;
  app_detach_connection(app, c);
}

size_t app_conn_holders(const NcApp *app, const NcConnection *c) {
  size_t i = find(app, c);
  return i == SIZE_MAX ? 0 : app->conns[i].holders;
}

char *app_conn_open_in(const NcApp *app, const char *saved_id) {
  if (!saved_id || !*saved_id) return NULL;
  char *out = NULL;
  for (size_t w = 0; w < app->num_workspaces; w++) {
    const NcWorkspace *ws = app->workspaces[w];
    bool here = false;
    for (size_t k = 0; k < ws->num_tabs && !here; k++) {
      const NcConnection *c = ws->tabs[k]->conn;
      const char *sid = c ? nc_connection_saved_id(c) : NULL;
      here = sid && !strcmp(sid, saved_id);
    }
    if (!here) continue;
    char *n = out ? str_printf("%s, %zu", out, w + 1) : str_printf("%zu", w + 1);
    free(out);
    out = n;
  }
  return out;
}

/* The connection leaves the app's care and is closed on its worker; it is
 * freed when the disconnect has run (NC_CONN_EV_CLOSED) or at exit */
void app_detach_connection(NcApp *app, NcConnection *c) {
  if (!c) return;
  app->closing = safe_realloc(app->closing, (app->num_closing + 1) * sizeof *app->closing);
  app->closing[app->num_closing++] = c;
  if (nc_connection_busy(c)) nc_connection_cancel(c); /* the disconnect queues behind a running statement */
  nc_connection_close(c);
}

void app_conn_event(NcConnection *c, NcConnEvent ev, const char *msg, void *data) {
  NcApp *app = data;
  if (ev == NC_CONN_EV_TABLES) {
    size_t n = 0;
    nc_connection_tables(c, &n);
    app_log(app, false, "list tables  \xe2\x86\x92 %zu", n);
    ConnTab *t = app_current_tab(app);
    if (t && t->conn == c) app_auto_open(app, t);
  } else if (ev == NC_CONN_EV_NOTICE && msg) {
    app_set_status(app, false, "%s", msg);
  } else if (ev == NC_CONN_EV_CLOSED) {
    for (size_t i = 0; i < app->num_closing; i++) {
      if (app->closing[i] != c) continue;
      memmove(app->closing + i, app->closing + i + 1, (app->num_closing - i - 1) * sizeof *app->closing);
      app->num_closing--;
      nc_connection_free(c);
      return;
    }
  }
}
