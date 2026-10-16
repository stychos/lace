/*
 * Lace
 * Cell editing and row deletion
 *
 * Uses VmTable for data access where possible.
 * TUI-specific code remains for ncurses dialogs and confirmation UI.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/config.h"
#include "../../liblace/include/util/mem.h"
#include "tui_internal.h"
#include "views/editor_view.h"
#include "views/text_input.h"
#include "row_key.h"
#include "core/undo_stack.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>


/* Note: History recording is now handled automatically by the database layer
 * via the history callback set up in app_add_connection(). */

bool tui_edit_new_value(const DbValue *orig, LaceValueType col_type,
                        const char *buffer, DbValue *out, bool *unchanged,
                        char **err) {
  *unchanged = false;
  memset(out, 0, sizeof(*out));
  bool orig_null = !orig || orig->is_null;
  if (!buffer) {
    /* Explicit NULL (Ctrl+N, the editor's NULL action) */
    *unchanged = orig_null;
    *out = db_value_null();
    return true;
  }
  if (orig_null && buffer[0] == '\0') {
    *unchanged = true; /* Opened a NULL cell and left it empty */
    return true;
  }
  if (!orig_null) {
    char *before = lace_value_to_edit_text(orig);
    bool same = before && strcmp(before, buffer) == 0;
    free(before);
    if (same) {
      *unchanged = true; /* Nothing to write (and nothing to round) */
      return true;
    }
  }
  /* Same kind of value as the cell held: hex is bytes for a BLOB, digits
   * stay exact for a DECIMAL; an empty text is the empty string, not NULL */
  LaceValueType like = orig_null ? col_type : orig->type;
  return lace_value_from_edit_text(buffer, like, out, err);
}

/* ==========================================================================
 * Writing cells and deleting rows: one path for table tabs and query results
 * ========================================================================== */

bool tui_edit_target_table(TuiState *state, EditTarget *t) {
  memset(t, 0, sizeof(*t));
  VmTable *vm = tui_vm_table(state);
  Tab *tab = TUI_TAB(state);
  if (!vm || !tab)
    return false;
  t->conn = table_vm_connection(vm);
  t->table = table_vm_table_name(vm);
  t->schema = table_vm_get_schema(vm);
  t->data = tab->data;
  t->tab = tab;
  return t->conn && t->table && t->schema && t->data;
}

void tui_edit_target_query(TuiState *state, Tab *tab, EditTarget *t) {
  memset(t, 0, sizeof(*t));
  t->conn = TUI_CONN(state);
  t->table = tab->query_source_table;
  t->schema = tab->query_source_schema;
  t->data = tab->query_results;
  t->tab = tab;
  t->query_result = true;
}

static DbValue *target_cell(const EditTarget *t, size_t row, size_t col) {
  ResultSet *d = t->data;
  if (!d || !d->rows || row >= d->num_rows || col >= d->num_columns ||
      !d->rows[row].cells || col >= d->rows[row].num_cells)
    return NULL;
  return &d->rows[row].cells[col];
}

/* The row's key; query results need every key column exactly once */
static RowKeyStatus target_row_key(const EditTarget *t, size_t row,
                                   RowKey *key, const char **why) {
  if (!t->conn || !t->table || !t->schema) {
    memset(key, 0, sizeof(*key));
    *why = "the source table is not known";
    return ROW_KEY_NONE;
  }
  return row_key_build(key, t->data, row, t->schema, t->query_result, why);
}

/* Re-read the row as the UPDATE or DELETE will address it: the edited cell
 * (col), or every loaded column of the table (SIZE_MAX). Query results are
 * always checked whole: an aliased or computed column, or a same-shaped
 * other table, must not be written through. */
static bool target_row_verified(const EditTarget *t, size_t row, size_t col,
                                const RowKey *key, char **why) {
  if (t->query_result)
    col = SIZE_MAX;
  return row_key_verify(t->conn, t->table, key, t->data, row, col, t->schema,
                        why);
}

/* Is column col of t->data part of the table tab's page order (a sort
 * column or a primary-key column)? Editing one moves its row in the order. */
