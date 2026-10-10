/*
 * Lace
 * Query tab result grid editing functions
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "query_internal.h"
#include "../../liblace/include/util/mem.h"
#include "views/editor_view.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* MAX_PK_COLUMNS is defined in db_types.h */

/* Get column width for query results */
int query_get_col_width(Tab *tab, size_t col) {
  if (tab->query_result_col_widths && col < tab->query_result_num_cols) {
    return tab->query_result_col_widths[col];
  }
  return DEFAULT_COL_WIDTH;
}

/* Start editing a cell in query results (inline or modal based on content) */
void query_result_start_edit(TuiState *state, Tab *tab) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !tab->query_results || !ui || ui->query_result_editing)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;
  if (tab->query_result_col >= tab->query_results->num_columns)
    return;

  Row *row = &tab->query_results->rows[tab->query_result_row];
  if (!row->cells || tab->query_result_col >= row->num_cells)
    return;

  DbValue *val = &row->cells[tab->query_result_col];
  if (val->omitted) {
    tui_set_error(state, "This value is too large to load (%zu bytes): it "
                         "can't be edited here", val->omitted);
    return;
  }

  /* The exact text (all of a BLOB's bytes, full float precision) */
  char *content = lace_value_to_edit_text(val);
  if (!content)
    content = str_dup("");

  /* Check if content is truncated (exceeds column width) */
  int col_width = query_get_col_width(tab, tab->query_result_col);
  size_t content_len = content ? strlen(content) : 0;
  bool is_truncated =
      content && utf8_width(content, content_len) > col_width;

  /* Also check if content has newlines (always use modal for multi-line) */
  bool has_newlines = content && strchr(content, '\n') != NULL;

  if (is_truncated || has_newlines) {
    /* Use modal editor for truncated or multi-line content */
    const char *col_name =
        tab->query_results->columns[tab->query_result_col].name;
    char *title = str_printf("Edit: %s", col_name);

    EditorResult result =
        editor_view_show(state, title ? title : "Edit Cell", content, false);
    free(title);

    if (result.saved) {
      /* Update via the confirm flow */
      free(ui->query_result_edit_buf);
      ui->query_result_edit_buf = result.set_null ? NULL : result.content;
      ui->query_result_editing = true;
      query_result_confirm_edit(state, tab);
    } else {
      free(result.content);
    }

    free(content);
  } else {
    /* Use inline editing for short content */
    free(ui->query_result_edit_buf);
    ui->query_result_edit_buf = content;
    ui->query_result_edit_pos =
        ui->query_result_edit_buf ? strlen(ui->query_result_edit_buf) : 0;
    ui->query_result_editing = true;
    curs_set(1);
  }
}

/* Start modal editing for query results (always uses modal) */
void query_result_start_modal_edit(TuiState *state, Tab *tab) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !tab->query_results || !ui || ui->query_result_editing)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;
  if (tab->query_result_col >= tab->query_results->num_columns)
    return;

  Row *row = &tab->query_results->rows[tab->query_result_row];
  if (!row->cells || tab->query_result_col >= row->num_cells)
    return;

  DbValue *val = &row->cells[tab->query_result_col];
  if (val->omitted) {
    tui_set_error(state, "This value is too large to load (%zu bytes): it "
                         "can't be edited here", val->omitted);
    return;
  }

  /* The exact text (all of a BLOB's bytes, full float precision) */
  char *content = lace_value_to_edit_text(val);
  if (!content)
    content = str_dup("");

  /* Always use modal editor */
  const char *col_name =
      tab->query_results->columns[tab->query_result_col].name;
  char *title = str_printf("Edit: %s", col_name);

  EditorResult result =
      editor_view_show(state, title ? title : "Edit Cell", content, false);
  free(title);

  if (result.saved) {
    /* Update via the confirm flow */
    free(ui->query_result_edit_buf);
    ui->query_result_edit_buf = result.set_null ? NULL : result.content;
    ui->query_result_editing = true;
    query_result_confirm_edit(state, tab);
  } else {
    free(result.content);
  }

  free(content);
}

