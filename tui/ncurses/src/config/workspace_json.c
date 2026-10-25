#include "workspace_json.h"
#include "../core/app_state.h"
#include "../../../liblace/include/sql_builder.h"
#include "../../../liblace/include/util/json_helpers.h"
#include "../../../liblace/include/util/str.h"
#include "../../../liblace/include/util/mem.h"
#include <stdlib.h>
#include <string.h>

/* FilterOperator (this TUI) <-> LaceFilterOp (the key strings' owner) */
static const LaceFilterOp OP_TO_LACE[FILTER_OP_COUNT] = {
    [FILTER_OP_EQ] = LACE_FILTER_EQ,           [FILTER_OP_NE] = LACE_FILTER_NE,
    [FILTER_OP_GT] = LACE_FILTER_GT,           [FILTER_OP_GE] = LACE_FILTER_GE,
    [FILTER_OP_LT] = LACE_FILTER_LT,           [FILTER_OP_LE] = LACE_FILTER_LE,
    [FILTER_OP_IN] = LACE_FILTER_IN,           [FILTER_OP_CONTAINS] = LACE_FILTER_CONTAINS,
    [FILTER_OP_REGEX] = LACE_FILTER_REGEX,     [FILTER_OP_BETWEEN] = LACE_FILTER_BETWEEN,
    [FILTER_OP_IS_EMPTY] = LACE_FILTER_EMPTY,  [FILTER_OP_IS_NOT_EMPTY] = LACE_FILTER_NOT_EMPTY,
    [FILTER_OP_IS_NULL] = LACE_FILTER_NULL,    [FILTER_OP_IS_NOT_NULL] = LACE_FILTER_NOT_NULL,
    [FILTER_OP_RAW] = LACE_FILTER_RAW,         [FILTER_OP_STARTS] = LACE_FILTER_STARTS,
    [FILTER_OP_ENDS] = LACE_FILTER_ENDS,
};

const char *workspace_json_op_key(int op) {
  if (op < 0 || op >= FILTER_OP_COUNT)
    return NULL;
  return lace_filter_op_key(OP_TO_LACE[op]);
}

int workspace_json_op_from_key(const char *key) {
  if (!key)
    return -1;
  for (int op = 0; op < FILTER_OP_COUNT; op++)
    if (str_eq(lace_filter_op_key(OP_TO_LACE[op]), key))
      return op;
  return -1;
}

WorkspaceJsonShape workspace_json_shape(const cJSON *ws_c) {
  cJSON *ws = (cJSON *)ws_c;
  if (!cJSON_IsObject(ws))
    return WORKSPACE_JSON_INVALID;
  if (cJSON_IsArray(cJSON_GetObjectItem(ws, "connections")))
    return WORKSPACE_JSON_NESTED;
  if (cJSON_IsArray(cJSON_GetObjectItem(ws, "tabs")))
    return WORKSPACE_JSON_FLAT;
  return WORKSPACE_JSON_INVALID;
}

static void copy_if(cJSON *dst, const char *dkey, cJSON *src, const char *skey) {
  cJSON *v = cJSON_GetObjectItem(src, skey);
  if (v)
    cJSON_AddItemToObject(dst, dkey, cJSON_Duplicate(v, true));
}

/* ---- flat -> nested ---------------------------------------------------- */

static cJSON *nested_filters(cJSON *flat_filters, char **error) {
  cJSON *out = cJSON_CreateArray();
  cJSON *f;
  cJSON_ArrayForEach(f, flat_filters) {
    int op = json_get_int(f, "op", -1);
    bool raw = json_get_bool(f, "raw", false) || op == FILTER_OP_RAW;
    const char *key = raw ? "raw" : workspace_json_op_key(op);
    if (!key) {
      err_setf(error, "Filter operator %d is unknown", op);
      cJSON_Delete(out);
      return NULL;
    }
    cJSON *n = cJSON_CreateObject();
    if (!raw)
      cJSON_AddStringToObject(n, "column", json_get_string(f, "column", ""));
    cJSON_AddStringToObject(n, "op", key);
    cJSON_AddStringToObject(n, "value", json_get_string(f, "value", ""));
    const char *v2 = json_get_string(f, "value2", NULL);
    if (v2 && v2[0])
      cJSON_AddStringToObject(n, "value2", v2);
    cJSON_AddItemToArray(out, n);
  }
  return out;
}

