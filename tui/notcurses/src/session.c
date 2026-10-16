#include "session.h"
#include "config/workspace_json.h"
#include "model/workspace.h"
#include "ui/dialogs/confirm.h"
#include "platform/platform.h"
#include "util/json_helpers.h"
#include "util/mem.h"
#include "util/str.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define SESSION_MAX_BYTES (10 * 1024 * 1024)

char *nc_session_path(void) {
  const char *dir = platform_get_config_dir();
  return dir ? str_printf("%s/session.json", dir) : NULL;
}

/* Read and parse the file. Returns 0 absent, 1 parsed (*out), 2 empty,
 * -1 broken (error set). The text may hold connection strings: wiped when freed. */
static int read_session(const char *path, cJSON **out, char **error) {
  *out = NULL;
  if (!platform_file_exists(path)) return 0;
  char *content = platform_read_file(path, SESSION_MAX_BYTES);
  if (!content || !content[0]) {
    if (content) {
      str_secure_free(content);
      return 2;
    }
    err_setf(error, "Failed to read %s: %s", path, strerror(errno));
    return -1;
  }
  cJSON *json = cJSON_Parse(content);
  if (!json) {
    const char *p = cJSON_GetErrorPtr();
    err_setf(error, "session.json: JSON parse error near byte %ld", p ? (long)(p - content) : -1L);
    str_secure_free(content);
    return -1;
  }
  str_secure_free(content);
  *out = json;
  return 1;
}

cJSON *nc_session_load(char **error) {
  char *path = nc_session_path();
  if (!path) return NULL;
  cJSON *json = NULL;
  int r = read_session(path, &json, error);
  free(path);
  if (r == 2) err_setf(error, "session.json: empty file");
  if (r <= 0 || r == 2) return NULL;
  if (!cJSON_IsObject(json)) {
    err_setf(error, "session.json: not an object");
    cJSON_Delete(json);
    return NULL;
  }
  if (json_get_int(json, "version", 1) >= 2) return json;
  char *cerr = NULL;
  cJSON *v2 = session_json_to_v2(json, &cerr);
  cJSON_Delete(json);
  if (!v2) {
    err_setf(error, "session.json: %s", cerr ? cerr : "cannot convert");
    free(cerr);
    return NULL;
  }
  free(cerr); /* a warning about skipped workspaces */
  return v2;
}

static bool ours(const char *key) {
  static const char *const k[] = {"version", "workspaces", "current", "notcurses"};
  for (size_t i = 0; i < sizeof k / sizeof *k; i++)
    if (!strcmp(key, k[i])) return true;
  return false;
}

bool nc_session_save(NcApp *app, char **error) {
  char *path = nc_session_path();
  if (!path) {
    err_setf(error, "session.json: no config directory");
    return false;
  }
  /* Rule 1: read what is there first. A non-empty file that does not parse
   * is never overwritten. */
  cJSON *old = NULL;
  int r = read_session(path, &old, error);
  if (r < 0) {
    free(path);
    return false;
  }
  if (r == 1) {
    if (!cJSON_IsObject(old)) {
      err_setf(error, "session.json: not a JSON object");
      cJSON_Delete(old);
      free(path);
      return false;
    }
    if (json_get_int(old, "version", 1) < 2) { /* keep the ncurses settings */
      char *cerr = NULL;
      cJSON *v2 = session_json_to_v2(old, &cerr);
      cJSON_Delete(old);
      old = v2;
      if (!v2) {
        err_setf(error, "session.json: %s", cerr ? cerr : "cannot convert");
        free(cerr);
        free(path);
        return false;
      }
      free(cerr);
    }
  }
  bool with_cursor = app->config ? app->config->general.restore_cursor_position : true;
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "version", 2);
  cJSON *wss = cJSON_AddArrayToObject(root, "workspaces");
  for (size_t i = 0; i < app->num_workspaces; i++)
    cJSON_AddItemToArray(wss, workspace_to_json(app->workspaces[i], with_cursor));
  cJSON_AddNumberToObject(root, "current", (double)app->current_ws);
  cJSON_AddItemToObject(root, "notcurses", cJSON_CreateObject());
  if (cJSON_IsObject(old)) {
    for (cJSON *m = old->child; m; m = m->next) {
      if (!m->string || ours(m->string)) continue;
      cJSON_AddItemToObject(root, m->string, cJSON_Duplicate(m, true));
    }
  }
  cJSON_Delete(old);
  char *text = cJSON_Print(root);
  cJSON_Delete(root);
  bool ok = text && platform_write_file_private(path, text, strlen(text));
  if (!ok) err_setf(error, "session.json: cannot write %s", path);
  str_secure_free(text);
  free(path);
  return ok;
}