/* Cancel editing in query results */
void query_result_cancel_edit(TuiState *state, Tab *tab) {
  if (!tab)
    return;

  UITabState *ui = TUI_TAB_UI(state);
  if (!ui)
    return;

  free(ui->query_result_edit_buf);
  ui->query_result_edit_buf = NULL;
  ui->query_result_edit_pos = 0;
  ui->query_result_editing = false;
  curs_set(0);
}

/* Find column index in result set by name */
size_t query_find_column_by_name(Tab *tab, const char *name) {
  if (!tab || !tab->query_results || !name)
    return (size_t)-1;

  for (size_t i = 0; i < tab->query_results->num_columns; i++) {
    if (tab->query_results->columns[i].name &&
        strcmp(tab->query_results->columns[i].name, name) == 0) {
      return i;
    }
  }
  return (size_t)-1;
}

/* Find primary key columns in query results
 * Uses schema if available (for SQLite/PostgreSQL), falls back to result
 * metadata */
size_t query_find_pk_columns(Tab *tab, size_t *pk_indices, size_t max_pks) {
  if (!tab || !tab->query_results || !pk_indices || max_pks == 0)
    return 0;

  /* Editing needs the source table's schema, and every primary-key column
   * must appear exactly once in the results. A partial composite key
   * matched many rows; a duplicated name (e.g. from SELECT *, id) is
   * ambiguous. Without a schema there is no reliable key at all. */
  TableSchema *schema = tab->query_source_schema;
  if (!schema)
    return 0;

  size_t count = 0;
  for (size_t i = 0; i < schema->num_columns; i++) {
    if (!schema->columns[i].primary_key)
      continue;
    const char *pk_name = schema->columns[i].name;
    if (!pk_name || count >= max_pks)
      return 0;
    size_t found = (size_t)-1, hits = 0;
    for (size_t j = 0; j < tab->query_results->num_columns; j++) {
      const char *n = tab->query_results->columns[j].name;
      if (n && strcmp(n, pk_name) == 0) {
        found = j;
        hits++;
      }
    }
    if (hits != 1)
      return 0;
    pk_indices[count++] = found;
  }
  return count;
}

/* Why the last query_pk_info_build() refused a row whose key columns are
 * present (NULL if it did not, or there was simply no usable key) */
static const char *g_pk_problem = NULL;

/* Build PK info from query result row. Returns false on error. */
bool query_pk_info_build(QueryPkInfo *pk, Tab *tab, size_t row_idx) {
  g_pk_problem = NULL;
  if (!pk || !tab || !tab->query_results ||
      row_idx >= tab->query_results->num_rows)
    return false;

  size_t pk_indices[MAX_PK_COLUMNS];
  size_t num_pk = query_find_pk_columns(tab, pk_indices, MAX_PK_COLUMNS);
  if (num_pk == 0)
    return false;

  Row *row = &tab->query_results->rows[row_idx];
  for (size_t i = 0; i < num_pk; i++) {
    if (pk_indices[i] >= tab->query_results->num_columns ||
        pk_indices[i] >= row->num_cells) {
      return false;
    }
    /* A NULL key part matches nothing with "=" and every NULL row with
     * "IS NULL"; NaN/Inf have no exact literal on most databases */
    const DbValue *v = &row->cells[pk_indices[i]];
    if (v->is_null || v->type == DB_TYPE_NULL) {
      g_pk_problem = "its primary key contains NULL";
      return false;
    }
    if (v->type == DB_TYPE_FLOAT && !isfinite(v->float_val)) {
      g_pk_problem = "its primary key is NaN or infinite";
      return false;
    }
  }

  pk->col_names = safe_malloc(sizeof(char *) * num_pk);
  pk->values = safe_malloc(sizeof(DbValue) * num_pk);

  for (size_t i = 0; i < num_pk; i++) {
    pk->col_names[i] = tab->query_results->columns[pk_indices[i]].name;
    pk->values[i] = db_value_copy(&row->cells[pk_indices[i]]);
  }
  pk->count = num_pk;
  return true;
}