static cJSON *nested_tab(cJSON *t, int order, char **error, bool *failed) {
  const char *type = json_get_string(t, "type", "");
  cJSON *n = cJSON_CreateObject();
  if (str_eq(type, "TABLE")) {
    cJSON_AddStringToObject(n, "type", "table");
    cJSON_AddStringToObject(n, "table", json_get_string(t, "table_name", ""));
    cJSON *ff = json_get_array(t, "filters");
    cJSON *filters = ff ? nested_filters(ff, error) : cJSON_CreateArray();
    if (!filters) { cJSON_Delete(n); *failed = true; return NULL; }
    cJSON_AddItemToObject(n, "filters", filters);
    cJSON *sorts = cJSON_CreateArray();
    cJSON *s;
    cJSON_ArrayForEach(s, json_get_array(t, "sort")) {
      int dir = json_get_int(s, "direction", 0);
      if (dir != SORT_ASC && dir != SORT_DESC)
        continue;
      cJSON *ns = cJSON_CreateObject();
      cJSON_AddStringToObject(ns, "column", json_get_string(s, "column", ""));
      cJSON_AddBoolToObject(ns, "desc", dir == SORT_DESC);
      cJSON_AddItemToArray(sorts, ns);
    }
    cJSON_AddItemToObject(n, "sorts", sorts);
    cJSON *ui = json_get_object(t, "ui");
    cJSON_AddBoolToObject(n, "filters_visible", ui && json_get_bool(ui, "filters_visible", false));
    copy_if(n, "cursor", t, "cursor");
    copy_if(n, "scroll", t, "scroll");
    copy_if(n, "object", t, "object"); /* a database view, routine or trigger, and its sub-view */
    copy_if(n, "object_table", t, "object_table");
    copy_if(n, "object_detail", t, "object_detail");
    copy_if(n, "subview", t, "subview");
  } else if (str_eq(type, "QUERY")) {
    cJSON_AddStringToObject(n, "type", "query");
    cJSON_AddStringToObject(n, "text", json_get_string(t, "query_text", ""));
    cJSON *qc = cJSON_GetObjectItem(t, "query_cursor");
    if (cJSON_IsNumber(qc))
      cJSON_AddNumberToObject(n, "cursor", qc->valuedouble);
    cJSON *ql = cJSON_GetObjectItem(t, "query_scroll_line");
    if (cJSON_IsNumber(ql)) {
      cJSON *sc = cJSON_AddArrayToObject(n, "scroll");
      cJSON_AddItemToArray(sc, cJSON_CreateNumber(ql->valuedouble));
      cJSON_AddItemToArray(sc, cJSON_CreateNumber(json_get_int(t, "query_scroll_col", 0)));
    }
  } else {
    cJSON_Delete(n);
    return NULL; /* CONNECTION: no view */
  }
  cJSON_AddNumberToObject(n, "order", order);
  /* This TUI's focus state; other frontends ignore it */
  cJSON *fui = json_get_object(t, "ui");
  cJSON *nc = cJSON_AddObjectToObject(n, "ncurses");
  cJSON_AddBoolToObject(nc, "filters_focused", fui && json_get_bool(fui, "filters_focused", false));
  cJSON_AddBoolToObject(nc, "query_focus_results", fui && json_get_bool(fui, "query_focus_results", false));
  return n;
}

