/*
 * Lace
 * Adding rows: the new-row editor and its INSERT
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
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ===== Add Row Mode ===== */

/* Helper to ensure cursor column is visible in add-row mode */
static void add_row_ensure_col_visible(TuiState *state) {
  if (!state || !state->main_win || !state->adding_row)
    return;

  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;

  /* Get layout info */
  LayoutInfo layout = tui_get_layout_info(state);

  size_t cursor_col = state->new_row_cursor_col;
  size_t scroll_col = tab->scroll_col;
  size_t num_cols = state->new_row_num_cols;

  /* Find visible range */
  size_t first_visible = scroll_col;
  size_t last_visible = scroll_col;
  int x = 1;
  for (size_t col = scroll_col; col < num_cols; col++) {
    int width = tui_get_column_width(state, col);
    if (x + width + 3 > layout.win_cols)
      break;
    x += width + 1;
    last_visible = col;
  }

  if (cursor_col < first_visible) {
    tab->scroll_col = cursor_col;
  } else if (cursor_col > last_visible) {
    /* Scroll right to show cursor */
    tab->scroll_col = cursor_col;
    x = 1;
    while (tab->scroll_col > 0) {
      int width = tui_get_column_width(state, tab->scroll_col);
      if (x + width + 3 > layout.win_cols)
        break;
      x += width + 1;
      if (tab->scroll_col == cursor_col)
        break;
      tab->scroll_col--;
    }
  }
}

/* A NOT NULL column with no default and no generated value: the user must
 * supply it, and an explicitly confirmed empty value means '' */
static bool add_row_col_required(const ColumnDef *col) {
  return !col->auto_increment && !col->generated && !col->nullable &&
         !(col->default_val && col->default_val[0]);
}

/* Initial state of a new-row cell. A placeholder is not part of the INSERT,
 * so the database applies its own default: auto-increment, generated,
 * columns with a default, and required columns without one (so a missing
 * value is reported by the database instead of being stored as ''). */
static void add_row_init_cell(TuiState *state, const TableSchema *schema,
                              size_t i) {
  const ColumnDef *col = &schema->columns[i];
  db_value_free(&state->new_row_values[i]);
  state->new_row_auto_increment[i] = col->auto_increment || col->generated;
  state->new_row_edited[i] = false;
  if (col->auto_increment || col->generated) {
    state->new_row_values[i] = db_value_null();
    state->new_row_placeholders[i] = true;
  } else if (col->default_val && col->default_val[0]) {
    state->new_row_values[i] = db_value_text(col->default_val);
    state->new_row_placeholders[i] = true;
  } else if (col->nullable) {
    state->new_row_values[i] = db_value_null();
    state->new_row_placeholders[i] = false;
  } else {
    state->new_row_values[i] = db_value_text("");
    state->new_row_placeholders[i] = true;
  }
}

/* Start add-row mode - create a temporary row for editing */
bool tui_start_add_row(TuiState *state) {
  if (!state || state->adding_row || state->editing)
    return false;

  VmTable *vm = tui_vm_table(state);
  if (!vm)
    return false;

  const TableSchema *schema = table_vm_get_schema(vm);
  if (!schema || schema->num_columns == 0)
    return false;

  size_t num_cols = schema->num_columns;

  /* Check for overflow before allocations */
  if (num_cols > SIZE_MAX / sizeof(DbValue)) {
    return false;
  }

  /* Allocate arrays for new row data */
  state->new_row_values = safe_calloc(num_cols, sizeof(DbValue));
  state->new_row_placeholders = safe_calloc(num_cols, sizeof(bool));
  state->new_row_auto_increment = safe_calloc(num_cols, sizeof(bool));
  state->new_row_edited = safe_calloc(num_cols, sizeof(bool));

  /* Initialize each column value */
  for (size_t i = 0; i < num_cols; i++) {
    add_row_init_cell(state, schema, i);
  }

  state->new_row_num_cols = num_cols;
  state->new_row_cursor_col = 0;
  state->new_row_edit_buffer = NULL;
  state->new_row_edit_pos = 0;
  state->new_row_cell_editing = false;
  state->adding_row = true;

  tui_set_status(state,
                 "Adding row - Enter to edit, Esc to cancel, F2 to save");
  return true;
}

