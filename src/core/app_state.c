/*
 * lace - Database Viewer and Manager
 * Core application state implementation
 */

#include "app_state.h"
#include <stdlib.h>
#include <string.h>

/* Default page size for data loading */
#define DEFAULT_PAGE_SIZE 500

/* Initialize application state */
void app_state_init(AppState *app) {
  if (!app)
    return;

  memset(app, 0, sizeof(AppState));
  app->page_size = DEFAULT_PAGE_SIZE;
}

/* Cleanup application state */
void app_state_cleanup(AppState *app) {
  if (!app)
    return;

  /* Close all workspaces */
  for (size_t i = 0; i < app->num_workspaces; i++) {
    Workspace *ws = &app->workspaces[i];
    if (!ws->active)
      continue;

    /* Free table data */
    free(ws->table_name);
    db_result_free(ws->data);
    db_schema_free(ws->schema);
    free(ws->col_widths);
    filters_free(&ws->filters);

    /* Free query data */
    free(ws->query_text);
    db_result_free(ws->query_results);
    free(ws->query_error);
    free(ws->query_result_col_widths);
    free(ws->query_result_edit_buf);
    free(ws->query_source_table);
    db_schema_free(ws->query_source_schema);
    free(ws->query_base_sql);
  }

  /* Free table list */
  if (app->tables) {
    for (size_t i = 0; i < app->num_tables; i++) {
      free(app->tables[i]);
    }
    free(app->tables);
  }

  /* Disconnect database */
  if (app->conn) {
    db_disconnect(app->conn);
  }

  memset(app, 0, sizeof(AppState));
}

/* Get current workspace (convenience) */
Workspace *app_current_workspace(AppState *app) {
  if (!app || app->num_workspaces == 0)
    return NULL;
  if (app->current_workspace >= app->num_workspaces)
    return NULL;
  return &app->workspaces[app->current_workspace];
}