/* ==========================================================================
 * Restore
 * ========================================================================== */

/* One restore in flight: the workspaces array is a private copy, so the
 * caller's root may go; the queue is (wi, ci), the next connection to open */
struct NcRestore {
  struct NcRestore *next;
  NcApp *app;
  cJSON *workspaces; /* owned copy */
  size_t current;    /* the root's current workspace */
  size_t wi, ci;
  bool started;      /* workspace wi was created */
  NcWorkspace **wss; /* per workspace of the root: created, NULL until then */
  size_t num_wss;
  NcConnection **landed; /* of workspace wi, per connection (NULL: failed) */
  size_t num_landed;
  NcWorkspace *expected;   /* what the restore left current: another one means the user switched */
  NcWorkspace *return_to;  /* what was current when the step began: the user's choice */
  bool user_switched;      /* the user changed workspace meanwhile: leave their choice */
  void (*done)(NcApp *app);
};
typedef struct NcRestore NcRestore;

static void restore_step(NcRestore *s);


static const cJSON *nth(const cJSON *arr, size_t i) { return cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, (int)i) : NULL; }

static bool ws_live(NcApp *app, const NcWorkspace *w, size_t *index) {
  for (size_t i = 0; w && i < app->num_workspaces; i++)
    if (app->workspaces[i] == w) {
      if (index) *index = i;
      return true;
    }
  return false;
}

/* The connect needed the restored workspace current: give the user's back */
static void restore_return(NcRestore *s) {
  size_t at;
  if (s->return_to && ws_live(s->app, s->return_to, &at)) s->app->current_ws = at;
  s->expected = app_current_workspace(s->app);
}

static void restore_unlink_free(NcRestore *s) {
  NcApp *app = s->app;
  for (NcRestore **pp = &app->restore; *pp; pp = &(*pp)->next)
    if (*pp == s) {
      *pp = s->next;
      break;
    }
  cJSON_Delete(s->workspaces);
  free(s->wss);
  free(s->landed);
  free(s);
}

void nc_session_restore_free(NcApp *app) {
  while (app->restore) restore_unlink_free(app->restore);
}

static void restore_finish(NcRestore *s) {
  NcApp *app = s->app;
  size_t at;
  if (!s->user_switched && s->current < s->num_wss && ws_live(app, s->wss[s->current], &at)) app_switch_workspace(app, at);
  void (*done)(NcApp *) = s->done;
  restore_unlink_free(s);
  if (done) done(app);
}

static size_t clamp_index(const cJSON *item, size_t n) {
  double d = cJSON_IsNumber(item) ? item->valuedouble : 0;
  if (d < 0 || n == 0) return 0;
  return (size_t)d >= n ? n - 1 : (size_t)d;
}

static bool json_pair(const cJSON *arr, size_t *a, size_t *b) {
  const cJSON *x = nth(arr, 0), *y = nth(arr, 1);
  if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || x->valuedouble < 0 || y->valuedouble < 0) return false;
  *a = (size_t)x->valuedouble;
  *b = (size_t)y->valuedouble;
  return true;
}