static bool orders_by_column(const EditTarget *t, size_t col) {
  const char *name = t->data->columns[col].name;
  if (!name || !t->schema)
    return false;
  for (size_t k = 0; k < t->schema->num_columns; k++) {
    const ColumnDef *c = &t->schema->columns[k];
    if (!c->name || strcmp(c->name, name) != 0)
      continue;
    if (c->primary_key)
      return true;
    for (size_t i = 0; i < t->tab->num_sort_entries; i++) {
      if (t->tab->sort_entries[i].column == k)
        return true;
    }
  }
  return false;
}

/* The TUI's RowKey (names borrowed) as liblace's (owned), the shape the
 * undo stack keeps; an edited key column holds the new value afterwards */
static void undo_key(const RowKey *pk, const char *column, const LaceValue *value, LaceRowKey *out) {
  memset(out, 0, sizeof *out);
  out->names = safe_calloc(pk->count + 1, sizeof *out->names);
  out->values = safe_calloc(pk->count + 1, sizeof *out->values);
  for (size_t i = 0; i < pk->count; i++) {
    out->names[i] = str_dup(pk->col_names[i]);
    out->values[i] = lace_value_copy(column && !strcmp(pk->col_names[i], column) ? value : &pk->values[i]);
  }
  out->count = pk->count;
}

bool tui_edit_write_cell(TuiState *state, const EditTarget *t, size_t row,
                         size_t col, const char *text, const char *done_msg) {
  DbValue *cell = target_cell(t, row, col);
  if (!cell)
    return false;

  /* The new value, from the text and what the cell held: nothing is
   * written for an unchanged value, and an empty text is '' (not NULL) */
  DbValue new_val;
  bool unchanged = false;
  char *conv_err = NULL;
  if (!tui_edit_new_value(cell, t->data->columns[col].type, text, &new_val,
                          &unchanged, &conv_err)) {
    tui_set_error(state, "Not updated: %s", conv_err ? conv_err : "invalid value");
    free(conv_err);
    return false;
  }
  if (unchanged) {
    tui_set_status(state, "No change");
    db_value_free(&new_val);
    return false;
  }

  RowKey pk = {0};
  const char *pk_why = NULL;
  RowKeyStatus key_status = target_row_key(t, row, &pk, &pk_why);
  if (key_status == ROW_KEY_BAD ||
      (key_status == ROW_KEY_NONE && !t->query_result)) {
    /* A row that cannot be addressed: say so instead of quietly keeping
     * the edit on screen only */
    tui_set_error(state, "Not updated: %s", pk_why);
    db_value_free(&new_val);
    return false;
  }

  if (key_status == ROW_KEY_OK) {
    char *why = NULL;
    if (!target_row_verified(t, row, col, &pk, &why)) {
      tui_set_error(state, "Not updated: %s", why ? why : "row check failed");
      free(why);
      row_key_free(&pk);
      db_value_free(&new_val);
      return false;
    }
    char *err = NULL;
    bool ok = db_update_cell(t->conn, t->table, pk.col_names, pk.values,
                             pk.count, t->data->columns[col].name, &new_val,
                             &err);
    if (ok && t->tab) { /* for `undo`: the row as it is now, the value it had */
      LaceRowKey after = {0};
      undo_key(&pk, t->data->columns[col].name, &new_val, &after);
      undo_stack_push_cell(&t->tab->undo, t->table, &after, t->data->columns[col].name, cell, &new_val);
      lace_row_key_free(&after);
    }
    row_key_free(&pk);
    if (!ok) {
      tui_set_error(state, "Update failed: %s", err ? err : "unknown error");
      free(err);
      db_value_free(&new_val);
      return false;
    }
  }

  db_value_free(cell);
  *cell = new_val;
  if (!t->query_result && orders_by_column(t, col))
    t->tab->keyset_edge_stale = true;

  if (key_status == ROW_KEY_OK) {
    tui_set_status(state, "%s", done_msg);
    app_mark_table_tabs_dirty(state->app, t->tab->connection_index, t->table,
                              t->tab);
  } else if (!t->conn) {
    tui_set_status(state, "Cell updated (not connected)");
  } else if (!t->table) {
    tui_set_status(state, "Cell updated (local only - complex query)");
  } else {
    tui_set_status(state, "Cell updated (local only - %s)", pk_why);
  }
  return true;
}