/* Cancel add-row mode and clean up */
void tui_cancel_add_row(TuiState *state) {
  if (!state)
    return;

  if (state->new_row_values) {
    for (size_t i = 0; i < state->new_row_num_cols; i++) {
      db_value_free(&state->new_row_values[i]);
    }
    free(state->new_row_values);
  }
  free(state->new_row_placeholders);
  free(state->new_row_auto_increment);
  free(state->new_row_edited);
  free(state->new_row_edit_buffer);

  state->new_row_values = NULL;
  state->new_row_placeholders = NULL;
  state->new_row_auto_increment = NULL;
  state->new_row_edited = NULL;
  state->new_row_edit_buffer = NULL;
  state->new_row_num_cols = 0;
  state->new_row_cursor_col = 0;
  state->new_row_edit_pos = 0;
  state->new_row_cell_editing = false;
  state->adding_row = false;

  curs_set(0); /* Hide cursor */
  tui_set_status(state, "Add row cancelled");
}

/* Persist the new row to the database */
bool tui_confirm_add_row(TuiState *state) {
  if (!state || !state->adding_row)
    return false;

  VmTable *vm = tui_vm_table(state);
  if (!vm)
    return false;

  DbConnection *conn = table_vm_connection(vm);
  const char *table = table_vm_table_name(vm);
  const TableSchema *schema = table_vm_get_schema(vm);

  if (!conn || !table || !schema) {
    tui_set_error(state, "Cannot add row: invalid table state");
    return false;
  }

  /* Insert only the columns the user set. A column still showing its
   * placeholder (auto-increment, or a default the database fills in) is left
   * out so the database applies the real default instead of its text. */
  size_t n = state->new_row_num_cols;
  ColumnDef *ins_cols = safe_calloc(n ? n : 1, sizeof(ColumnDef));
  DbValue *ins_vals = safe_calloc(n ? n : 1, sizeof(DbValue));
  size_t num_ins = 0;
  for (size_t i = 0; i < n && i < schema->num_columns; i++) {
    if (state->new_row_placeholders[i] && !state->new_row_edited[i])
      continue;
    ins_cols[num_ins] = schema->columns[i]; /* Shallow: borrowed strings */
    ins_vals[num_ins] = state->new_row_values[i];
    num_ins++;
  }

  char *err = NULL;
  bool success =
      db_insert_row(conn, table, ins_cols, ins_vals, num_ins, &err);
  free(ins_cols);
  free(ins_vals);

  if (success) {
    /* Clean up add-row state */
    if (state->new_row_values) {
      for (size_t i = 0; i < state->new_row_num_cols; i++) {
        db_value_free(&state->new_row_values[i]);
      }
      free(state->new_row_values);
    }
    free(state->new_row_placeholders);
    free(state->new_row_auto_increment);
    free(state->new_row_edited);
    free(state->new_row_edit_buffer);

    state->new_row_values = NULL;
    state->new_row_placeholders = NULL;
    state->new_row_auto_increment = NULL;
    state->new_row_edited = NULL;
    state->new_row_edit_buffer = NULL;
    state->new_row_num_cols = 0;
    state->new_row_cursor_col = 0;
    state->new_row_edit_pos = 0;
    state->new_row_cell_editing = false;
    state->adding_row = false;

    curs_set(0);

    /* Mark other tabs with the same table as needing refresh */
    Tab *tab = TUI_TAB(state);
    if (tab) {
      app_mark_table_tabs_dirty(state->app, tab->connection_index, table, tab);
      /* Reload to show the new row: recounts and keeps the cursor on a
       * loaded row (the new one may sort anywhere) */
      tui_refresh_table(state);
    }
    tui_set_status(state, "Row added");

    return true;
  } else {
    tui_set_error(state, "Insert failed: %s", err ? err : "unknown error");
    free(err);
    return false;
  }
}

/* Store text typed into new-row cell col as the column's kind of value
 * (hex is bytes for a BLOB, digits stay exact for a DECIMAL), as cell edits
 * do. False, with the reason in the status bar, if it does not convert. */
static bool add_row_store_text(TuiState *state, const TableSchema *schema,
                               size_t col, const char *text) {
  LaceValueType like = schema && col < schema->num_columns
                           ? schema->columns[col].type
                           : LACE_TYPE_TEXT;
  LaceValue v;
  char *err = NULL;
  if (!lace_value_from_edit_text(text ? text : "", like, &v, &err)) {
    tui_set_error(state, "%s", err ? err : "Invalid value");
    free(err);
    return false;
  }
  db_value_free(&state->new_row_values[col]);
  state->new_row_values[col] = v;
  state->new_row_edited[col] = true;
  state->new_row_placeholders[col] = false;
  return true;
}