static void restore_table(NcApp *app, const cJSON *tj) {
  const cJSON *tn = cJSON_GetObjectItem(tj, "table");
  if (!cJSON_IsString(tn) || !*tn->valuestring) return;
  View *v = app_open_table(app, tn->valuestring);
  if (!v) return;
  /* Set before the schema lands: the first page is read with them */
  const cJSON *f;
  cJSON_ArrayForEach(f, cJSON_GetObjectItem(tj, "filters")) {
    const cJSON *col = cJSON_GetObjectItem(f, "column"), *op = cJSON_GetObjectItem(f, "op");
    const cJSON *val = cJSON_GetObjectItem(f, "value"), *val2 = cJSON_GetObjectItem(f, "value2");
    const char *value = cJSON_IsString(val) ? val->valuestring : "";
    if (!cJSON_IsString(op)) continue;
    if (!strcmp(op->valuestring, "raw") || !cJSON_IsString(col)) {
      if (*value) view_add_raw_filter(v, value);
      continue;
    }
    view_set_filter(v, col->valuestring, lace_filter_op_from_key(op->valuestring), value,
                    cJSON_IsString(val2) ? val2->valuestring : NULL);
  }
  /* view_cycle_sort puts the newest first: replay oldest first */
  const cJSON *sorts = cJSON_GetObjectItem(tj, "sorts");
  for (int i = cJSON_IsArray(sorts) ? cJSON_GetArraySize(sorts) - 1 : -1; i >= 0; i--) {
    const cJSON *sj = cJSON_GetArrayItem(sorts, i);
    const cJSON *col = cJSON_GetObjectItem(sj, "column");
    if (!cJSON_IsString(col)) continue;
    view_cycle_sort(v, col->valuestring);
    if (cJSON_IsTrue(cJSON_GetObjectItem(sj, "desc"))) view_cycle_sort(v, col->valuestring);
  }
  const cJSON *search = cJSON_GetObjectItem(tj, "search");
  if (cJSON_IsString(search) && *search->valuestring) {
    free(v->search);
    v->search = str_dup(search->valuestring);
  }
  v->filters_visible = cJSON_IsTrue(cJSON_GetObjectItem(tj, "filters_visible"));
  size_t a, b;
  if (app->config->general.restore_cursor_position && json_pair(cJSON_GetObjectItem(tj, "cursor"), &a, &b)) {
    v->cursor_col = b; /* clamped to the columns when they arrive */
    v->jump_pending = true; /* the row, once the total is known */
    v->jump_row = a;
    if (json_pair(cJSON_GetObjectItem(tj, "scroll"), &a, &b)) v->scroll_col = b;
  }
}

static void restore_query(NcApp *app, const cJSON *tj) {
  const cJSON *text = cJSON_GetObjectItem(tj, "text");
  View *v = app_open_query(app, cJSON_IsString(text) ? text->valuestring : "");
  if (!v) return;
  v->focus_results = true; /* keys reach the global hotkeys until the user asks for the editor */
  const cJSON *cur = cJSON_GetObjectItem(tj, "cursor");
  if (app->config->general.restore_cursor_position && cJSON_IsNumber(cur) && cur->valuedouble >= 0 && v->text) {
    size_t n = strlen(v->text), c = (size_t)cur->valuedouble;
    if (c > n) c = n;
    while (c > 0 && ((unsigned char)v->text[c] & 0xc0) == 0x80) c--;
    v->text_cursor = c;
  }
}

static void restore_failed(NcRestore *s, const cJSON *cj, const char *error) {
  NcApp *app = s->app;
  const cJSON *nm = cJSON_GetObjectItem(cj, "name");
  const char *name = cJSON_IsString(nm) && *nm->valuestring ? nm->valuestring : "a connection";
  app_log(app, true, "Could not reopen %s: %s", name,
          error && *error ? error : "connection failed");
}

static void restore_conn_done(NcApp *app, NcConnection *conn, const char *error, void *data) {
  NcRestore *s = data;
  const cJSON *wsj = nth(s->workspaces, s->wi);
  const cJSON *cj = nth(cJSON_GetObjectItem(wsj, "connections"), s->ci - 1);
  size_t wsi;
  if (!conn || !cj || !ws_live(app, s->wss[s->wi], &wsi)) {
    if (cj) restore_failed(s, cj, conn ? "the workspace was closed" : error);
    restore_return(s);
    restore_step(s);
    return;
  }
  s->landed[s->ci - 1] = conn;
  NcWorkspace *ws = app->workspaces[wsi];
  size_t ti = SIZE_MAX;
  for (size_t i = 0; i < ws->num_tabs; i++)
    if (ws->tabs[i]->conn == conn) ti = i;
  if (ti != SIZE_MAX) {
    app->current_ws = wsi;
    ws->current = ti;
    ConnTab *t = ws->tabs[ti];
    const cJSON *sb = cJSON_GetObjectItem(cj, "sidebar");
    if (cJSON_IsBool(sb)) t->sidebar = cJSON_IsTrue(sb);
    const cJSON *tj;
    cJSON_ArrayForEach(tj, cJSON_GetObjectItem(cj, "tabs")) {
      const cJSON *type = cJSON_GetObjectItem(tj, "type");
      if (!cJSON_IsString(type)) continue;
      if (!strcmp(type->valuestring, "table")) restore_table(app, tj);
      else if (!strcmp(type->valuestring, "query")) restore_query(app, tj);
    }
    if (t->num_views) t->current = clamp_index(cJSON_GetObjectItem(cj, "current"), t->num_views);
    t->sidebar_focused = false;
  }
  restore_return(s);
  restore_step(s);
}