void tui_edit_paste_cell(TuiState *state, const EditTarget *t, size_t row,
                         size_t col) {
  const DbValue *cell = target_cell(t, row, col);
  if (!cell)
    return;
  if (cell->omitted) {
    tui_set_error(state, "This value is too large to load: it can't be edited here");
    return;
  }
  char *text = tui_clipboard_read(state);
  if (!text || !text[0]) {
    /* Nothing to paste: not a request to set NULL */
    free(text);
    tui_set_error(state, "Clipboard is empty");
    return;
  }
  tui_edit_write_cell(state, t, row, col, text, "Cell updated from clipboard");
  free(text);
}

bool tui_edit_delete_row(const EditTarget *t, size_t row, char **err) {
  *err = NULL;
  RowKey pk = {0};
  const char *pk_why = NULL;
  if (target_row_key(t, row, &pk, &pk_why) != ROW_KEY_OK) {
    *err = str_dup(pk_why ? pk_why : "no primary key found");
    return false;
  }
  /* The key must still match exactly this one row, unchanged */
  char *why = NULL;
  if (!target_row_verified(t, row, SIZE_MAX, &pk, &why)) {
    *err = why ? why : str_dup("row check failed");
    row_key_free(&pk);
    return false;
  }
  /* The row as it is on screen, for `undo` (a bulk delete's steps are
   * merged by the caller) */
  UndoRow urow;
  bool omitted = false;
  bool captured = t->tab && undo_row_capture(t->data, row, t->schema, &urow, &omitted);
  bool ok = db_delete_row(t->conn, t->table, pk.col_names, pk.values, pk.count,
                          err);
  row_key_free(&pk);
  if (ok && captured) {
    UndoRow *rows = safe_calloc(2, sizeof *rows);
    rows[0] = urow;
    undo_stack_push_delete(&t->tab->undo, t->table, rows, 1,
                           omitted ? str_dup("a value of the row was too large to load") : NULL);
  } else if (captured) {
    undo_row_free(&urow);
  }
  return ok;
}

bool tui_edit_confirm_delete(TuiState *state, size_t count) {
  bool ask = !state->app || !state->app->config ||
             state->app->config->general.delete_confirmation;
  if (!ask)
    return true;
  char msg[64];
  if (count == 1)
    snprintf(msg, sizeof(msg), "Delete this row?");
  else
    snprintf(msg, sizeof(msg), "Delete %zu selected rows?", count);
  if (!tui_show_confirm_dialog(state, msg)) {
    tui_set_status(state, "Delete cancelled");
    return false;
  }
  return true;
}

static int cmp_desc(const void *a, const void *b) {
  size_t x = *(const size_t *)a, y = *(const size_t *)b;
  return x < y ? 1 : x > y ? -1 : 0;
}

size_t *tui_edit_selection_desc(const Tab *tab) {
  size_t *rows = safe_calloc(tab->num_selected ? tab->num_selected : 1,
                             sizeof(size_t));
  if (tab->num_selected)
    memcpy(rows, tab->selected_rows, tab->num_selected * sizeof(size_t));
  qsort(rows, tab->num_selected, sizeof(size_t), cmp_desc);
  return rows;
}

/* Start inline editing */
void tui_start_edit(TuiState *state) {
  if (!state || state->editing)
    return;

  VmTable *vm = tui_vm_table(state);
  if (!vm)
    return;

  /* Get cursor position from viewmodel */
  size_t cursor_row, cursor_col;
  table_vm_get_cursor(vm, &cursor_row, &cursor_col);

  /* Validate bounds */
  size_t num_rows = table_vm_row_count(vm);
  size_t num_cols = table_vm_col_count(vm);
  if (cursor_row >= num_rows || cursor_col >= num_cols)
    return;

  /* Get current cell value via viewmodel */
  const DbValue *val = table_vm_cell(vm, cursor_row, cursor_col);
  if (!val)
    return;
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
  int col_width = tui_get_column_width(state, cursor_col);
  size_t content_len = content ? strlen(content) : 0;
  bool is_truncated =
      content && utf8_width(content, content_len) > col_width;

  /* Also check if content has newlines (always use modal for multi-line) */
  bool has_newlines = content && strchr(content, '\n') != NULL;

  if (is_truncated || has_newlines) {
    /* Use modal editor for truncated or multi-line content */
    const char *col_name = table_vm_result_column_name(vm, cursor_col);
    char *title = str_printf("Edit: %s", col_name ? col_name : "Cell");

    EditorResult result =
        editor_view_show(state, title ? title : "Edit Cell", content, false);
    free(title);

    if (result.saved) {
      /* Update the cell with new content (or NULL) */
      free(state->edit_buffer);
      if (result.set_null || !result.content) {
        state->edit_buffer = NULL;
        free(result.content); /* Free content even when setting NULL */
      } else {
        state->edit_buffer = result.content; /* Transfer ownership */
      }
      state->editing = true; /* Required for tui_confirm_edit */
      tui_confirm_edit(state);
    } else {
      free(result.content);
    }

    free(content);
  } else {
    /* Use inline editing for short content */
    free(state->edit_buffer);
    state->edit_buffer = content;
    state->edit_pos = state->edit_buffer ? strlen(state->edit_buffer) : 0;
    state->editing = true;
    curs_set(1); /* Show cursor */
  }
}

