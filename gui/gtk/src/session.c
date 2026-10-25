/*
 * Lace GTK frontend
 * Session persistence (gtk-session.json)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "session.h"
#include "platform/platform.h"
#include <lace.h>
#include "util.h"
#include "views/query_view.h"
#include "views/table_view.h"
#include "window.h"
#include <config/saved_connection.h>
#include <util/json_helpers.h>
#include <util/str.h>
#include <cjson/cJSON.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define SESSION_FILE "gtk-session.json"

char *lace_session_path(void) {
  const char *dir = platform_get_config_dir();
  return dir ? str_printf("%s%s%s", dir, LACE_PATH_SEP_STR, SESSION_FILE) : NULL;
}

/* ==========================================================================
 * Save
 * ========================================================================== */

static cJSON *tab_json(GtkWidget *view) {
  cJSON *t = cJSON_CreateObject();
  if (LACE_IS_TABLE_VIEW(view)) {
    LaceTableView *tv = LACE_TABLE_VIEW(view);
    cJSON_AddStringToObject(t, "type", "table");
    cJSON_AddStringToObject(t, "table", lace_table_view_get_table(tv));
    /* what differs from a table on Data, so other frontends read the file as before */
    if (lace_table_view_get_object_kind(tv) != LACE_OBJ_TABLE) {
      cJSON_AddStringToObject(t, "object", lace_object_kind_key(lace_table_view_get_object_kind(tv)));
      JSON_ADD_STR(t, "object_table", lace_table_view_get_object_table(tv));
      JSON_ADD_STR(t, "object_detail", lace_table_view_get_object_detail(tv));
    }
    if (lace_table_view_get_subview(tv) != LACE_SUBVIEW_DATA)
      cJSON_AddStringToObject(t, "subview", lace_subview_key(lace_table_view_get_subview(tv), lace_table_view_get_object_kind(tv)));
    gsize n;
    const LaceFilter *f = lace_table_view_get_filters(tv, &n);
    cJSON *filters = cJSON_AddArrayToObject(t, "filters");
    for (gsize i = 0; i < n; i++) {
      cJSON *fj = cJSON_CreateObject();
      if (f[i].column)
        cJSON_AddStringToObject(fj, "column", f[i].column);
      cJSON_AddStringToObject(fj, "op", lace_filter_op_key(f[i].op));
      JSON_ADD_STR(fj, "value", f[i].value);
      if (f[i].value2)
        cJSON_AddStringToObject(fj, "value2", f[i].value2);
      cJSON_AddItemToArray(filters, fj);
    }
    JSON_ADD_STR(t, "search", lace_table_view_get_search(tv));
    gsize ns;
    LaceSortSpec *sorts = lace_table_view_get_sorts(tv, &ns);
    cJSON *sj = cJSON_AddArrayToObject(t, "sorts");
    for (gsize i = 0; i < ns; i++) {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "column", sorts[i].column);
      cJSON_AddBoolToObject(s, "desc", sorts[i].desc);
      cJSON_AddItemToArray(sj, s);
      g_free(sorts[i].column);
    }
    g_free(sorts);
    cJSON_AddBoolToObject(t, "filters_visible", lace_table_view_filters_visible(tv));
  } else if (LACE_IS_QUERY_VIEW(view)) {
    cJSON_AddStringToObject(t, "type", "query");
    char *text = lace_query_view_get_text(LACE_QUERY_VIEW(view));
    cJSON_AddStringToObject(t, "text", text);
    g_free(text);
  }
  return t;
}

static cJSON *connection_json(LaceConnectionPage *cp) {
  LaceConnection *conn = lace_connection_page_get_connection(cp);
  cJSON *c = cJSON_CreateObject();
  if (lace_connection_get_saved_id(conn))
    cJSON_AddStringToObject(c, "saved_id", lace_connection_get_saved_id(conn));
  char *masked = lace_connection_get_masked_connstr(conn);
  JSON_ADD_STR(c, "connstr", masked);
  free(masked);
  cJSON_AddStringToObject(c, "name", lace_connection_get_name(conn));
  cJSON_AddBoolToObject(c, "sidebar", lace_connection_page_sidebar_visible(cp));
  AdwTabView *tv = lace_connection_page_get_tab_view(cp);
  cJSON *tabs = cJSON_AddArrayToObject(c, "tabs");
  int n = adw_tab_view_get_n_pages(tv);
  AdwTabPage *sel = adw_tab_view_get_selected_page(tv);
  for (int i = 0; i < n; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(tv, i);
    cJSON_AddItemToArray(tabs, tab_json(lace_connection_page_view_of(page)));
    if (page == sel)
      cJSON_AddNumberToObject(c, "current", i);
  }
  return c;
}