static void restore_start_workspace(NcRestore *s, const cJSON *wsj) {
  NcApp *app = s->app;
  const cJSON *nm = cJSON_GetObjectItem(wsj, "name");
  const char *name = cJSON_IsString(nm) && *nm->valuestring ? nm->valuestring : NULL;
  NcWorkspace *cur = app_current_workspace(app);
  if (s->wi == 0 && cur && cur->num_tabs == 0) {
    if (name) app_rename_workspace(app, app->current_ws, name);
  } else {
    app_new_workspace(app, name);
  }
  s->wss[s->wi] = app_current_workspace(app);
  size_t n = cJSON_IsArray(cJSON_GetObjectItem(wsj, "connections"))
                 ? (size_t)cJSON_GetArraySize(cJSON_GetObjectItem(wsj, "connections")) : 0;
  free(s->landed);
  s->landed = safe_calloc(n ? n : 1, sizeof *s->landed);
  s->num_landed = n;
  s->expected = app_current_workspace(app);
  s->started = true;
}

static void restore_finish_workspace(NcRestore *s, const cJSON *wsj) {
  NcApp *app = s->app;
  size_t wsi;
  if (!ws_live(app, s->wss[s->wi], &wsi)) return;
  NcWorkspace *ws = app->workspaces[wsi];
  const cJSON *cur = cJSON_GetObjectItem(wsj, "current");
  if (!cJSON_IsNumber(cur) || cur->valuedouble < 0 || (size_t)cur->valuedouble >= s->num_landed) return;
  NcConnection *c = s->landed[(size_t)cur->valuedouble];
  for (size_t i = 0; c && i < ws->num_tabs; i++)
    if (ws->tabs[i]->conn == c) ws->current = i;
}

static void restore_step(NcRestore *s) {
  NcApp *app = s->app;
  s->return_to = app_current_workspace(app);
  if (s->expected && s->return_to != s->expected) s->user_switched = true;
  for (;;) {
    const cJSON *wsj = nth(s->workspaces, s->wi);
    if (!wsj) {
      restore_finish(s);
      return;
    }
    if (!s->started) restore_start_workspace(s, wsj);
    const cJSON *cj = nth(cJSON_GetObjectItem(wsj, "connections"), s->ci);
    if (!cj) {
      restore_finish_workspace(s, wsj);
      s->wi++;
      s->ci = 0;
      s->started = false;
      continue;
    }
    size_t wsi;
    if (!ws_live(app, s->wss[s->wi], &wsi)) { /* closed meanwhile: skip the rest of it */
      s->ci = (size_t)cJSON_GetArraySize(cJSON_GetObjectItem(wsj, "connections"));
      continue;
    }
    app->current_ws = wsi; /* the connection joins the current workspace */
    s->expected = s->wss[s->wi];
    const cJSON *sid = cJSON_GetObjectItem(cj, "saved_id"), *cs = cJSON_GetObjectItem(cj, "connstr");
    const cJSON *nm = cJSON_GetObjectItem(cj, "name");
    const char *saved = cJSON_IsString(sid) && *sid->valuestring ? sid->valuestring : NULL;
    s->ci++;
    if (saved && app->connmgr && connmgr_find_by_id(app->connmgr, saved)) {
      app_connect_saved(app, saved, true, restore_conn_done, s);
      return;
    }
    if (cJSON_IsString(cs) && *cs->valuestring) {
      app_connect_url(app, cs->valuestring, saved, cJSON_IsString(nm) ? nm->valuestring : NULL, true,
                      restore_conn_done, s);
      return;
    }
    restore_failed(s, cj, "saved connection not found");
  }
}

void nc_session_restore(NcApp *app, const cJSON *root, void (*done)(NcApp *app)) {
  const cJSON *wss = cJSON_GetObjectItem(root, "workspaces");
  int n = cJSON_IsArray(wss) ? cJSON_GetArraySize(wss) : 0;
  if (n == 0) {
    if (done) done(app);
    return;
  }
  NcRestore *s = safe_calloc(1, sizeof *s);
  s->app = app;
  s->workspaces = cJSON_Duplicate(wss, true);
  s->num_wss = (size_t)n;
  s->wss = safe_calloc(s->num_wss, sizeof *s->wss);
  s->current = clamp_index(cJSON_GetObjectItem(root, "current"), s->num_wss);
  s->done = done;
  s->next = app->restore;
  app->restore = s;
  restore_step(s);
}

