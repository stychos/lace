/*
 * Lace ncurses frontend
 * Session save/restore
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "session.h"
#include "app.h"
#include "config.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ==========================================================================
 * Helper Functions
 * ========================================================================== */

char *session_get_path(void) {
  char *dir = config_get_dir();
  if (!dir) return NULL;

  char *path = malloc(strlen(dir) + 64);
  if (!path) {
    free(dir);
    return NULL;
  }

  snprintf(path, strlen(dir) + 64, "%s/ncurses/%s", dir, SESSION_FILE);
  free(dir);
  return path;
}

/* ==========================================================================
 * Session Save
 * ========================================================================== */

bool session_save(TuiState *tui) {
  if (!tui || !tui->app) return false;

  char *path = session_get_path();
  if (!path) return false;

  /* Create directories */
  char *dir = config_get_dir();
  if (dir) {
    mkdir(dir, 0755);
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/ncurses", dir);
    mkdir(subdir, 0755);
    free(dir);
  }

  /* Build JSON */
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    free(path);
    return false;
  }

  /* Save connections */
  cJSON *connections = cJSON_CreateArray();
  for (size_t i = 0; i < tui->app->num_connections; i++) {
    Connection *conn = &tui->app->connections[i];
    if (conn->connstr) {
      cJSON *conn_obj = cJSON_CreateObject();
      cJSON_AddStringToObject(conn_obj, "connstr", conn->connstr);
      cJSON_AddItemToArray(connections, conn_obj);
    }
  }
  cJSON_AddItemToObject(json, "connections", connections);

  /* Save tabs */
  cJSON *tabs = cJSON_CreateArray();
  for (size_t i = 0; i < tui->app->num_tabs; i++) {
    Tab *tab = &tui->app->tabs[i];
    cJSON *tab_obj = cJSON_CreateObject();

    cJSON_AddStringToObject(tab_obj, "type",
                            tab->type == TAB_TYPE_QUERY ? "query" : "table");
    cJSON_AddNumberToObject(tab_obj, "conn_id", tab->conn_id);

    if (tab->table_name) {
      cJSON_AddStringToObject(tab_obj, "table", tab->table_name);
    }

    /* Save cursor position */
    cJSON_AddNumberToObject(tab_obj, "cursor_row", (double)tab->cursor_row);
    cJSON_AddNumberToObject(tab_obj, "cursor_col", (double)tab->cursor_col);
    cJSON_AddNumberToObject(tab_obj, "scroll_row", (double)tab->scroll_row);
    cJSON_AddNumberToObject(tab_obj, "scroll_col", (double)tab->scroll_col);

    /* Save filters */
    if (tab->num_filters > 0) {
      cJSON *filters = cJSON_CreateArray();
      for (size_t f = 0; f < tab->num_filters; f++) {
        LaceFilter *filter = &tab->filters[f];
        cJSON *filter_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(filter_obj, "column", (double)filter->column);
        cJSON_AddNumberToObject(filter_obj, "op", filter->op);
        if (filter->value) {
          cJSON_AddStringToObject(filter_obj, "value", filter->value);
        }
        cJSON_AddItemToArray(filters, filter_obj);
      }
      cJSON_AddItemToObject(tab_obj, "filters", filters);
    }

    cJSON_AddItemToArray(tabs, tab_obj);
  }
  cJSON_AddItemToObject(json, "tabs", tabs);

  /* Save current tab index */
  cJSON_AddNumberToObject(json, "active_tab", (double)tui->app->active_tab);

  /* Save sidebar state */
  cJSON_AddBoolToObject(json, "sidebar_visible", tui->in_sidebar);

  /* Write file */
  char *str = cJSON_Print(json);
  cJSON_Delete(json);
  if (!str) {
    free(path);
    return false;
  }

  FILE *f = fopen(path, "w");
  if (!f) {
    free(str);
    free(path);
    return false;
  }

  fputs(str, f);
  fclose(f);
  free(str);
  free(path);
  return true;
}

/* ==========================================================================
 * Session Restore
 * ========================================================================== */

