#include "model/workspace.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

NcWorkspace *workspace_new(const char *name) {
  NcWorkspace *w = safe_calloc(1, sizeof *w);
  w->name = str_dup(name ? name : "Workspace");
  return w;
}

static void conntab_free(ConnTab *t) {
  if (!t) return;
  for (size_t i = 0; i < t->num_views; i++) view_free(t->views[i]);
  free(t->views);
  free(t->sidebar_search);
  if (t->owner) nc_owner_release(t->owner);
  free(t);
}

void workspace_free(NcWorkspace *w) {
  if (!w) return;
  for (size_t i = 0; i < w->num_tabs; i++) conntab_free(w->tabs[i]);
  free(w->tabs);
  free(w->name);
  free(w);
}

ConnTab *workspace_add_tab(NcWorkspace *w, NcConnection *c) {
  ConnTab *t = safe_calloc(1, sizeof *t);
  t->conn = c;
  t->owner = nc_owner_new();
  t->sidebar = true;
  w->tabs = safe_realloc(w->tabs, (w->num_tabs + 1) * sizeof *w->tabs);
  w->tabs[w->num_tabs] = t;
  w->current = w->num_tabs++;
  return t;
}

void workspace_remove_tab(NcWorkspace *w, size_t i) {
  if (i >= w->num_tabs) return;
  conntab_free(w->tabs[i]);
  memmove(w->tabs + i, w->tabs + i + 1, (w->num_tabs - i - 1) * sizeof *w->tabs);
  w->num_tabs--;
  if (w->current > i || (w->current == i && w->current >= w->num_tabs))
    w->current = w->current ? w->current - 1 : 0;
}

ConnTab *workspace_current_tab(const NcWorkspace *w) {
  return w && w->current < w->num_tabs ? w->tabs[w->current] : NULL;
}

View *conntab_add_view(ConnTab *t, View *v) {
  t->views = safe_realloc(t->views, (t->num_views + 1) * sizeof *t->views);
  t->views[t->num_views] = v;
  t->current = t->num_views++;
  return v;
}

void conntab_remove_view(ConnTab *t, size_t i) {
  if (i >= t->num_views) return;
  view_free(t->views[i]);
  memmove(t->views + i, t->views + i + 1, (t->num_views - i - 1) * sizeof *t->views);
  t->num_views--;
  if (t->current > i || (t->current == i && t->current >= t->num_views))
    t->current = t->current ? t->current - 1 : 0;
}

View *conntab_current_view(const ConnTab *t) {
  return t && t->current < t->num_views ? t->views[t->current] : NULL;
}

size_t conntab_find_table_view(const ConnTab *t, const char *table) {
  for (size_t i = 0; t && table && i < t->num_views; i++)
    if (t->views[i]->kind == VIEW_TABLE && t->views[i]->table && !strcmp(t->views[i]->table, table))
      return i;
  return SIZE_MAX;
}

size_t conntab_find_preview(const ConnTab *t) {
  for (size_t i = 0; t && i < t->num_views; i++)
    if (t->views[i]->preview) return i;
  return SIZE_MAX;
}

View *conntab_replace_view(ConnTab *t, size_t i, View *v) {
  if (i >= t->num_views) return conntab_add_view(t, v);
  view_free(t->views[i]);
  t->views[i] = v;
  t->current = i;
  return v;
}

static cJSON *pair(size_t a, size_t b) {
  cJSON *arr = cJSON_CreateArray();
  cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)a));
  cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)b));
  return arr;
}

cJSON *view_to_json(const View *v, bool with_cursor) {
  cJSON *o = cJSON_CreateObject();
  if (v->kind == VIEW_QUERY) {
    cJSON_AddStringToObject(o, "type", "query");
    cJSON_AddStringToObject(o, "text", v->text ? v->text : "");
    if (with_cursor) {
      cJSON_AddNumberToObject(o, "cursor", (double)v->text_cursor);
      cJSON_AddItemToObject(o, "scroll", pair(v->text_scroll_line, v->text_scroll_col));
    }
    return o;
  }
  cJSON_AddStringToObject(o, "type", "table");
  cJSON_AddStringToObject(o, "table", v->table ? v->table : "");
  cJSON *fs = cJSON_AddArrayToObject(o, "filters");
  for (size_t i = 0; i < v->num_filters; i++) {
    const LaceFilter *f = &v->filters[i];
    if (!view_filter_active(f)) continue; /* the panel's blank rows */
    cJSON *fo = cJSON_CreateObject();
    if (f->column && f->op != LACE_FILTER_RAW) {
      cJSON_AddStringToObject(fo, "column", f->column);
      cJSON_AddStringToObject(fo, "op", lace_filter_op_key(f->op));
    } else {
      cJSON_AddStringToObject(fo, "op", "raw");
    }
    cJSON_AddStringToObject(fo, "value", f->value ? f->value : "");
    if (f->value2) cJSON_AddStringToObject(fo, "value2", f->value2);
    cJSON_AddItemToArray(fs, fo);
  }
  cJSON *ss = cJSON_AddArrayToObject(o, "sorts");
  for (size_t i = 0; i < v->num_sorts; i++) {
    cJSON *so = cJSON_CreateObject();
    cJSON_AddStringToObject(so, "column", v->sorts[i].column);
    cJSON_AddBoolToObject(so, "desc", v->sorts[i].desc);
    cJSON_AddItemToArray(ss, so);
  }
  cJSON_AddStringToObject(o, "search", v->search ? v->search : "");
  cJSON_AddBoolToObject(o, "filters_visible", v->filters_visible);
  if (with_cursor) {
    cJSON_AddItemToObject(o, "cursor", pair(v->cursor_row, v->cursor_col));
    cJSON_AddItemToObject(o, "scroll", pair(v->scroll_row, v->scroll_col));
  }
  return o;
}

cJSON *workspace_to_json(const NcWorkspace *w, bool with_cursor) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "name", w->name ? w->name : "");
  cJSON *conns = cJSON_AddArrayToObject(o, "connections");
  for (size_t i = 0; i < w->num_tabs; i++) {
    const ConnTab *t = w->tabs[i];
    cJSON *c = cJSON_CreateObject();
    if (t->conn) {
      const char *sid = nc_connection_saved_id(t->conn);
      if (sid && *sid) cJSON_AddStringToObject(c, "saved_id", sid);
      char *cs = nc_connection_masked_connstr(t->conn);
      if (cs) cJSON_AddStringToObject(c, "connstr", cs);
      str_secure_free(cs);
      const char *nm = nc_connection_name(t->conn);
      if (nm) cJSON_AddStringToObject(c, "name", nm);
    }
    cJSON_AddBoolToObject(c, "sidebar", t->sidebar);
    cJSON *tabs = cJSON_AddArrayToObject(c, "tabs");
    for (size_t k = 0; k < t->num_views; k++)
      cJSON_AddItemToArray(tabs, view_to_json(t->views[k], with_cursor));
    cJSON_AddNumberToObject(c, "current", (double)t->current);
    cJSON_AddItemToArray(conns, c);
  }
  cJSON_AddNumberToObject(o, "current", (double)w->current);
  return o;
}