/* ==========================================================================
 * Saved workspaces
 * ========================================================================== */

void app_save_workspace_as(NcApp *app, size_t ws, const char *name) {
  if (ws >= app->num_workspaces || !name || !*name) return;
  cJSON *j = workspace_to_json(app->workspaces[ws], false);
  /* A connection without a saved entry could not be opened again */
  cJSON *conns = cJSON_GetObjectItem(j, "connections");
  int left_out = 0;
  for (int i = cJSON_GetArraySize(conns) - 1; i >= 0; i--) {
    cJSON *c = cJSON_GetArrayItem(conns, i);
    cJSON *sid = cJSON_GetObjectItem(c, "saved_id");
    if (cJSON_IsString(sid) && *sid->valuestring) continue;
    cJSON_DeleteItemFromArray(conns, i);
    left_out++;
  }
  if (cJSON_GetArraySize(conns) == 0) {
    cJSON_Delete(j);
    app_set_status(app, true, "No saved connections in this workspace to save");
    return;
  }
  if (left_out) cJSON_ReplaceItemInObject(j, "current", cJSON_CreateNumber(0));
  saved_workspaces_put(&app->saved_ws, name, j);
  char *err = NULL;
  if (!saved_workspaces_save(&app->saved_ws, &err)) {
    app_set_status(app, true, "Cannot save workspaces: %s", err ? err : "write failed");
    free(err);
  } else if (left_out) {
    app_set_status(app, false, "Workspace saved as %s (%d connection%s without a saved entry left out)", name,
                   left_out, left_out == 1 ? "" : "s");
  } else {
    app_set_status(app, false, "Workspace saved as %s", name);
  }
}

void app_open_saved_workspace(NcApp *app, size_t index) {
  if (index >= app->saved_ws.count) return;
  const cJSON *w = app->saved_ws.items[index].workspace;
  cJSON *nested = NULL;
  if (workspace_json_shape(w) == WORKSPACE_JSON_FLAT) {
    char *err = NULL;
    nested = workspace_json_to_nested(w, &err);
    if (!nested) {
      app_set_status(app, true, "%s", err ? err : "Cannot read the saved workspace");
      free(err);
      return;
    }
    w = nested;
  } else if (workspace_json_shape(w) == WORKSPACE_JSON_INVALID) {
    app_set_status(app, true, "The saved workspace is not readable");
    return;
  }
  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(root, "workspaces");
  cJSON *copy = cJSON_Duplicate(w, true);
  if (cJSON_GetObjectItem(copy, "name")) cJSON_ReplaceItemInObject(copy, "name", cJSON_CreateString(app->saved_ws.items[index].name));
  else cJSON_AddStringToObject(copy, "name", app->saved_ws.items[index].name);
  cJSON_AddItemToArray(arr, copy); /* it opens under the name it was saved as */
  cJSON_AddNumberToObject(root, "current", 0);
  cJSON_Delete(nested);
  nc_session_restore(app, root, NULL);
  cJSON_Delete(root);
}

static void delete_confirmed(NcApp *app, bool yes, void *data) {
  char *name = data;
  if (yes) {
    size_t i = saved_workspaces_find(&app->saved_ws, name);
    if (i != SIZE_MAX) {
      saved_workspaces_remove(&app->saved_ws, i);
      char *err = NULL;
      if (!saved_workspaces_save(&app->saved_ws, &err))
        app_set_status(app, true, "Cannot save workspaces: %s", err ? err : "write failed");
      else app_set_status(app, false, "Deleted saved workspace %s", name);
      free(err);
    }
  }
  free(name);
}

void app_delete_saved_workspace(NcApp *app, size_t index) {
  if (index >= app->saved_ws.count) return;
  char *msg = str_printf("Delete the saved workspace %s?", app->saved_ws.items[index].name);
  confirm_open(app, "Delete saved workspace", msg, "Delete", true, delete_confirmed,
               str_dup(app->saved_ws.items[index].name));
  free(msg);
}
