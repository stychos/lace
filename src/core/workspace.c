/*
 * lace - Database Viewer and Manager
 * Core Workspace Management Implementation
 */

#include "workspace.h"
#include "../db/db.h"
#include "../util/str.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Workspace Lifecycle
 * ============================================================================
 */

void workspace_init(Workspace *ws) {
  if (!ws)
    return;
  memset(ws, 0, sizeof(Workspace));
  filters_init(&ws->filters);

  /* Default visibility */
  ws->header_visible = true;
  ws->status_visible = true;
}

void workspace_free_data(Workspace *ws) {
  if (!ws)
    return;

  /* Free table data */
  free(ws->table_name);
  ws->table_name = NULL;

  db_result_free(ws->data);
  ws->data = NULL;

  db_schema_free(ws->schema);
  ws->schema = NULL;

  free(ws->col_widths);
  ws->col_widths = NULL;
  ws->num_col_widths = 0;

  filters_free(&ws->filters);

  /* Free query-specific data */
  free(ws->query_text);
  ws->query_text = NULL;

  db_result_free(ws->query_results);
  ws->query_results = NULL;

  free(ws->query_error);
  ws->query_error = NULL;

  free(ws->query_result_col_widths);
  ws->query_result_col_widths = NULL;

  free(ws->query_result_edit_buf);
  ws->query_result_edit_buf = NULL;

  free(ws->query_source_table);
  ws->query_source_table = NULL;

  db_schema_free(ws->query_source_schema);
  ws->query_source_schema = NULL;

  free(ws->query_base_sql);
  ws->query_base_sql = NULL;
}

/* ============================================================================
 * Workspace Operations
 * ============================================================================
 */

Workspace *app_workspace_switch(AppState *app, size_t index) {
  if (!app || index >= app->num_workspaces)
    return NULL;
  if (index == app->current_workspace)
    return &app->workspaces[index];

  app->current_workspace = index;
  return &app->workspaces[index];
}

Workspace *app_workspace_create(AppState *app, size_t table_index,
                                const char *table_name) {
  if (!app || app->num_workspaces >= MAX_WORKSPACES)
    return NULL;

  size_t new_idx = app->num_workspaces;
  Workspace *ws = &app->workspaces[new_idx];
  workspace_init(ws);

  ws->active = true;
  ws->type = WORKSPACE_TYPE_TABLE;
  ws->table_index = table_index;
  ws->table_name = str_dup(table_name);

  app->num_workspaces++;
  app->current_workspace = new_idx;

  return ws;
}

Workspace *app_workspace_create_query(AppState *app) {
  if (!app || app->num_workspaces >= MAX_WORKSPACES)
    return NULL;

  size_t new_idx = app->num_workspaces;
  Workspace *ws = &app->workspaces[new_idx];
  workspace_init(ws);

  ws->active = true;
  ws->type = WORKSPACE_TYPE_QUERY;
  ws->table_name = str_dup("Query");

  /* Initialize query buffer */
  ws->query_capacity = 1024;
  ws->query_text = malloc(ws->query_capacity);
  if (ws->query_text) {
    ws->query_text[0] = '\0';
    ws->query_len = 0;
  }

  app->num_workspaces++;
  app->current_workspace = new_idx;

  return ws;
}

bool app_workspace_close(AppState *app, size_t index) {
  if (!app || index >= app->num_workspaces)
    return false;

  Workspace *ws = &app->workspaces[index];

  /* Free workspace data */
  workspace_free_data(ws);
  memset(ws, 0, sizeof(Workspace));

  /* Shift remaining workspaces down */
  for (size_t i = index; i < app->num_workspaces - 1; i++) {
    app->workspaces[i] = app->workspaces[i + 1];
  }
  memset(&app->workspaces[app->num_workspaces - 1], 0, sizeof(Workspace));

  app->num_workspaces--;

  /* Adjust current workspace index */
  if (app->num_workspaces > 0) {
    if (app->current_workspace >= app->num_workspaces) {
      app->current_workspace = app->num_workspaces - 1;
    }
  } else {
    app->current_workspace = 0;
  }

  return true;
}

Workspace *app_workspace_at(AppState *app, size_t index) {
  if (!app || index >= app->num_workspaces)
    return NULL;
  return &app->workspaces[index];
}

/* ============================================================================
 * Navigation Operations
 * ============================================================================
 */