/* Start modal editing */
void tui_start_modal_edit(TuiState *state) {
  if (!state || state->editing)
    return;

  VmTable *vm = tui_vm_table(state);
  if (!vm)
    return;

  /* Get cursor position from viewmodel */
  size_t cursor_row, cursor_col;
  table_vm_get_cursor(vm, &cursor_row, &cursor_col);

  /* Validate bounds */
  size_t num_rows = table_vm_row_count(vm);
  size_t num_cols = table_vm_col_count(vm);
  if (cursor_row >= num_rows || cursor_col >= num_cols)
    return;

  /* Get current cell value via viewmodel */
  const DbValue *val = table_vm_cell(vm, cursor_row, cursor_col);
  if (!val)
    return;
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
  const char *col_name = table_vm_result_column_name(vm, cursor_col);
  char *title = str_printf("Edit: %s", col_name ? col_name : "Cell");

  EditorResult result =
      editor_view_show(state, title ? title : "Edit Cell", content, false);
  free(title);

  if (result.saved) {
    /* Update the cell with new content (or NULL) */
    free(state->edit_buffer);
    if (result.set_null) {
      state->edit_buffer = NULL;
      free(result.content); /* Free content even when setting NULL */
    } else {
      state->edit_buffer = result.content; /* Transfer ownership */
    }
    state->editing = true; /* Required for tui_confirm_edit */
    tui_confirm_edit(state);
  } else {
    free(result.content);
  }

  free(content);
}

/* Cancel editing */
void tui_cancel_edit(TuiState *state) {
  if (!state)
    return;

  free(state->edit_buffer);
  state->edit_buffer = NULL;
  state->edit_pos = 0;
  state->editing = false;
  curs_set(0); /* Hide cursor */
}

/* Confirm edit and update database */
void tui_confirm_edit(TuiState *state) {
  EditTarget t;
  if (state && state->editing && tui_edit_target_table(state, &t)) {
    size_t row, col;
    table_vm_get_cursor(state->vm_table, &row, &col);
    tui_edit_write_cell(state, &t, row, col, state->edit_buffer,
                        "Cell updated");
  }
  tui_cancel_edit(state);
}

/* Set cell value directly (NULL or empty string) */
void tui_set_cell_direct(TuiState *state, bool set_null) {
  EditTarget t;
  if (!tui_edit_target_table(state, &t))
    return;
  size_t row, col;
  table_vm_get_cursor(state->vm_table, &row, &col);
  tui_edit_write_cell(state, &t, row, col, set_null ? NULL : "",
                      set_null ? "Cell set to NULL" : "Cell set to empty");
}