/* Start editing a cell in the new row */
void tui_add_row_start_cell_edit(TuiState *state, size_t col) {
  if (!state || !state->adding_row || col >= state->new_row_num_cols)
    return;
  if (!state->new_row_values || !state->new_row_placeholders ||
      !state->new_row_edited)
    return;

  state->new_row_cursor_col = col;

  /* Get current value as string */
  const DbValue *val = &state->new_row_values[col];
  char *content = NULL;

  if (state->new_row_placeholders[col] && !state->new_row_edited[col]) {
    /* Placeholder - start with empty buffer */
    content = str_dup("");
  } else if (val->is_null) {
    content = str_dup("");
  } else {
    content = lace_value_to_edit_text(val); /* Full hex for a BLOB */
    if (!content)
      content = str_dup("");
  }

  free(state->new_row_edit_buffer);
  state->new_row_edit_buffer = content;
  state->new_row_edit_pos = content ? strlen(content) : 0;
  state->new_row_cell_editing = true;

  curs_set(1); /* Show cursor */
}

/* Confirm cell edit in new row */
bool tui_add_row_confirm_cell(TuiState *state) {
  if (!state || !state->adding_row || !state->new_row_cell_editing)
    return true;

  size_t col = state->new_row_cursor_col;
  if (col >= state->new_row_num_cols)
    return true;

  /* Cleared: back to the column's initial state (its default, or NULL);
   * an explicit NULL is Ctrl+N. A required column stores '' instead. */
  VmTable *vm = tui_vm_table(state);
  const TableSchema *schema = vm ? table_vm_get_schema(vm) : NULL;
  if ((!state->new_row_edit_buffer || state->new_row_edit_buffer[0] == '\0') &&
      schema && col < schema->num_columns &&
      !add_row_col_required(&schema->columns[col])) {
    add_row_init_cell(state, schema, col);
  } else if (!add_row_store_text(state, schema, col, state->new_row_edit_buffer)) {
    return false; /* Keep editing: the text does not fit the column */
  }

  /* Clean up edit state */
  free(state->new_row_edit_buffer);
  state->new_row_edit_buffer = NULL;
  state->new_row_edit_pos = 0;
  state->new_row_cell_editing = false;

  curs_set(0);
  return true;
}

/* Set the edited new-row cell to an explicit NULL or '' and end its edit:
 * unlike a cleared cell, it is part of the INSERT */
void tui_add_row_set_cell(TuiState *state, bool set_null) {
  size_t col = state->new_row_cursor_col;
  if (!state->adding_row || col >= state->new_row_num_cols)
    return;
  db_value_free(&state->new_row_values[col]);
  state->new_row_values[col] = set_null ? db_value_null() : db_value_text("");
  state->new_row_edited[col] = true;
  state->new_row_placeholders[col] = false;
  free(state->new_row_edit_buffer);
  state->new_row_edit_buffer = NULL;
  state->new_row_edit_pos = 0;
  state->new_row_cell_editing = false;
  curs_set(0);
}

/* Cancel cell edit in new row */
void tui_add_row_cancel_cell(TuiState *state) {
  if (!state || !state->adding_row)
    return;

  free(state->new_row_edit_buffer);
  state->new_row_edit_buffer = NULL;
  state->new_row_edit_pos = 0;
  state->new_row_cell_editing = false;

  curs_set(0);
}