bool workspace_move_cursor(Workspace *ws, int row_delta, int col_delta,
                           int visible_rows) {
  if (!ws || !ws->data)
    return false;

  bool moved = false;
  size_t old_row = ws->cursor_row;
  size_t old_col = ws->cursor_col;

  /* Update row */
  if (row_delta < 0 && ws->cursor_row > 0) {
    ws->cursor_row--;
    moved = true;
  } else if (row_delta > 0 && ws->data->num_rows > 0 &&
             ws->cursor_row < ws->data->num_rows - 1) {
    ws->cursor_row++;
    moved = true;
  }

  /* Update column */
  if (col_delta < 0 && ws->cursor_col > 0) {
    ws->cursor_col--;
    moved = true;
  } else if (col_delta > 0 && ws->cursor_col < ws->data->num_columns - 1) {
    ws->cursor_col++;
    moved = true;
  }

  /* Adjust vertical scroll */
  if (visible_rows < 1)
    visible_rows = 1;

  if (ws->cursor_row < ws->scroll_row) {
    ws->scroll_row = ws->cursor_row;
  } else if (ws->cursor_row >= ws->scroll_row + (size_t)visible_rows) {
    ws->scroll_row = ws->cursor_row - visible_rows + 1;
  }

  return moved || (old_row != ws->cursor_row) || (old_col != ws->cursor_col);
}

void workspace_page_up(Workspace *ws, int visible_rows) {
  if (!ws || !ws->data)
    return;

  if (visible_rows < 1)
    visible_rows = 1;

  if (ws->cursor_row > (size_t)visible_rows) {
    ws->cursor_row -= visible_rows;
  } else {
    ws->cursor_row = 0;
  }

  if (ws->scroll_row > (size_t)visible_rows) {
    ws->scroll_row -= visible_rows;
  } else {
    ws->scroll_row = 0;
  }

  /* Ensure cursor visible */
  if (ws->cursor_row < ws->scroll_row) {
    ws->scroll_row = ws->cursor_row;
  } else if (ws->cursor_row >= ws->scroll_row + (size_t)visible_rows) {
    ws->scroll_row = ws->cursor_row - visible_rows + 1;
  }
}

void workspace_page_down(Workspace *ws, int visible_rows) {
  if (!ws || !ws->data)
    return;

  if (visible_rows < 1)
    visible_rows = 1;

  size_t target_row = ws->cursor_row + visible_rows;
  if (target_row >= ws->data->num_rows) {
    target_row = ws->data->num_rows > 0 ? ws->data->num_rows - 1 : 0;
  }

  ws->cursor_row = target_row;
  ws->scroll_row += visible_rows;

  size_t max_scroll = ws->data->num_rows > (size_t)visible_rows
                          ? ws->data->num_rows - visible_rows
                          : 0;
  if (ws->scroll_row > max_scroll) {
    ws->scroll_row = max_scroll;
  }

  /* Ensure cursor visible */
  if (ws->cursor_row < ws->scroll_row) {
    ws->scroll_row = ws->cursor_row;
  } else if (ws->cursor_row >= ws->scroll_row + (size_t)visible_rows) {
    ws->scroll_row = ws->cursor_row - visible_rows + 1;
  }
}

void workspace_home(Workspace *ws) {
  if (!ws)
    return;
  ws->cursor_row = 0;
  ws->cursor_col = 0;
  ws->scroll_row = 0;
  ws->scroll_col = 0;
}

void workspace_end(Workspace *ws, int visible_rows) {
  if (!ws || !ws->data)
    return;

  if (visible_rows < 1)
    visible_rows = 1;

  ws->cursor_row = ws->data->num_rows > 0 ? ws->data->num_rows - 1 : 0;

  ws->scroll_row = ws->data->num_rows > (size_t)visible_rows
                       ? ws->data->num_rows - visible_rows
                       : 0;
}

void workspace_column_first(Workspace *ws) {
  if (!ws)
    return;
  ws->cursor_col = 0;
  ws->scroll_col = 0;
}

void workspace_column_last(Workspace *ws) {
  if (!ws || !ws->data)
    return;
  ws->cursor_col =
      ws->data->num_columns > 0 ? ws->data->num_columns - 1 : 0;
}

/* ============================================================================
 * Pagination State
 * ============================================================================
 */

int workspace_check_data_edge(Workspace *ws, size_t threshold) {
  if (!ws || !ws->data)
    return 0;

  size_t rows_from_start = ws->cursor_row;
  size_t rows_from_end =
      ws->data->num_rows > ws->cursor_row
          ? ws->data->num_rows - ws->cursor_row
          : 0;

  if (rows_from_start < threshold && ws->loaded_offset > 0) {
    return -1; /* Near start, more data available before */
  }

  if (rows_from_end < threshold) {
    size_t loaded_end = ws->loaded_offset + ws->loaded_count;
    if (loaded_end < ws->total_rows) {
      return 1; /* Near end, more data available after */
    }
  }

  return 0; /* Not near edge or no more data */
}

bool workspace_has_more_data_forward(Workspace *ws) {
  if (!ws)
    return false;
  size_t loaded_end = ws->loaded_offset + ws->loaded_count;
  return loaded_end < ws->total_rows;
}

bool workspace_has_more_data_backward(Workspace *ws) {
  if (!ws)
    return false;
  return ws->loaded_offset > 0;
}

void workspace_update_pagination(Workspace *ws, size_t loaded_offset,
                                 size_t loaded_count, size_t total_rows) {
  if (!ws)
    return;
  ws->loaded_offset = loaded_offset;
  ws->loaded_count = loaded_count;
  ws->total_rows = total_rows;
}