static cJSON *window_json(LaceWindow *w) {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "name", lace_window_get_name(w));
  int width, height;
  gtk_window_get_default_size(GTK_WINDOW(w), &width, &height);
  cJSON_AddNumberToObject(j, "width", width);
  cJSON_AddNumberToObject(j, "height", height);
  cJSON_AddBoolToObject(j, "maximized", gtk_window_is_maximized(GTK_WINDOW(w)));
  AdwTabView *tv = lace_window_get_tab_view(w);
  cJSON *conns = cJSON_AddArrayToObject(j, "connections");
  int n = adw_tab_view_get_n_pages(tv);
  AdwTabPage *sel = adw_tab_view_get_selected_page(tv);
  for (int i = 0; i < n; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(tv, i);
    cJSON_AddItemToArray(conns, connection_json(LACE_CONNECTION_PAGE(adw_tab_page_get_child(page))));
    if (page == sel)
      cJSON_AddNumberToObject(j, "current", i);
  }
  return j;
}

void lace_session_save(LaceApp *app, LaceWindow *except) {
  char *path = lace_session_path();
  if (!path)
    return;
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "version", 1);
  cJSON *windows = cJSON_AddArrayToObject(root, "windows");
  GList *wins = lace_app_get_lace_windows(app);
  for (GList *l = wins; l; l = l->next) {
    if (l->data == except)
      continue;
    cJSON_AddItemToArray(windows, window_json(LACE_WINDOW(l->data)));
  }
  g_list_free(wins);
  char *text = cJSON_Print(root);
  cJSON_Delete(root);
  if (!text || !platform_write_file_private(path, text, strlen(text)))
    g_warning("session: could not write %s: %s", path, strerror(errno));
  str_secure_free(text);
  free(path);
}

/* ==========================================================================
 * Restore
 * ========================================================================== */

typedef struct {
  LaceApp *app;
  GWeakRef window;
  cJSON *json;     /* The window's object (owned copy) */
  cJSON *conns;    /* Array */
  int index;
  int current;
} Restore;

static void restore_next(Restore *r);

static void restore_free(Restore *r) {
  g_weak_ref_clear(&r->window);
  cJSON_Delete(r->json);
  g_free(r);
}

static void restore_tabs(LaceConnectionPage *cp, cJSON *conn_json) {
  cJSON *tabs = json_get_array(conn_json, "tabs");
  int current = json_get_int(conn_json, "current", 0);
  AdwTabPage *select = NULL;
  int i = 0;
  cJSON *t;
  cJSON_ArrayForEach(t, tabs) {
    const char *type = json_get_string(t, "type", "");
    GtkWidget *view = NULL;
    if (g_str_equal(type, "table")) {
      const char *table = json_get_string(t, "table", NULL);
      if (!table)
        continue;
      LaceObjectKind kind = LACE_OBJ_TABLE;
      if (!lace_object_kind_from_key(json_get_string(t, "object", "table"), &kind))
        kind = LACE_OBJ_TABLE;
      LaceTableView *tv = lace_connection_page_open_object(cp, kind, table, json_get_string(t, "object_table", NULL),
                                                           json_get_string(t, "object_detail", NULL), FALSE);
      view = GTK_WIDGET(tv);
      LaceSubView sv;
      if (lace_subview_from_key(json_get_string(t, "subview", NULL), &sv) && lace_object_has_subview(kind, sv))
        lace_table_view_set_subview(tv, sv);
      /* Filters, search, sorts */
      cJSON *filters = json_get_array(t, "filters");
      int nf = json_array_size(filters);
      LaceFilter *f = g_new0(LaceFilter, nf + 1);
      int k = 0;
      cJSON *fj;
      cJSON_ArrayForEach(fj, filters) {
        f[k].column = g_strdup(json_get_string(fj, "column", NULL));
        f[k].op = lace_filter_op_from_key(json_get_string(fj, "op", "eq"));
        f[k].value = g_strdup(json_get_string(fj, "value", ""));
        f[k].value2 = g_strdup(json_get_string(fj, "value2", NULL));
        k++;
      }
      cJSON *sorts = json_get_array(t, "sorts");
      int ns = json_array_size(sorts);
      LaceSortSpec *s = g_new0(LaceSortSpec, ns + 1);
      int m = 0;
      cJSON *sj;
      cJSON_ArrayForEach(sj, sorts) {
        const char *col = json_get_string(sj, "column", NULL);
        if (!col)
          continue;
        s[m].column = g_strdup(col);
        s[m].desc = json_get_bool(sj, "desc", false);
        m++;
      }
      lace_table_view_restore(tv, f, (gsize)k, json_get_string(t, "search", ""), s, (gsize)m,
                              json_get_bool(t, "filters_visible", false));
      for (int x = 0; x < k; x++) {
        g_free(f[x].column);
        g_free(f[x].value);
        g_free(f[x].value2);
      }
      g_free(f);
      for (int x = 0; x < m; x++)
        g_free(s[x].column);
      g_free(s);
    } else if (g_str_equal(type, "query")) {
      view = GTK_WIDGET(lace_connection_page_new_query(cp, json_get_string(t, "text", "")));
    }
    if (view && i == current)
      select = adw_tab_view_get_page(lace_connection_page_get_tab_view(cp), view);
    i++;
  }
  if (select)
    adw_tab_view_set_selected_page(lace_connection_page_get_tab_view(cp), select);
  lace_connection_page_set_sidebar_visible(cp, json_get_bool(conn_json, "sidebar", true));
}

