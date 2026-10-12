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
#include "row_key.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>


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

/* Key of a result row: the source table's primary key, every key column
 * present exactly once in the results (SELECT *, id repeats a name) */
/* Confirm edit and update database */
void query_result_confirm_edit(TuiState *state, Tab *tab) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !ui || !ui->query_result_editing)
    return;
  EditTarget t;
  tui_edit_target_query(state, tab, &t);
  tui_edit_write_cell(state, &t, tab->query_result_row, tab->query_result_col,
                      ui->query_result_edit_buf, "Cell updated");
  query_result_cancel_edit(state, tab);
}

/* Set query result cell directly to NULL or empty string */
void query_result_set_cell_direct(TuiState *state, Tab *tab, bool set_null) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !tab->query_results || !ui || ui->query_result_editing)
    return;
  EditTarget t;
  tui_edit_target_query(state, tab, &t);
  tui_edit_write_cell(state, &t, tab->query_result_row, tab->query_result_col,
                      set_null ? NULL : "",
                      set_null ? "Cell set to NULL" : "Cell set to empty");
}

/* Copy query result cell to clipboard */
void query_result_cell_copy(TuiState *state, Tab *tab) {
  if (!tab || !tab->query_results ||
      tab->query_result_row >= tab->query_results->num_rows ||
      tab->query_result_col >= tab->query_results->num_columns)
    return;
  Row *row = &tab->query_results->rows[tab->query_result_row];
  if (!row->cells || tab->query_result_col >= row->num_cells)
    return;

  DbValue *val = &row->cells[tab->query_result_col];
  char *content = val->is_null ? NULL : db_value_to_string(val);
  if (tui_clipboard_copy(state, content ? content : ""))
    tui_set_status(state, "Copied to clipboard");
  else
    tui_set_error(state, "Failed to copy to clipboard");
  free(content);
}

/* Paste clipboard content to query result cell */
void query_result_cell_paste(TuiState *state, Tab *tab) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || !tab->query_results || !ui || ui->query_result_editing)
    return;
  EditTarget t;
  tui_edit_target_query(state, tab, &t);
  tui_edit_paste_cell(state, &t, tab->query_result_row, tab->query_result_col);
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

/* Keep the cursor and scroll inside the rows left after a delete */
static void clamp_result_cursor(Tab *tab) {
  size_t n = tab->query_results->num_rows;
  if (tab->query_result_row >= n)
    tab->query_result_row = n > 0 ? n - 1 : 0;
  if (tab->query_result_scroll_row > tab->query_result_row)
    tab->query_result_scroll_row = tab->query_result_row;
}

/* Delete row(s) from query results - bulk if selections exist */
void query_result_delete_row(TuiState *state, Tab *tab) {
  if (!tab || !tab->query_results || !TUI_CONN(state))
    return;
  if (tab->query_result_row >= tab->query_results->num_rows)
    return;

  if (!tab->query_source_table) {
    tui_set_error(state, "Cannot delete: no source table");
    return;
  }

  /* Check if we have selections for bulk delete */
  size_t num_selected = tab->num_selected;

  EditTarget t;
  tui_edit_target_query(state, tab, &t);

  if (num_selected > 0) {
    /* Bulk delete: refuse up front if any loaded selected row has no key */
    for (size_t i = 0; i < num_selected; i++) {
      size_t global_row = tab->selected_rows[i];
      if (global_row < tab->query_loaded_offset)
        continue;
      size_t local_row = global_row - tab->query_loaded_offset;
      if (local_row >= tab->query_results->num_rows)
        continue;

      RowKey pk = {0};
      const char *pk_why = NULL;
      if (row_key_build(&pk, tab->query_results, local_row,
                        tab->query_source_schema, true, &pk_why) != ROW_KEY_OK) {
        tui_set_error(state, "Cannot delete row %zu: %s", global_row + 1,
                      pk_why);
        return;
      }
      row_key_free(&pk);
    }

    if (!tui_edit_confirm_delete(state, num_selected))
      return;

    size_t deleted = 0;
    size_t errors = 0;
    size_t *sorted = tui_edit_selection_desc(tab);

    /* Delete in descending order */
    for (size_t i = 0; i < num_selected; i++) {
      size_t global_row = sorted[i];
      /* A row outside the loaded window cannot be checked: not deleted */
      if (global_row < tab->query_loaded_offset ||
          global_row - tab->query_loaded_offset >= tab->query_results->num_rows) {
        errors++;
        continue;
      }
      size_t local_row = global_row - tab->query_loaded_offset;

      char *err = NULL;
      if (tui_edit_delete_row(&t, local_row, &err)) {
        query_result_remove_local_row(tab, local_row);
        deleted++;
      } else {
        errors++;
        free(err);
      }
    }

    free(sorted);
    tab_clear_selections(tab);

    clamp_result_cursor(tab);

    if (errors > 0) {
      tui_set_error(state, "Deleted %zu rows, %zu not deleted", deleted, errors);
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
  RowKey pk = {0};
  const char *pk_why = NULL;
  if (row_key_build(&pk, tab->query_results, tab->query_result_row,
                    tab->query_source_schema, true, &pk_why) != ROW_KEY_OK) {
    tui_set_error(state, "Cannot delete: %s", pk_why);
    return;
  }
  row_key_free(&pk);

  if (!tui_edit_confirm_delete(state, 1))
    return;

  char *err = NULL;
  bool success = tui_edit_delete_row(&t, tab->query_result_row, &err);
  if (success) {
    tui_set_status(state, "Row deleted");

    /* Mark other tabs with the same table as needing refresh */
    if (tab->query_source_table) {
      app_mark_table_tabs_dirty(state->app, tab->connection_index,
                                tab->query_source_table, tab);
    }

    query_result_remove_local_row(tab, tab->query_result_row);

    clamp_result_cursor(tab);
  } else {
    tui_set_error(state, "Not deleted: %s", err ? err : "unknown error");
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

  switch (tui_cell_edit_key(state, &ui->query_result_edit_buf,
                            &ui->query_result_edit_pos, event)) {
  case CELL_EDIT_CANCEL:
    query_result_cancel_edit(state, tab);
    break;
  case CELL_EDIT_CONFIRM:
  case CELL_EDIT_SET_NULL:
  case CELL_EDIT_SET_EMPTY:
    query_result_confirm_edit(state, tab);
    break;
  case CELL_EDIT_CONTINUE:
    break;
  }
  return true; /* Consume all keys when editing */
}