cJSON *workspace_json_to_nested(const cJSON *flat_c, char **error) {
  cJSON *flat = (cJSON *)flat_c;
  if (workspace_json_shape(flat) != WORKSPACE_JSON_FLAT) {
    err_setf(error, "Not a flat workspace");
    return NULL;
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddStringToObject(out, "name", json_get_string(flat, "name", ""));
  cJSON *conns = cJSON_AddArrayToObject(out, "connections");
  int current_tab = json_get_int(flat, "current_tab", 0);
  int cur_conn = 0;
  int order = 0;
  cJSON *t;
  cJSON_ArrayForEach(t, json_get_array(flat, "tabs")) {
    /* A saved connection by id; else the password-less string of one
     * opened by URL; a tab with neither cannot be reopened */
    const char *id = json_get_string(t, "connection_id", "");
    const char *cs = json_get_string(t, "connstr", "");
    int idx = order++;
    if (str_eq(json_get_string(t, "type", ""), "FORM")) {
      /* A connection form: a connection object of its own, never merged
       * with another (no id to match), holding only its position */
      cJSON *f = cJSON_CreateObject();
      cJSON_AddBoolToObject(f, "form", true);
      cJSON_AddNumberToObject(f, "order", idx);
      if (idx == current_tab)
        cur_conn = json_array_size(conns);
      cJSON_AddItemToArray(conns, f);
      continue;
    }
    if (!id[0] && !cs[0])
      continue;
    /* Find or add the connection */
    cJSON *c = NULL;
    int ci = 0;
    cJSON *k;
    cJSON_ArrayForEach(k, conns) {
      const char *kid = json_get_string(k, "saved_id", "");
      if (id[0] ? str_eq(kid, id)
                : !kid[0] && str_eq(json_get_string(k, "connstr", ""), cs)) {
        c = k;
        break;
      }
      ci++;
    }
    if (!c) {
      c = cJSON_CreateObject();
      if (id[0])
        cJSON_AddStringToObject(c, "saved_id", id);
      else
        cJSON_AddStringToObject(c, "connstr", cs);
      cJSON_AddBoolToObject(c, "sidebar", true);
      cJSON_AddArrayToObject(c, "tabs");
      cJSON_AddNumberToObject(c, "current", 0);
      cJSON_AddItemToArray(conns, c);
    }
    bool failed = false;
    cJSON *n = nested_tab(t, idx, error, &failed);
    if (failed) { cJSON_Delete(out); return NULL; }
    if (idx == current_tab)
      cur_conn = ci;
    if (!n) {
      /* A connection tab has no view: its position goes on the connection
       * (other frontends ignore it), so it comes back where it was */
      if (!cJSON_GetObjectItem(c, "order"))
        cJSON_AddNumberToObject(c, "order", idx);
      continue;
    }
    cJSON *ctabs = json_get_array(c, "tabs");
    cJSON_AddItemToArray(ctabs, n);
    if (idx == current_tab) {
      cJSON_DeleteItemFromObject(c, "current");
      cJSON_AddNumberToObject(c, "current", json_array_size(ctabs) - 1);
    }
  }
  cJSON_AddNumberToObject(out, "current", cur_conn);
  return out;
}

/* ---- nested -> flat ---------------------------------------------------- */

/* A flat tab of type for conn: its saved id, else (an empty id is none) its
 * password-less connstr */
static cJSON *flat_tab_for(cJSON *conn, const char *type) {
  cJSON *t = cJSON_CreateObject();
  cJSON_AddStringToObject(t, "type", type);
  const char *sid = json_get_string(conn, "saved_id", "");
  cJSON_AddStringToObject(t, "connection_id", sid);
  const char *cs = json_get_string(conn, "connstr", "");
  if (!sid[0] && cs[0])
    cJSON_AddStringToObject(t, "connstr", cs);
  return t;
}

static void flat_ui_extras(cJSON *ui, cJSON *n) {
  cJSON *nc = json_get_object(n, "ncurses");
  cJSON_AddBoolToObject(ui, "filters_focused", nc && json_get_bool(nc, "filters_focused", false));
  cJSON_AddBoolToObject(ui, "query_focus_results", nc && json_get_bool(nc, "query_focus_results", false));
}

static cJSON *flat_tab(cJSON *n, cJSON *conn, char **error) {
  const char *type = json_get_string(n, "type", "");
  cJSON *t = flat_tab_for(conn, str_eq(type, "query") ? "QUERY" : "TABLE");
  if (str_eq(type, "query")) {
    cJSON_AddStringToObject(t, "query_text", json_get_string(n, "text", ""));
    cJSON *qc = cJSON_GetObjectItem(n, "cursor");
    if (cJSON_IsNumber(qc))
      cJSON_AddNumberToObject(t, "query_cursor", qc->valuedouble);
    cJSON *sc = json_get_array(n, "scroll");
    if (sc && json_array_size(sc) == 2) {
      cJSON_AddNumberToObject(t, "query_scroll_line", json_get_array_item(sc, 0)->valuedouble);
      cJSON_AddNumberToObject(t, "query_scroll_col", json_get_array_item(sc, 1)->valuedouble);
    }
    flat_ui_extras(cJSON_AddObjectToObject(t, "ui"), n);
    return t;
  }
  if (!str_eq(type, "table")) {
    cJSON_Delete(t);
    err_setf(error, "Unknown tab type '%s'", type);
    return NULL;
  }
  cJSON_AddStringToObject(t, "table_name", json_get_string(n, "table", ""));
  copy_if(t, "cursor", n, "cursor");
  copy_if(t, "scroll", n, "scroll");
  copy_if(t, "object", n, "object");
  copy_if(t, "object_table", n, "object_table");
  copy_if(t, "object_detail", n, "object_detail");
  copy_if(t, "subview", n, "subview");
  cJSON *sorts = cJSON_AddArrayToObject(t, "sort");
  cJSON *s;
  cJSON_ArrayForEach(s, json_get_array(n, "sorts")) {
    cJSON *fs = cJSON_CreateObject();
    cJSON_AddStringToObject(fs, "column", json_get_string(s, "column", ""));
    cJSON_AddNumberToObject(fs, "direction", json_get_bool(s, "desc", false) ? SORT_DESC : SORT_ASC);
    cJSON_AddItemToArray(sorts, fs);
  }
  cJSON *filters = cJSON_AddArrayToObject(t, "filters");
  cJSON *f;
  cJSON_ArrayForEach(f, json_get_array(n, "filters")) {
    const char *key = json_get_string(f, "op", "eq");
    const char *col = json_get_string(f, "column", NULL);
    bool raw = str_eq(key, "raw") || !col;
    int op = raw ? FILTER_OP_RAW : workspace_json_op_from_key(key);
    if (op < 0) {
      err_setf(error, "Filter operator '%s' is not supported by this version", key);
      cJSON_Delete(t);
      return NULL;
    }
    cJSON *ff = cJSON_CreateObject();
    cJSON_AddStringToObject(ff, "column", raw ? "" : col);
    if (raw)
      cJSON_AddBoolToObject(ff, "raw", true);
    cJSON_AddNumberToObject(ff, "op", op);
    cJSON_AddStringToObject(ff, "value", json_get_string(f, "value", ""));
    const char *v2 = json_get_string(f, "value2", NULL);
    if (v2 && v2[0])
      cJSON_AddStringToObject(ff, "value2", v2);
    cJSON_AddItemToArray(filters, ff);
  }
  cJSON *ui = cJSON_AddObjectToObject(t, "ui");
  cJSON_AddBoolToObject(ui, "filters_visible", json_get_bool(n, "filters_visible", false));
  flat_ui_extras(ui, n);
  return t;
}

typedef struct { cJSON *tab; int order; int conn; int in_conn; } FlatEntry;

cJSON *workspace_json_to_flat(const cJSON *nested_c, char **error) {
  cJSON *nested = (cJSON *)nested_c;
  if (workspace_json_shape(nested) != WORKSPACE_JSON_NESTED) {
    err_setf(error, "Not a nested workspace");
    return NULL;
  }
  /* Collect every tab, then order by "order" when all have one */
  size_t cap = 16, n = 0;
  FlatEntry *entries = safe_calloc(cap, sizeof(FlatEntry));
  bool all_ordered = true;
  int ci = 0;
  cJSON *c;
  cJSON_ArrayForEach(c, json_get_array(nested, "connections")) {
    if (json_get_bool(c, "form", false)) { /* wins over any saved_id/connstr */
      int forder = json_get_int(c, "order", -1);
      if (forder < 0) all_ordered = false;
      cJSON *ft = cJSON_CreateObject();
      cJSON_AddStringToObject(ft, "type", "FORM");
      cJSON_AddStringToObject(ft, "connection_id", "");
      if (n == cap) entries = safe_reallocarray(entries, cap *= 2, sizeof(FlatEntry));
      entries[n++] = (FlatEntry){ft, forder, ci++, 0};
      continue;
    }
    int ti = 0;
    cJSON *t;
    cJSON_ArrayForEach(t, json_get_array(c, "tabs")) {
      cJSON *ft = flat_tab(t, c, error);
      if (!ft) {
        for (size_t i = 0; i < n; i++) cJSON_Delete(entries[i].tab);
        free(entries);
        return NULL;
      }
      if (n == cap) entries = safe_reallocarray(entries, cap *= 2, sizeof(FlatEntry));
      int order = json_get_int(t, "order", -1);
      if (order < 0) all_ordered = false;
      entries[n++] = (FlatEntry){ft, order, ci, ti++};
    }
    /* The connection tab this TUI had (its position on the connection), or
     * one for a connection with no tabs, so the connection is reopened. It
     * is the connection's current tab only when there is no other. */
    int corder = json_get_int(c, "order", -1);
    if (corder >= 0 || ti == 0) {
      if (n == cap) entries = safe_reallocarray(entries, cap *= 2, sizeof(FlatEntry));
      if (corder < 0) all_ordered = false;
      entries[n++] = (FlatEntry){flat_tab_for(c, "CONNECTION"), corder, ci, ti == 0 ? 0 : -1};
    }
    ci++;
  }
  if (all_ordered) { /* insertion sort: a workspace has a handful of tabs */
    for (size_t i = 1; i < n; i++)
      for (size_t j = i; j > 0 && entries[j - 1].order > entries[j].order; j--) {
        FlatEntry tmp = entries[j]; entries[j] = entries[j - 1]; entries[j - 1] = tmp;
      }
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddStringToObject(out, "name", json_get_string(nested, "name", ""));
  int cur_conn = json_get_int(nested, "current", 0);
  cJSON *cc = json_get_array_item(json_get_array(nested, "connections"), cur_conn);
  int cur_in = cc ? json_get_int(cc, "current", 0) : 0;
  int current_tab = 0;
  cJSON *tabs = cJSON_AddArrayToObject(out, "tabs");
  for (size_t i = 0; i < n; i++) {
    if (entries[i].conn == cur_conn && entries[i].in_conn == cur_in)
      current_tab = (int)i;
    cJSON_AddItemToArray(tabs, entries[i].tab);
  }
  free(entries);
  cJSON_AddNumberToObject(out, "current_tab", current_tab);
  return out;
}

/* ---- session roots ----------------------------------------------------- */

static const char *const OWNED_V1[] = {"settings", "workspaces", "current_workspace", "version", NULL};
static const char *const OWNED_V2[] = {"ncurses", "workspaces", "current", "version", NULL};

static bool in_list(const char *s, const char *const *list) {
  for (; *list; list++)
    if (str_eq(s, *list)) return true;
  return false;
}

/* Adds one skipped workspace (index i of root's) to the warning in *warn */
static void note_skipped(char **warn, cJSON *w, int i, const char *why) {
  const char *name = json_get_string(w, "name", "");
  char *one = name[0] ? str_printf("Workspace '%s' skipped: %s", name, why ? why : "cannot convert")
                      : str_printf("Workspace %d skipped: %s", i + 1, why ? why : "cannot convert");
  if (!one)
    return;
  if (!*warn) {
    *warn = one;
    return;
  }
  char *both = str_printf("%s; %s", *warn, one);
  free(one);
  if (both) {
    free(*warn);
    *warn = both;
  }
}

typedef cJSON *(*WorkspaceConvert)(const cJSON *, char **);

/* Converts every workspace of src into dst; one that fails is skipped and
 * named in *warn. *current (an index into src's) becomes an index into
 * dst's: moved down past skipped ones, 0 if it was skipped, clamped. False
 * when there were workspaces and none converted. */
static bool convert_workspaces(cJSON *dst, cJSON *src, WorkspaceJsonShape keep,
                               WorkspaceConvert convert, int *current, char **warn) {
  int i = 0, kept = 0, cur = *current, new_cur = 0;
  cJSON *w;
  cJSON_ArrayForEach(w, src) {
    char *why = NULL;
    cJSON *x = workspace_json_shape(w) == keep ? cJSON_Duplicate(w, true) : convert(w, &why);
    if (x) {
      if (i == cur)
        new_cur = kept;
      cJSON_AddItemToArray(dst, x);
      kept++;
    } else {
      note_skipped(warn, w, i, why);
    }
    free(why);
    i++;
  }
  *current = new_cur;
  return i == 0 || kept > 0;
}

/* Reports the warning through error (or frees it) */
static void hand_warning(char *warn, char **error) {
  if (!warn)
    return;
  if (error) {
    free(*error);
    *error = warn;
  } else {
    free(warn);
  }
}

static void copy_others(cJSON *dst, cJSON *src, const char *const *skip) {
  cJSON *m;
  cJSON_ArrayForEach(m, src)
    if (m->string && !in_list(m->string, skip))
      cJSON_AddItemToObject(dst, m->string, cJSON_Duplicate(m, true));
}

cJSON *session_json_to_v2(const cJSON *root_c, char **error) {
  cJSON *root = (cJSON *)root_c;
  if (!cJSON_IsObject(root)) { err_setf(error, "Not a session"); return NULL; }
  if (json_get_int(root, "version", 1) >= 2)
    return cJSON_Duplicate(root, true);
  cJSON *out = cJSON_CreateObject();
  cJSON_AddNumberToObject(out, "version", 2);
  cJSON *wss = cJSON_AddArrayToObject(out, "workspaces");
  int current = json_get_int(root, "current_workspace", 0);
  char *warn = NULL;
  if (!convert_workspaces(wss, json_get_array(root, "workspaces"), WORKSPACE_JSON_NESTED,
                          workspace_json_to_nested, &current, &warn)) {
    err_setf(error, "No workspace could be converted: %s", warn ? warn : "?");
    free(warn);
    cJSON_Delete(out);
    return NULL;
  }
  hand_warning(warn, error);
  cJSON_AddNumberToObject(out, "current", current);
  cJSON *settings = json_get_object(root, "settings");
  cJSON_AddItemToObject(out, "ncurses", settings ? cJSON_Duplicate(settings, true) : cJSON_CreateObject());
  copy_others(out, root, OWNED_V1);
  return out;
}

cJSON *session_json_to_v1(const cJSON *root_c, char **error) {
  cJSON *root = (cJSON *)root_c;
  if (!cJSON_IsObject(root)) { err_setf(error, "Not a session"); return NULL; }
  cJSON *out = cJSON_CreateObject();
  cJSON *settings = json_get_object(root, "ncurses");
  if (!settings) settings = json_get_object(root, "settings");
  cJSON_AddItemToObject(out, "settings", settings ? cJSON_Duplicate(settings, true) : cJSON_CreateObject());
  cJSON *wss = cJSON_AddArrayToObject(out, "workspaces");
  cJSON *cur = cJSON_GetObjectItem(root, "current");
  int current = cJSON_IsNumber(cur) ? (int)cur->valuedouble
                                    : json_get_int(root, "current_workspace", 0);
  char *warn = NULL;
  if (!convert_workspaces(wss, json_get_array(root, "workspaces"), WORKSPACE_JSON_FLAT,
                          workspace_json_to_flat, &current, &warn)) {
    err_setf(error, "No workspace could be converted: %s", warn ? warn : "?");
    free(warn);
    cJSON_Delete(out);
    return NULL;
  }
  hand_warning(warn, error);
  cJSON_AddNumberToObject(out, "current_workspace", current);
  copy_others(out, root, OWNED_V2);
  return out;
}