bool session_restore(TuiState *tui) {
  if (!tui || !tui->app) return false;

  char *path = session_get_path();
  if (!path) return false;

  /* Read file */
  FILE *f = fopen(path, "r");
  if (!f) {
    free(path);
    return false;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0 || size > 1024 * 1024) {
    fclose(f);
    free(path);
    return false;
  }

  char *data = malloc((size_t)size + 1);
  if (!data) {
    fclose(f);
    free(path);
    return false;
  }

  size_t read = fread(data, 1, (size_t)size, f);
  fclose(f);
  free(path);
  data[read] = '\0';

  /* Parse JSON */
  cJSON *json = cJSON_Parse(data);
  free(data);
  if (!json) return false;

  /* Restore connections */
  cJSON *connections = cJSON_GetObjectItem(json, "connections");
  if (cJSON_IsArray(connections)) {
    cJSON *conn_obj;
    cJSON_ArrayForEach(conn_obj, connections) {
      cJSON *connstr = cJSON_GetObjectItem(conn_obj, "connstr");
      if (cJSON_IsString(connstr)) {
        /* Try to connect */
        app_connect(tui->app, connstr->valuestring, NULL);
      }
    }
  }

  /* Restore tabs */
  cJSON *tabs = cJSON_GetObjectItem(json, "tabs");
  if (cJSON_IsArray(tabs)) {
    cJSON *tab_obj;
    cJSON_ArrayForEach(tab_obj, tabs) {
      cJSON *type = cJSON_GetObjectItem(tab_obj, "type");
      cJSON *conn_id_obj = cJSON_GetObjectItem(tab_obj, "conn_id");
      cJSON *table = cJSON_GetObjectItem(tab_obj, "table");

      const char *type_str = cJSON_IsString(type) ? type->valuestring : "table";
      int conn_idx = cJSON_IsNumber(conn_id_obj) ? conn_id_obj->valueint : 0;
      const char *table_name = cJSON_IsString(table) ? table->valuestring : NULL;

      /* Create tab */
      int tab_idx = -1;
      if (strcmp(type_str, "query") == 0) {
        tab_idx = app_open_query_tab(tui->app, conn_idx);
      } else if (table_name) {
        tab_idx = app_open_table(tui->app, conn_idx, table_name);
      }

      if (tab_idx >= 0 && (size_t)tab_idx < tui->app->num_tabs) {
        Tab *tab = &tui->app->tabs[tab_idx];

        /* Restore positions */
        cJSON *item;
        if ((item = cJSON_GetObjectItem(tab_obj, "cursor_row"))) {
          tab->cursor_row = (size_t)item->valuedouble;
        }
        if ((item = cJSON_GetObjectItem(tab_obj, "cursor_col"))) {
          tab->cursor_col = (size_t)item->valuedouble;
        }
        if ((item = cJSON_GetObjectItem(tab_obj, "scroll_row"))) {
          tab->scroll_row = (size_t)item->valuedouble;
        }
        if ((item = cJSON_GetObjectItem(tab_obj, "scroll_col"))) {
          tab->scroll_col = (size_t)item->valuedouble;
        }

        /* Restore filters */
        cJSON *filters = cJSON_GetObjectItem(tab_obj, "filters");
        if (cJSON_IsArray(filters)) {
          cJSON *filter_obj;
          cJSON_ArrayForEach(filter_obj, filters) {
            cJSON *col = cJSON_GetObjectItem(filter_obj, "column");
            cJSON *op = cJSON_GetObjectItem(filter_obj, "op");
            cJSON *val = cJSON_GetObjectItem(filter_obj, "value");

            if (cJSON_IsNumber(col) && cJSON_IsNumber(op)) {
              /* Add filter to tab */
              tab->num_filters++;
              tab->filters = realloc(tab->filters,
                                     tab->num_filters * sizeof(LaceFilter));
              LaceFilter *filter = &tab->filters[tab->num_filters - 1];
              memset(filter, 0, sizeof(LaceFilter));
              filter->column = (size_t)col->valuedouble;
              filter->op = (LaceFilterOp)op->valueint;
              if (cJSON_IsString(val)) {
                filter->value = strdup(val->valuestring);
              }
            }
          }
        }
      }
    }
  }

  /* Restore active tab */
  cJSON *active_tab = cJSON_GetObjectItem(json, "active_tab");
  if (cJSON_IsNumber(active_tab)) {
    size_t idx = (size_t)active_tab->valuedouble;
    if (idx < tui->app->num_tabs) {
      tui->app->active_tab = idx;
    }
  }

  /* Restore sidebar state */
  cJSON *sidebar = cJSON_GetObjectItem(json, "sidebar_visible");
  if (cJSON_IsBool(sidebar)) {
    tui->in_sidebar = cJSON_IsTrue(sidebar);
  }

  cJSON_Delete(json);
  return true;
}

void session_clear(void) {
  char *path = session_get_path();
  if (!path) return;
  unlink(path);
  free(path);
}