void query_pk_info_free(QueryPkInfo *pk) {
  if (!pk)
    return;
  free(pk->col_names);
  if (pk->values) {
    for (size_t i = 0; i < pk->count; i++) {
      db_value_free(&pk->values[i]);
    }
    free(pk->values);
  }
  pk->col_names = NULL;
  pk->values = NULL;
  pk->count = 0;
}

/* Re-read the target row by primary key, exactly as the following UPDATE
 * or DELETE will address it, and require that it is one row whose values
 * still equal what the results show (for every result column that is a
 * column of the source table). Guards against wrong-table detection,
 * aliased/computed columns and rows changed since the query ran. */
static bool query_verify_target_row(TuiState *state, Tab *tab, size_t row_idx,
                                    const QueryPkInfo *pk, char **why) {
  ResultSet *res = tab->query_results;
  TableSchema *schema = tab->query_source_schema;
  if (!res || !schema || row_idx >= res->num_rows) {
    *why = str_dup("results changed");
    return false;
  }
  Row *row = &res->rows[row_idx];

  /* Result columns that name a real column of the table */
  const char **cols = safe_calloc(res->num_columns, sizeof(char *));
  size_t *idx = safe_calloc(res->num_columns, sizeof(size_t));
  size_t n = 0;
  for (size_t j = 0; j < res->num_columns && j < row->num_cells; j++) {
    const char *name = res->columns[j].name;
    if (!name)
      continue;
    for (size_t k = 0; k < schema->num_columns; k++) {
      if (schema->columns[k].name && strcmp(schema->columns[k].name, name) == 0) {
        cols[n] = name;
        idx[n] = j;
        n++;
        break;
      }
    }
  }

  bool ok = false;
  char *err = NULL;
  ResultSet *db = n ? db_select_by_pk(state->conn, tab->query_source_table,
                                      (const char **)pk->col_names, pk->values,
                                      pk->count, cols, n, &err)
                    : NULL;
  if (!db) {
    *why = err ? err : str_dup("cannot re-read the row");
    err = NULL;
  } else if (db->num_rows != 1 || !db->rows) {
    *why = str_dup(db->num_rows == 0 ? "row no longer exists"
                                     : "key matches more than one row");
  } else {
    ok = true;
    for (size_t i = 0; i < n && ok; i++) {
      if (i >= db->rows[0].num_cells) {
        ok = false;
        break;
      }
      char *shown = db_value_to_string(&row->cells[idx[i]]);
      char *actual = db_value_to_string(&db->rows[0].cells[i]);
      bool shown_null = row->cells[idx[i]].is_null;
      bool actual_null = db->rows[0].cells[i].is_null;
      if (shown_null != actual_null ||
          (!shown_null && (!shown || !actual || strcmp(shown, actual) != 0))) {
        ok = false;
        *why = str_printf("column '%s' differs from the database "
                          "(computed/aliased column, or row changed; re-run the query)",
                          cols[i]);
      }
      free(shown);
      free(actual);
    }
  }
  free(err);
  if (db) db_result_free(db);
  free(cols);
  free(idx);
  return ok;
}