/* Copy current cell value to clipboard */
void tui_cell_copy(TuiState *state) {
  VmTable *vm = tui_vm_table(state);
  if (!vm)
    return;

  /* Get cursor position from viewmodel */
  size_t cursor_row, cursor_col;
  table_vm_get_cursor(vm, &cursor_row, &cursor_col);

  /* Validate bounds */
  size_t num_rows = table_vm_row_count(vm);
  size_t num_cols = table_vm_col_count(vm);
  if (cursor_row >= num_rows || cursor_col >= num_cols)
    return;

  /* Get current cell value via viewmodel */
  const DbValue *val = table_vm_cell(vm, cursor_row, cursor_col);
  if (!val)
    return;

  /* Convert value to string */
  char *content = NULL;
  if (val->is_null) {
    content = str_dup(""); /* Copy empty string for NULL */
  } else {
    content = db_value_to_string(val);
    if (!content)
      content = str_dup("");
  }

  if (content) {
    if (tui_clipboard_copy(state, content)) {
      tui_set_status(state, "Copied to clipboard");
    } else {
      tui_set_error(state, "Failed to copy to clipboard");
    }
    free(content);
  }
}

/* Paste clipboard content to current cell and update database */
void tui_cell_paste(TuiState *state) {
  EditTarget t;
  if (!tui_edit_target_table(state, &t))
    return;
  size_t row, col;
  table_vm_get_cursor(state->vm_table, &row, &col);
  tui_edit_paste_cell(state, &t, row, col);
}

/* Delete current row or selected rows */
void tui_delete_row(TuiState *state) {
  EditTarget t;
  if (!tui_edit_target_table(state, &t))
    return;
  VmTable *vm = state->vm_table;
  Tab *tab = t.tab;

  /* Get cursor and scroll positions from viewmodel */
  size_t cursor_row, cursor_col;
  table_vm_get_cursor(vm, &cursor_row, &cursor_col);
  size_t scroll_row, scroll_col;
  table_vm_get_scroll(vm, &scroll_row, &scroll_col);

  size_t num_rows = table_vm_row_count(vm);
  if (cursor_row >= num_rows)
    return;

  /* Selected rows: a bulk delete */
  size_t num_selected = tab->num_selected;
  bool bulk_delete = (num_selected > 0);
  if (!tui_edit_confirm_delete(state, bulk_delete ? num_selected : 1))
    return;

  size_t loaded_offset = table_vm_loaded_offset(vm);
  size_t total_rows = table_vm_total_rows(vm);
  size_t deleted_count = 0;
  size_t failed_count = 0;

  if (bulk_delete) {
    /* Highest first; the window is reloaded afterwards */
    size_t *to_delete = tui_edit_selection_desc(tab);
    for (size_t i = 0; i < num_selected; i++) {
      size_t global_row = to_delete[i];
      /* A row outside the loaded window cannot be checked: skip it */
      if (global_row < loaded_offset ||
          (global_row - loaded_offset) >= num_rows) {
        failed_count++;
        continue;
      }
      char *err = NULL;
      if (tui_edit_delete_row(&t, global_row - loaded_offset, &err)) {
        deleted_count++;
        if (total_rows > 0)
          total_rows--;
      } else {
        failed_count++;
      }
      free(err);
    }
    free(to_delete);
    undo_stack_merge_last_deletes(&tab->undo, deleted_count); /* one undo step for the bulk */
    tab_clear_selections(tab);
  } else {
    char *err = NULL;
    if (!tui_edit_delete_row(&t, cursor_row, &err)) {
      tui_set_error(state, "Delete failed: %s", err ? err : "unknown error");
      free(err);
      return;
    }
    deleted_count = 1;
    if (total_rows > 0)
      total_rows--;
  }

  /* Show result message */
  if (deleted_count > 0) {
    if (failed_count > 0) {
      tui_set_status(state, "%zu row(s) deleted, %zu failed", deleted_count,
                     failed_count);
    } else if (deleted_count == 1) {
      tui_set_status(state, "Row deleted");
    } else {
      tui_set_status(state, "%zu rows deleted", deleted_count);
    }

    /* Mark other tabs with the same table as needing refresh */
    app_mark_table_tabs_dirty(state->app, tab->connection_index,
                              table_vm_table_name(vm), tab);

    /* Update Tab (authoritative) */
    tab->total_rows = total_rows;

    /* Calculate absolute row with overflow protection */
    size_t abs_row;
    if (cursor_row > SIZE_MAX - loaded_offset) {
      abs_row = total_rows > 0 ? total_rows - 1 : 0; /* Overflow: use last row */
    } else {
      abs_row = loaded_offset + cursor_row;
      if (abs_row >= total_rows && total_rows > 0)
        abs_row = total_rows - 1;
    }

    size_t target_offset = (abs_row / TUI_PAGE_SIZE) * TUI_PAGE_SIZE;
    tui_load_rows_at(state, target_offset);

    /* Re-read state after reload */
    num_rows = table_vm_row_count(vm);

    if (num_rows > 0) {
      loaded_offset = table_vm_loaded_offset(vm);
      cursor_row = abs_row - loaded_offset;
      if (cursor_row >= num_rows)
        cursor_row = num_rows - 1;

      size_t visual_offset =
          cursor_row >= scroll_row ? cursor_row - scroll_row : 0;
      if (cursor_row >= visual_offset)
        scroll_row = cursor_row - visual_offset;
      else
        scroll_row = 0;

      /* Update via viewmodel */
      table_vm_set_cursor(vm, cursor_row, cursor_col);
      table_vm_set_scroll(vm, scroll_row, scroll_col);
    }
  } else if (failed_count > 0) {
    tui_set_error(state, "Failed to delete %zu row(s)", failed_count);
  }
}