static void restore_connected(LaceConnection *conn, const char *error, gpointer data) {
  Restore *r = data;
  LaceWindow *w = g_weak_ref_get(&r->window);
  if (!w) {
    restore_free(r);
    return;
  }
  cJSON *cj = cJSON_GetArrayItem(r->conns, r->index - 1);
  if (conn) {
    LaceConnectionPage *cp = lace_window_add_connection(w, conn);
    restore_tabs(cp, cj);
    if (r->index - 1 == r->current)
      adw_tab_view_set_selected_page(lace_window_get_tab_view(w),
                                     adw_tab_view_get_page(lace_window_get_tab_view(w), GTK_WIDGET(cp)));
  } else if (error) {
    lace_toast(GTK_WIDGET(w), "Could not reopen %s: %s", json_get_string(cj, "name", "a connection"), error);
  }
  g_object_unref(w);
  restore_next(r);
}

static void restore_next(Restore *r) {
  LaceWindow *w = g_weak_ref_get(&r->window);
  if (!w) {
    restore_free(r);
    return;
  }
  if (r->index >= cJSON_GetArraySize(r->conns)) {
    g_object_unref(w);
    restore_free(r);
    return;
  }
  cJSON *cj = cJSON_GetArrayItem(r->conns, r->index++);
  const char *saved_id = json_get_string(cj, "saved_id", NULL);
  const char *name = json_get_string(cj, "name", NULL);
  char *connstr = NULL;
  if (saved_id) {
    ConnectionItem *item = connmgr_find_by_id(lace_app_get_saved(r->app), saved_id);
    if (item && item->type == CONN_ITEM_CONNECTION) {
      connstr = lace_saved_conn_to_connstr(&item->connection);
      if (!name || !*name)
        name = item->connection.name;
    } else {
      saved_id = NULL; /* Deleted since: fall back to the string */
    }
  }
  if (!connstr) {
    const char *c = json_get_string(cj, "connstr", NULL);
    if (!c || !*c) {
      g_object_unref(w);
      restore_next(r);
      return;
    }
    connstr = str_dup(c);
  }
  lace_app_connect(r->app, GTK_WIDGET(w), connstr, saved_id, name, restore_connected, r);
  str_secure_free(connstr);
  g_object_unref(w);
}

gboolean lace_session_restore(LaceApp *app) {
  char *path = lace_session_path();
  if (!path || !platform_file_exists(path)) {
    free(path);
    return FALSE;
  }
  char *content = platform_read_file(path, 16 * 1024 * 1024);
  free(path);
  if (!content)
    return FALSE;
  cJSON *root = cJSON_Parse(content);
  str_secure_free(content);
  if (!root)
    return FALSE;
  cJSON *windows = json_get_array(root, "windows");
  gboolean any = FALSE;
  cJSON *wj;
  cJSON_ArrayForEach(wj, windows) {
    LaceWindow *w = lace_app_new_window(app, json_get_string(wj, "name", NULL));
    int width = json_get_int(wj, "width", 1200), height = json_get_int(wj, "height", 760);
    if (width > 200 && height > 200)
      gtk_window_set_default_size(GTK_WINDOW(w), width, height);
    if (json_get_bool(wj, "maximized", false))
      gtk_window_maximize(GTK_WINDOW(w));
    any = TRUE;
    cJSON *conns = json_get_array(wj, "connections");
    if (!conns || cJSON_GetArraySize(conns) == 0)
      continue;
    Restore *r = g_new0(Restore, 1);
    r->app = app;
    g_weak_ref_init(&r->window, w);
    r->json = cJSON_Duplicate(wj, 1);
    r->conns = json_get_array(r->json, "connections");
    r->current = json_get_int(wj, "current", 0);
    restore_next(r);
  }
  cJSON_Delete(root);
  return any;
}