/* Confirm edit and update database */
void query_result_confirm_edit(TuiState *state, Tab *tab) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !ui || !ui->query_result_editing || !tab->query_results)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;
  if (tab->query_result_col >= tab->query_results->num_columns)
    return;

  Row *row = &tab->query_results->rows[tab->query_result_row];
  if (tab->query_result_col >= row->num_cells) {
    query_result_cancel_edit(state, tab);
    return;
  }

  /* The new value, from the edited text and what the cell held: nothing is
   * written for an unchanged value, and an empty text is '' (not NULL) */
  DbValue new_val;
  bool unchanged = false;
  char *conv_err = NULL;
  if (!tui_edit_new_value(&row->cells[tab->query_result_col],
                          tab->query_results->columns[tab->query_result_col].type,
                          ui->query_result_edit_buf, &new_val, &unchanged,
                          &conv_err)) {
    tui_set_error(state, "Not updated: %s", conv_err ? conv_err : "invalid value");
    free(conv_err);
    query_result_cancel_edit(state, tab);
    return;
  }
  if (unchanged) {
    tui_set_status(state, "No change");
    db_value_free(&new_val);
    query_result_cancel_edit(state, tab);
    return;
  }

  /* Try to update database if we have table name and primary keys */
  bool db_updated = false;
  bool db_error = false;
  bool can_update_db = false;

  if (state->conn && tab->query_source_table) {
    QueryPkInfo pk = {0};
    bool have_pk = query_pk_info_build(&pk, tab, tab->query_result_row);
    if (!have_pk && g_pk_problem) {
      /* An editable result whose row cannot be addressed: say so instead of
       * quietly keeping the edit on screen only */
      tui_set_error(state, "Not updated: %s", g_pk_problem);
      db_value_free(&new_val);
      query_result_cancel_edit(state, tab);
      return;
    }
    if (have_pk) {
      can_update_db = true;

      char *why = NULL;
      if (!query_verify_target_row(state, tab, tab->query_result_row, &pk, &why)) {
        tui_set_error(state, "Not updated: %s", why ? why : "verification failed");
        free(why);
        query_pk_info_free(&pk);
        db_value_free(&new_val);
        query_result_cancel_edit(state, tab);
        return;
      }

      const char *col_name =
          tab->query_results->columns[tab->query_result_col].name;

      char *err = NULL;
      db_updated =
          db_update_cell(state->conn, tab->query_source_table, pk.col_names,
                         pk.values, pk.count, col_name, &new_val, &err);

      if (!db_updated) {
        db_error = true;
        tui_set_error(state, "Update failed: %s", err ? err : "unknown error");
        free(err);
      }

      query_pk_info_free(&pk);
    }
  }

  /* If database update failed, don't update local cell */
  if (db_error) {
    db_value_free(&new_val);
    query_result_cancel_edit(state, tab);
    return;
  }

  /* Update the local cell value */
  DbValue *cell = &row->cells[tab->query_result_col];
  db_value_free(cell);
  *cell = new_val;

  if (db_updated) {
    tui_set_status(state, "Cell updated");

    /* Mark other tabs with the same table as needing refresh */
    if (tab->query_source_table) {
      app_mark_table_tabs_dirty(state->app, tab->connection_index,
                                tab->query_source_table, tab);
    }
  } else if (!state->conn) {
    tui_set_status(state, "Cell updated (not connected)");
  } else if (!tab->query_source_table) {
    tui_set_status(state, "Cell updated (local only - complex query)");
  } else if (!can_update_db) {
    tui_set_status(state, "Cell updated (local only - no primary key)");
  }

  query_result_cancel_edit(state, tab);
}

/* Set query result cell directly to NULL or empty string */
void query_result_set_cell_direct(TuiState *state, Tab *tab, bool set_null) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !tab->query_results || !ui || ui->query_result_editing)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;
  if (tab->query_result_col >= tab->query_results->num_columns)
    return;

  /* Set up the edit buffer and trigger confirm */
  free(ui->query_result_edit_buf);
  if (set_null) {
    ui->query_result_edit_buf = NULL;
  } else {
    ui->query_result_edit_buf = str_dup("");
  }
  ui->query_result_editing = true;
  query_result_confirm_edit(state, tab);
}