/* Handle input in add-row mode */
bool tui_handle_add_row_input(TuiState *state, const UiEvent *event) {
  if (!state || !state->adding_row || !event)
    return false;

  Config *cfg = state->app ? state->app->config : NULL;
  size_t num_cols = state->new_row_num_cols;

  /* If editing a cell, handle cell-level input first */
  if (state->new_row_cell_editing) {

    /* Tab - confirm and move to next column */
    if (render_event_is_special(event, UI_KEY_TAB)) {
      if (tui_add_row_confirm_cell(state) &&
          state->new_row_cursor_col < num_cols - 1) {
        state->new_row_cursor_col++;
      }
      return true;
    }

    CellEditAction action = tui_cell_edit_key(
        state, &state->new_row_edit_buffer, &state->new_row_edit_pos, event);
    switch (action) {
    case CELL_EDIT_CANCEL:
      tui_add_row_cancel_cell(state);
      break;
    case CELL_EDIT_CONFIRM:
      tui_add_row_confirm_cell(state);
      break;
    case CELL_EDIT_SET_NULL:
    case CELL_EDIT_SET_EMPTY:
      /* Explicit: stored as given (part of the INSERT), not cleared */
      tui_add_row_set_cell(state, action == CELL_EDIT_SET_NULL);
      break;
    case CELL_EDIT_CONTINUE:
      break;
    }
    return true; /* Consume all other keys when editing cell */
  }

  /* Not editing a cell - handle row-level navigation */

  /* Escape - cancel add-row mode */
  if (render_event_is_special(event, UI_KEY_ESCAPE)) {
    tui_cancel_add_row(state);
    return true;
  }

  /* F2 (HOTKEY_ROW_SAVE) - save the new row */
  if (cfg && hotkey_matches(cfg, event, HOTKEY_ROW_SAVE)) {
    tui_confirm_add_row(state);
    return true;
  }

  /* Up/Down arrows - save and exit add-row mode */
  if (render_event_is_special(event, UI_KEY_UP) ||
      render_event_is_special(event, UI_KEY_DOWN)) {
    tui_confirm_add_row(state);
    return true;
  }

  /* Left arrow - move to previous column */
  if (render_event_is_special(event, UI_KEY_LEFT)) {
    if (state->new_row_cursor_col > 0) {
      state->new_row_cursor_col--;
      add_row_ensure_col_visible(state);
    }
    return true;
  }

  /* Right arrow - move to next column */
  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    if (state->new_row_cursor_col < num_cols - 1) {
      state->new_row_cursor_col++;
      add_row_ensure_col_visible(state);
    }
    return true;
  }

  /* Tab - move to next column */
  if (render_event_is_special(event, UI_KEY_TAB)) {
    if (state->new_row_cursor_col < num_cols - 1) {
      state->new_row_cursor_col++;
      add_row_ensure_col_visible(state);
    }
    return true;
  }

  /* Home - go to first column */
  if (render_event_is_special(event, UI_KEY_HOME)) {
    state->new_row_cursor_col = 0;
    add_row_ensure_col_visible(state);
    return true;
  }

  /* End - go to last column */
  if (render_event_is_special(event, UI_KEY_END)) {
    state->new_row_cursor_col = num_cols - 1;
    add_row_ensure_col_visible(state);
    return true;
  }

  /* Enter - start editing current cell */
  if (render_event_is_special(event, UI_KEY_ENTER)) {
    tui_add_row_start_cell_edit(state, state->new_row_cursor_col);
    return true;
  }

  /* e or F4 - open modal editor for current cell */
  if (cfg && hotkey_matches(cfg, event, HOTKEY_EDIT_MODAL)) {
    size_t col = state->new_row_cursor_col;
    const DbValue *val = &state->new_row_values[col];

    char *content = NULL;
    if (state->new_row_placeholders[col] && !state->new_row_edited[col]) {
      content = str_dup("");
    } else if (val->is_null) {
      content = str_dup("");
    } else {
      content = lace_value_to_edit_text(val);
      if (!content)
        content = str_dup("");
    }

    VmTable *vm = state->vm_table;
    const char *col_name = vm ? table_vm_column_name(vm, col) : "Cell";
    char *title = str_printf("Edit: %s", col_name ? col_name : "Cell");

    EditorResult result =
        editor_view_show(state, title ? title : "Edit Cell", content, false);
    free(title);
    free(content);

    const TableSchema *schema = vm ? table_vm_get_schema(vm) : NULL;
    if (result.saved && !result.set_null &&
        (!result.content || !result.content[0]) && schema &&
        col < schema->num_columns &&
        !add_row_col_required(&schema->columns[col])) {
      /* Cleared: back to the column's initial state, as inline editing */
      add_row_init_cell(state, schema, col);
      free(result.content);
    } else if (result.saved && result.set_null) {
      db_value_free(&state->new_row_values[col]);
      state->new_row_values[col] = db_value_null();
      state->new_row_edited[col] = true;
      state->new_row_placeholders[col] = false;
      free(result.content);
    } else if (result.saved) {
      add_row_store_text(state, schema, col, result.content);
      free(result.content);
    } else {
      free(result.content);
    }
    return true;
  }

  /* n - set cell to NULL */
  if (cfg && hotkey_matches(cfg, event, HOTKEY_EDITOR_NULL)) {
    size_t col = state->new_row_cursor_col;
    db_value_free(&state->new_row_values[col]);
    state->new_row_values[col] = db_value_null();
    state->new_row_edited[col] = true;
    state->new_row_placeholders[col] = false;
    return true;
  }

  /* Printable character - start editing and insert */
  int key_char = render_event_get_char(event);
  if (render_event_is_char(event) && utf8_is_text_key(key_char)) {
    tui_add_row_start_cell_edit(state, state->new_row_cursor_col);
    /* The typed character replaces the cell's text (also a NULL one) */
    free(state->new_row_edit_buffer);
    state->new_row_edit_buffer = NULL;
    state->new_row_edit_pos = 0;
    tui_edit_buffer_key(&state->new_row_edit_buffer, &state->new_row_edit_pos,
                        event);
    return true;
  }

  /* Consume other keys to prevent leaking to table navigation */
  return true;
}