/* Handle edit mode input */
bool tui_edit_buffer_key(char **buf, size_t *pos, const UiEvent *event) {
  bool was_null = *buf == NULL;
  TextInput in;
  text_input_adopt(&in, was_null ? str_dup("") : *buf, *pos);
  bool handled = text_input_handle_key(&in, event);
  char *text = text_input_release(&in, pos);
  if (was_null && text[0] == '\0') {
    free(text); /* Still NULL: nothing was typed */
    text = NULL;
    *pos = 0;
  }
  *buf = text;
  return handled;
}

void tui_edit_buffer_insert(char **buf, size_t *pos, const char *text) {
  if (!text || !text[0])
    return;
  TextInput in;
  text_input_adopt(&in, *buf ? *buf : str_dup(""), *pos);
  text_input_insert(&in, text);
  *buf = text_input_release(&in, pos);
}

CellEditAction tui_cell_edit_key(TuiState *state, char **buf, size_t *pos,
                                 const UiEvent *event) {
  const Config *cfg = state->app ? state->app->config : NULL;

  if (render_event_is_special(event, UI_KEY_ESCAPE))
    return CELL_EDIT_CANCEL;
  if (render_event_is_special(event, UI_KEY_ENTER))
    return CELL_EDIT_CONFIRM;

  /* Ctrl+K - copy the text to the clipboard */
  if (render_event_is_ctrl(event, 'K')) {
    if (*buf && (*buf)[0]) {
      tui_clipboard_copy(state, *buf);
      tui_set_status(state, "Copied to clipboard");
    }
    return CELL_EDIT_CONTINUE;
  }

  /* Ctrl+U - paste from the clipboard at the cursor */
  if (render_event_is_ctrl(event, 'U')) {
    char *paste_text = tui_clipboard_read(state);
    tui_edit_buffer_insert(buf, pos, paste_text);
    free(paste_text);
    return CELL_EDIT_CONTINUE;
  }

  /* Set to NULL / to an empty string, and confirm */
  if (cfg && hotkey_matches(cfg, event, HOTKEY_EDITOR_NULL)) {
    free(*buf);
    *buf = NULL;
    *pos = 0;
    return CELL_EDIT_SET_NULL;
  }
  if (cfg && hotkey_matches(cfg, event, HOTKEY_EDITOR_EMPTY)) {
    free(*buf);
    *buf = str_dup("");
    *pos = 0;
    return CELL_EDIT_SET_EMPTY;
  }

  /* Cursor movement, deletion and text; other keys are swallowed */
  tui_edit_buffer_key(buf, pos, event);
  return CELL_EDIT_CONTINUE;
}

bool tui_handle_edit_input(TuiState *state, const UiEvent *event) {
  if (!state->editing)
    return false;
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  switch (tui_cell_edit_key(state, &state->edit_buffer, &state->edit_pos,
                            event)) {
  case CELL_EDIT_CANCEL:
    tui_cancel_edit(state);
    break;
  case CELL_EDIT_CONFIRM:
  case CELL_EDIT_SET_NULL:
  case CELL_EDIT_SET_EMPTY:
    tui_confirm_edit(state);
    break;
  case CELL_EDIT_CONTINUE:
    break;
  }
  return true; /* Consume all keys when editing */
}