/* Copy query result cell to clipboard */
void query_result_cell_copy(TuiState *state, Tab *tab) {
  if (!tab || !tab->query_results)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;
  if (tab->query_result_col >= tab->query_results->num_columns)
    return;

  /* Get the cell value */
  Row *row = &tab->query_results->rows[tab->query_result_row];
  if (tab->query_result_col >= row->num_cells)
    return;

  DbValue *val = &row->cells[tab->query_result_col];
  char *content = NULL;
  if (val->is_null) {
    content = str_dup("");
  } else {
    content = db_value_to_string(val);
    if (!content)
      content = str_dup("");
  }

  /* Save to internal buffer */
  free(state->clipboard_buffer);
  state->clipboard_buffer = str_dup(content);

  /* Copy to OS clipboard (redirect stderr to avoid error messages) */
#ifdef __APPLE__
  FILE *p = popen("pbcopy 2>/dev/null", "w");
#else
  const char *cmd = getenv("WAYLAND_DISPLAY")
                        ? "wl-copy 2>/dev/null"
                        : "xclip -selection clipboard 2>/dev/null";
  FILE *p = popen(cmd, "w");
#endif
  if (p) {
    fwrite(content, 1, strlen(content), p);
    pclose(p);
  }

  tui_set_status(state, "Copied to clipboard");
  free(content);
}

/* Paste clipboard content to query result cell */
void query_result_cell_paste(TuiState *state, Tab *tab) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !tab->query_results || !ui || ui->query_result_editing)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;
  if (tab->query_result_col >= tab->query_results->num_columns)
    return;

  /* Check if editing is supported (need source table and schema) */
  if (!tab->query_source_table || !tab->query_source_schema) {
    tui_set_error(state, "Cannot paste: source table not identified");
    return;
  }

  /* Read from clipboard */
  char *paste_text = NULL;
  bool os_clipboard_accessible = false;

#ifdef __APPLE__
  FILE *p = popen("pbpaste", "r");
#else
  const char *cmd = getenv("WAYLAND_DISPLAY")
                        ? "wl-paste -n 2>/dev/null"
                        : "xclip -selection clipboard -o 2>/dev/null";
  FILE *p = popen(cmd, "r");
#endif
  if (p) {
    size_t capacity = 4096;
    size_t len = 0;
    paste_text = safe_malloc(capacity);
    int c;
    bool size_exceeded = false;
    while ((c = fgetc(p)) != EOF) {
      if (len + 1 >= capacity) {
        /* Check for overflow before doubling */
        if (capacity > SIZE_MAX / 2) {
          size_exceeded = true;
          break;
        }
        capacity *= 2;
        paste_text = safe_realloc(paste_text, capacity);
      }
      paste_text[len++] = (char)c;
    }
    if (size_exceeded) {
      free(paste_text);
      paste_text = NULL;
    } else {
      paste_text[len] = '\0';
    }
    int status = pclose(p);
    os_clipboard_accessible = (status == 0);
    if (!os_clipboard_accessible || (paste_text && paste_text[0] == '\0')) {
      free(paste_text);
      paste_text = NULL;
    }
  }

  /* Fall back to internal buffer */
  if (!os_clipboard_accessible && state->clipboard_buffer) {
    paste_text = str_dup(state->clipboard_buffer);
  }

  if (!paste_text) {
    tui_set_error(state, "Clipboard is empty");
    return;
  }

  /* Set the edit buffer and trigger confirm to update database */
  free(ui->query_result_edit_buf);
  if (paste_text[0] == '\0') {
    ui->query_result_edit_buf = NULL; /* Empty = NULL */
  } else {
    ui->query_result_edit_buf = paste_text;
    paste_text = NULL; /* Ownership transferred */
  }
  ui->query_result_editing = true;
  query_result_confirm_edit(state, tab);
  free(paste_text);
}

/* Delete a single row from query results by local index */
static bool query_result_delete_single_row(TuiState *state, Tab *tab,
                                           size_t local_row, char **err) {
  if (local_row >= tab->query_results->num_rows)
    return false;

  QueryPkInfo pk = {0};
  if (!query_pk_info_build(&pk, tab, local_row)) {
    *err = str_dup(g_pk_problem ? g_pk_problem : "No primary key found");
    return false;
  }

  char *why = NULL;
  if (!query_verify_target_row(state, tab, local_row, &pk, &why)) {
    *err = str_printf("row %zu: %s", local_row + 1, why ? why : "verification failed");
    free(why);
    query_pk_info_free(&pk);
    return false;
  }

  bool success = db_delete_row(state->conn, tab->query_source_table,
                               pk.col_names, pk.values, pk.count, err);
  query_pk_info_free(&pk);
  return success;
}

/* Remove a row from local results by index */
static void query_result_remove_local_row(Tab *tab, size_t local_row) {
  if (local_row >= tab->query_results->num_rows)
    return;

  Row *row = &tab->query_results->rows[local_row];
  for (size_t j = 0; j < row->num_cells; j++) {
    db_value_free(&row->cells[j]);
  }
  free(row->cells);

  for (size_t i = local_row; i < tab->query_results->num_rows - 1; i++) {
    tab->query_results->rows[i] = tab->query_results->rows[i + 1];
  }
  tab->query_results->num_rows--;
  tab->query_loaded_count--;
  if (tab->query_total_rows > 0)
    tab->query_total_rows--;
}

/* Delete row(s) from query results - bulk if selections exist */
void query_result_delete_row(TuiState *state, Tab *tab) {
  if (!tab || !tab->query_results || !state->conn)
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;

  if (!tab->query_source_table) {
    tui_set_error(state, "Cannot delete: no source table");
    return;
  }

  /* Check if we have selections for bulk delete */
  size_t num_selected = tab->num_selected;

  if (num_selected > 0) {
    /* Bulk delete of selected rows */
    /* Verify all selected rows can be deleted (have PKs) */
    for (size_t i = 0; i < num_selected; i++) {
      size_t global_row = tab->selected_rows[i];
      if (global_row < tab->query_loaded_offset)
        continue;
      size_t local_row = global_row - tab->query_loaded_offset;
      if (local_row >= tab->query_results->num_rows)
        continue;

      QueryPkInfo pk = {0};
      if (!query_pk_info_build(&pk, tab, local_row)) {
        tui_set_error(state, "Cannot delete row %zu: %s", global_row + 1,
                      g_pk_problem ? g_pk_problem : "it has no primary key");
        return;
      }
      query_pk_info_free(&pk);
    }

    /* Show confirmation */
    char msg[64];
    snprintf(msg, sizeof(msg), "Delete %zu selected rows?", num_selected);
    if (!tui_show_confirm_dialog(state, msg)) {
      tui_set_status(state, "Delete cancelled");
      return;
    }

    /* Delete rows from database (in reverse order to preserve indices) */
    size_t deleted = 0;
    size_t errors = 0;

    /* Sort selections in descending order by local index for safe removal */
    /* Check for overflow before allocation */
    if (num_selected > SIZE_MAX / sizeof(size_t)) {
      tui_set_error(state, "Too many rows selected");
      return;
    }
    size_t *sorted = safe_malloc(num_selected * sizeof(size_t));
    memcpy(sorted, tab->selected_rows, num_selected * sizeof(size_t));

    /* Simple bubble sort (selections are typically small) */
    for (size_t i = 0; i < num_selected; i++) {
      for (size_t j = i + 1; j < num_selected; j++) {
        if (sorted[i] < sorted[j]) {
          size_t tmp = sorted[i];
          sorted[i] = sorted[j];
          sorted[j] = tmp;
        }
      }
    }

    /* Delete in descending order */
    for (size_t i = 0; i < num_selected; i++) {
      size_t global_row = sorted[i];
      if (global_row < tab->query_loaded_offset)
        continue;
      size_t local_row = global_row - tab->query_loaded_offset;
      if (local_row >= tab->query_results->num_rows)
        continue;

      char *err = NULL;
      if (query_result_delete_single_row(state, tab, local_row, &err)) {
        query_result_remove_local_row(tab, local_row);
        deleted++;
      } else {
        errors++;
        free(err);
      }
    }

    free(sorted);
    tab_clear_selections(tab);

    /* Adjust cursor position */
    if (tab->query_result_row >= tab->query_results->num_rows &&
        tab->query_results->num_rows > 0)
      tab->query_result_row = tab->query_results->num_rows - 1;

    if (tab->query_result_scroll_row > 0 &&
        tab->query_result_scroll_row >= tab->query_results->num_rows)
      tab->query_result_scroll_row = tab->query_results->num_rows > 0
                                         ? tab->query_results->num_rows - 1
                                         : 0;

    if (errors > 0) {
      tui_set_error(state, "Deleted %zu rows, %zu errors", deleted, errors);
    } else {
      tui_set_status(state, "Deleted %zu rows", deleted);
    }

    /* Mark other tabs with the same table as needing refresh */
    if (deleted > 0 && tab->query_source_table) {
      app_mark_table_tabs_dirty(state->app, tab->connection_index,
                                tab->query_source_table, tab);
    }
    return;
  }

  /* Single row delete (no selections) */
  QueryPkInfo pk = {0};
  if (!query_pk_info_build(&pk, tab, tab->query_result_row)) {
    tui_set_error(state, "Cannot delete: %s",
                  g_pk_problem ? g_pk_problem : "no primary key found");
    return;
  }

  Row *row = &tab->query_results->rows[tab->query_result_row];

  /* Highlight the row being deleted with danger background */
  int win_rows, win_cols;
  getmaxyx(state->main_win, win_rows, win_cols);

  int editor_height = (win_rows - 1) * 3 / 10;
  if (editor_height < 3)
    editor_height = 3;
  int results_start = editor_height + 1;
  int row_y = results_start + 3 +
              (int)(tab->query_result_row - tab->query_result_scroll_row);

  int sidebar_width = state->sidebar_visible ? SIDEBAR_WIDTH : 0;
  wattron(state->main_win, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
  int x = 1;
  for (size_t col = tab->query_result_scroll_col;
       col < tab->query_results->num_columns && col < row->num_cells; col++) {
    int col_width =
        tab->query_result_col_widths ? tab->query_result_col_widths[col] : 15;
    if (x + col_width + 3 > win_cols - sidebar_width)
      break;

    DbValue *val = &row->cells[col];
    if (val->is_null) {
      mvwprintw(state->main_win, row_y, x, "%-*s", col_width, "NULL");
    } else {
      char *str = db_value_to_string(val);
      if (str) {
        char *safe = tui_sanitize_for_display(str);
        tui_print_fit(state->main_win, row_y, x, col_width, safe ? safe : str);
        free(safe);
        free(str);
      }
    }
    x += col_width + 1;
    wattron(state->main_win, COLOR_PAIR(COLOR_BORDER));
    mvwaddch(state->main_win, row_y, x - 1, ACS_VLINE);
    wattroff(state->main_win, COLOR_PAIR(COLOR_BORDER));
  }
  wattroff(state->main_win, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
  wrefresh(state->main_win);

  /* Check delete confirmation setting */
  bool needs_confirm = true;
  if (state->app && state->app->config) {
    needs_confirm = state->app->config->general.delete_confirmation;
  }

  if (needs_confirm && !tui_show_confirm_dialog(state, "Delete this row?")) {
    tui_set_status(state, "Delete cancelled");
    query_pk_info_free(&pk);
    return;
  }

  char *why = NULL;
  if (!query_verify_target_row(state, tab, tab->query_result_row, &pk, &why)) {
    tui_set_error(state, "Not deleted: %s", why ? why : "verification failed");
    free(why);
    query_pk_info_free(&pk);
    return;
  }

  char *err = NULL;
  bool success = db_delete_row(state->conn, tab->query_source_table,
                               pk.col_names, pk.values, pk.count, &err);
  query_pk_info_free(&pk);

  if (success) {
    tui_set_status(state, "Row deleted");

    /* Mark other tabs with the same table as needing refresh */
    if (tab->query_source_table) {
      app_mark_table_tabs_dirty(state->app, tab->connection_index,
                                tab->query_source_table, tab);
    }

    query_result_remove_local_row(tab, tab->query_result_row);

    if (tab->query_result_row >= tab->query_results->num_rows &&
        tab->query_results->num_rows > 0)
      tab->query_result_row = tab->query_results->num_rows - 1;

    if (tab->query_result_scroll_row > 0 &&
        tab->query_result_scroll_row >= tab->query_results->num_rows)
      tab->query_result_scroll_row = tab->query_results->num_rows > 0
                                         ? tab->query_results->num_rows - 1
                                         : 0;
  } else {
    tui_set_error(state, "Delete failed: %s", err ? err : "unknown error");
    free(err);
  }
}

/* Handle edit input for query results */
bool query_result_handle_edit_input(TuiState *state, Tab *tab,
                                    const UiEvent *event) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!ui || !ui->query_result_editing)
    return false;
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  size_t len =
      ui->query_result_edit_buf ? strlen(ui->query_result_edit_buf) : 0;
  int key_char = render_event_get_char(event);

  /* Escape - cancel */
  if (render_event_is_special(event, UI_KEY_ESCAPE)) {
    query_result_cancel_edit(state, tab);
    return true;
  }

  /* Enter - confirm */
  if (render_event_is_special(event, UI_KEY_ENTER)) {
    query_result_confirm_edit(state, tab);
    return true;
  }

  /* Left arrow */
  if (render_event_is_special(event, UI_KEY_LEFT)) {
    ui->query_result_edit_pos =
        utf8_prev(ui->query_result_edit_buf, ui->query_result_edit_pos);
    return true;
  }

  /* Right arrow */
  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    if (ui->query_result_edit_buf)
      ui->query_result_edit_pos = utf8_next(ui->query_result_edit_buf, len,
                                            ui->query_result_edit_pos);
    return true;
  }

  /* Home */
  if (render_event_is_special(event, UI_KEY_HOME)) {
    ui->query_result_edit_pos = 0;
    return true;
  }

  /* End */
  if (render_event_is_special(event, UI_KEY_END)) {
    ui->query_result_edit_pos = len;
    return true;
  }

  /* Backspace */
  if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
    if (ui->query_result_edit_pos > 0 && ui->query_result_edit_buf) {
      size_t prev =
          utf8_prev(ui->query_result_edit_buf, ui->query_result_edit_pos);
      memmove(ui->query_result_edit_buf + prev,
              ui->query_result_edit_buf + ui->query_result_edit_pos,
              len - ui->query_result_edit_pos + 1);
      ui->query_result_edit_pos = prev;
    }
    return true;
  }

  /* Delete */
  if (render_event_is_special(event, UI_KEY_DELETE)) {
    if (ui->query_result_edit_pos < len && ui->query_result_edit_buf) {
      size_t next = utf8_next(ui->query_result_edit_buf, len,
                              ui->query_result_edit_pos);
      memmove(ui->query_result_edit_buf + ui->query_result_edit_pos,
              ui->query_result_edit_buf + next, len - next + 1);
    }
    return true;
  }

  /* Ctrl+U - clear line */
  if (render_event_is_ctrl(event, 'U')) {
    if (ui->query_result_edit_buf) {
      ui->query_result_edit_buf[0] = '\0';
      ui->query_result_edit_pos = 0;
    }
    return true;
  }

  /* Ctrl+N - set to NULL */
  if (render_event_is_ctrl(event, 'N')) {
    free(ui->query_result_edit_buf);
    ui->query_result_edit_buf = NULL;
    ui->query_result_edit_pos = 0;
    query_result_confirm_edit(state, tab);
    return true;
  }

  /* Printable character - insert */
  if (render_event_is_char(event) && utf8_is_text_key(key_char)) {
    size_t new_len = len + 2;
    ui->query_result_edit_buf = safe_realloc(ui->query_result_edit_buf, new_len);
    if (len == 0) {
      ui->query_result_edit_buf[0] = (char)key_char;
      ui->query_result_edit_buf[1] = '\0';
      ui->query_result_edit_pos = 1;
    } else {
      memmove(ui->query_result_edit_buf + ui->query_result_edit_pos + 1,
              ui->query_result_edit_buf + ui->query_result_edit_pos,
              len - ui->query_result_edit_pos + 1);
      ui->query_result_edit_buf[ui->query_result_edit_pos] = (char)key_char;
      ui->query_result_edit_pos++;
    }
    return true;
  }

  /* Consume all other keys when editing */
  return true;
}
