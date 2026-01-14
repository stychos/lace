/*
 * Lace ncurses frontend
 * Cell editing
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "edit.h"
#include "app.h"
#include <lace.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Initial edit buffer capacity */
#define EDIT_BUFFER_INIT 256

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static char *value_to_string(LaceValue *val) {
  if (!val || val->is_null || val->type == LACE_TYPE_NULL) {
    return strdup("");
  }

  char buf[64];
  switch (val->type) {
  case LACE_TYPE_INT:
    snprintf(buf, sizeof(buf), "%lld", (long long)val->int_val);
    return strdup(buf);

  case LACE_TYPE_FLOAT:
    snprintf(buf, sizeof(buf), "%.15g", val->float_val);
    return strdup(buf);

  case LACE_TYPE_TEXT:
    return val->text.data ? strdup(val->text.data) : strdup("");

  case LACE_TYPE_BLOB:
    return strdup("[BLOB]");

  case LACE_TYPE_BOOL:
    return strdup(val->bool_val ? "true" : "false");

  default:
    return strdup("");
  }
}

static void buffer_insert(EditState *edit, char c) {
  /* Grow buffer if needed */
  if (edit->buffer_len + 2 > edit->buffer_cap) {
    size_t new_cap = edit->buffer_cap * 2;
    char *new_buf = realloc(edit->buffer, new_cap);
    if (!new_buf) return;
    edit->buffer = new_buf;
    edit->buffer_cap = new_cap;
  }

  /* Insert character at cursor */
  memmove(edit->buffer + edit->cursor_pos + 1,
          edit->buffer + edit->cursor_pos,
          edit->buffer_len - edit->cursor_pos + 1);
  edit->buffer[edit->cursor_pos] = c;
  edit->cursor_pos++;
  edit->buffer_len++;
}

static void buffer_delete_back(EditState *edit) {
  if (edit->cursor_pos == 0) return;

  memmove(edit->buffer + edit->cursor_pos - 1,
          edit->buffer + edit->cursor_pos,
          edit->buffer_len - edit->cursor_pos + 1);
  edit->cursor_pos--;
  edit->buffer_len--;
}

static void buffer_delete_forward(EditState *edit) {
  if (edit->cursor_pos >= edit->buffer_len) return;

  memmove(edit->buffer + edit->cursor_pos,
          edit->buffer + edit->cursor_pos + 1,
          edit->buffer_len - edit->cursor_pos);
  edit->buffer_len--;
}

/* ==========================================================================
 * Edit Functions
 * ========================================================================== */

bool edit_start(TuiState *tui, EditState *edit) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->data || !tab->schema) return false;

  size_t row = tab->cursor_row;
  size_t col = tab->cursor_col;

  if (row >= tab->data->num_rows || col >= tab->data->num_columns) {
    return false;
  }

  /* Get current value */
  LaceValue *val = &tab->data->rows[row].cells[col];

  /* Initialize edit state */
  edit->row = row;
  edit->col = col;
  edit->is_null = val->is_null || (val->type == LACE_TYPE_NULL);
  edit->buffer = value_to_string(val);
  edit->buffer_len = strlen(edit->buffer);
  edit->buffer_cap = edit->buffer_len + EDIT_BUFFER_INIT;
  edit->cursor_pos = edit->buffer_len;
  edit->active = true;

  /* Reallocate to have some room */
  char *new_buf = realloc(edit->buffer, edit->buffer_cap);
  if (new_buf) {
    edit->buffer = new_buf;
  }

  tui->app->needs_redraw = true;
  return true;
}

void edit_cancel(EditState *edit) {
  edit->active = false;
  free(edit->buffer);
  edit->buffer = NULL;
  edit->buffer_len = 0;
  edit->buffer_cap = 0;
  edit->cursor_pos = 0;
}

bool edit_confirm(TuiState *tui, EditState *edit) {
  if (!edit->active) return false;

  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->data || !tab->schema) {
    edit_cancel(edit);
    return false;
  }

  /* Find primary key columns */
  size_t pk_count = 0;
  size_t pk_indices[16];
  for (size_t i = 0; i < tab->schema->num_columns && pk_count < 16; i++) {
    if (tab->schema->columns[i].primary_key) {
      pk_indices[pk_count++] = i;
    }
  }

  if (pk_count == 0) {
    app_set_error(tui->app, "Cannot edit: table has no primary key");
    edit_cancel(edit);
    return false;
  }

  /* Build primary key values */
  LacePkValue *pk_values = calloc(pk_count, sizeof(LacePkValue));
  if (!pk_values) {
    edit_cancel(edit);
    return false;
  }

  for (size_t i = 0; i < pk_count; i++) {
    pk_values[i].column = tab->schema->columns[pk_indices[i]].name;
    pk_values[i].value = tab->data->rows[edit->row].cells[pk_indices[i]];
  }

  /* Determine new value type based on column type */
  LaceValue new_val = {0};
  LaceValueType col_type = tab->schema->columns[edit->col].type;

  if (edit->is_null || (edit->buffer_len == 0 && col_type != LACE_TYPE_TEXT)) {
    new_val.type = LACE_TYPE_NULL;
    new_val.is_null = true;
  } else {
    switch (col_type) {
    case LACE_TYPE_INT:
      new_val.type = LACE_TYPE_INT;
      new_val.int_val = strtoll(edit->buffer, NULL, 10);
      break;
    case LACE_TYPE_FLOAT:
      new_val.type = LACE_TYPE_FLOAT;
      new_val.float_val = strtod(edit->buffer, NULL);
      break;
    default:
      new_val.type = LACE_TYPE_TEXT;
      new_val.text.data = edit->buffer;
      new_val.text.len = edit->buffer_len;
      break;
    }
  }

  /* Update via lace client */
  const char *column_name = tab->schema->columns[edit->col].name;
  int err = lace_update(tui->app->client, tab->conn_id, tab->table_name,
                        pk_values, pk_count, column_name, &new_val);

  free(pk_values);

  if (err != LACE_OK) {
    app_set_error(tui->app, lace_client_error(tui->app->client));
    edit_cancel(edit);
    return false;
  }

  /* Refresh data to show updated value */
  app_refresh_data(tui->app);
  app_set_status(tui->app, "Cell updated");

  edit_cancel(edit);
  return true;
}

bool edit_handle_input(TuiState *tui, EditState *edit, int ch) {
  if (!edit->active) return false;

  switch (ch) {
  case '\n':
  case KEY_ENTER:
    edit_confirm(tui, edit);
    return true;

  case 27: /* Escape */
    edit_cancel(edit);
    tui->app->needs_redraw = true;
    return true;

  case KEY_BACKSPACE:
  case 127:
  case 8:
    buffer_delete_back(edit);
    edit->is_null = false;
    tui->app->needs_redraw = true;
    return true;

  case KEY_DC: /* Delete key */
    buffer_delete_forward(edit);
    edit->is_null = false;
    tui->app->needs_redraw = true;
    return true;

  case KEY_LEFT:
    if (edit->cursor_pos > 0) {
      edit->cursor_pos--;
      tui->app->needs_redraw = true;
    }
    return true;

  case KEY_RIGHT:
    if (edit->cursor_pos < edit->buffer_len) {
      edit->cursor_pos++;
      tui->app->needs_redraw = true;
    }
    return true;

  case KEY_HOME:
  case 1: /* Ctrl+A */
    edit->cursor_pos = 0;
    tui->app->needs_redraw = true;
    return true;

  case KEY_END:
  case 5: /* Ctrl+E */
    edit->cursor_pos = edit->buffer_len;
    tui->app->needs_redraw = true;
    return true;

  case 14: /* Ctrl+N - set NULL */
    edit->is_null = true;
    free(edit->buffer);
    edit->buffer = strdup("");
    edit->buffer_len = 0;
    edit->cursor_pos = 0;
    tui->app->needs_redraw = true;
    return true;

  case 4: /* Ctrl+D - set empty */
    edit->is_null = false;
    free(edit->buffer);
    edit->buffer = strdup("");
    edit->buffer_len = 0;
    edit->cursor_pos = 0;
    tui->app->needs_redraw = true;
    return true;

  default:
    /* Insert printable characters */
    if (ch >= 32 && ch < 127) {
      buffer_insert(edit, (char)ch);
      edit->is_null = false;
      tui->app->needs_redraw = true;
      return true;
    }
    break;
  }

  return false;
}

void edit_draw(TuiState *tui, EditState *edit, WINDOW *win, int y, int x, int width) {
  (void)tui;

  if (!edit->active || !win) return;

  /* Draw edit background */
  wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);

  /* Show content */
  const char *display = edit->is_null ? "NULL" : edit->buffer;
  int len = (int)strlen(display);

  /* Truncate if too long */
  if (len > width - 1) {
    len = width - 1;
  }

  mvwprintw(win, y, x, "%-*.*s", width, len, display);

  /* Show cursor */
  int cursor_x = x + (int)edit->cursor_pos;
  if (cursor_x < x + width) {
    wmove(win, y, cursor_x);
    curs_set(1);
  }

  wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
}

bool edit_set_null(TuiState *tui) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->data || !tab->schema) return false;

  /* Find primary key columns */
  size_t pk_count = 0;
  size_t pk_indices[16];
  for (size_t i = 0; i < tab->schema->num_columns && pk_count < 16; i++) {
    if (tab->schema->columns[i].primary_key) {
      pk_indices[pk_count++] = i;
    }
  }

  if (pk_count == 0) {
    app_set_error(tui->app, "Cannot edit: table has no primary key");
    return false;
  }

  /* Build primary key values */
  LacePkValue *pk_values = calloc(pk_count, sizeof(LacePkValue));
  if (!pk_values) return false;

  for (size_t i = 0; i < pk_count; i++) {
    pk_values[i].column = tab->schema->columns[pk_indices[i]].name;
    pk_values[i].value = tab->data->rows[tab->cursor_row].cells[pk_indices[i]];
  }

  LaceValue null_val = { .type = LACE_TYPE_NULL, .is_null = true };

  const char *column_name = tab->schema->columns[tab->cursor_col].name;
  int err = lace_update(tui->app->client, tab->conn_id, tab->table_name,
                        pk_values, pk_count, column_name, &null_val);
  free(pk_values);

  if (err != LACE_OK) {
    app_set_error(tui->app, lace_client_error(tui->app->client));
    return false;
  }

  app_refresh_data(tui->app);
  app_set_status(tui->app, "Cell set to NULL");
  return true;
}

bool edit_set_empty(TuiState *tui) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->data || !tab->schema) return false;

  /* Find primary key columns */
  size_t pk_count = 0;
  size_t pk_indices[16];
  for (size_t i = 0; i < tab->schema->num_columns && pk_count < 16; i++) {
    if (tab->schema->columns[i].primary_key) {
      pk_indices[pk_count++] = i;
    }
  }

  if (pk_count == 0) {
    app_set_error(tui->app, "Cannot edit: table has no primary key");
    return false;
  }

  /* Build primary key values */
  LacePkValue *pk_values = calloc(pk_count, sizeof(LacePkValue));
  if (!pk_values) return false;

  for (size_t i = 0; i < pk_count; i++) {
    pk_values[i].column = tab->schema->columns[pk_indices[i]].name;
    pk_values[i].value = tab->data->rows[tab->cursor_row].cells[pk_indices[i]];
  }

  LaceValue empty_val = { .type = LACE_TYPE_TEXT, .text = { .data = "", .len = 0 } };

  const char *column_name = tab->schema->columns[tab->cursor_col].name;
  int err = lace_update(tui->app->client, tab->conn_id, tab->table_name,
                        pk_values, pk_count, column_name, &empty_val);
  free(pk_values);

  if (err != LACE_OK) {
    app_set_error(tui->app, lace_client_error(tui->app->client));
    return false;
  }

  app_refresh_data(tui->app);
  app_set_status(tui->app, "Cell set to empty");
  return true;
}

bool edit_delete_row(TuiState *tui) {
  Tab *tab = app_current_tab(tui->app);
  if (!tab || !tab->data || !tab->schema) return false;

  /* Find primary key columns */
  size_t pk_count = 0;
  size_t pk_indices[16];
  for (size_t i = 0; i < tab->schema->num_columns && pk_count < 16; i++) {
    if (tab->schema->columns[i].primary_key) {
      pk_indices[pk_count++] = i;
    }
  }

  if (pk_count == 0) {
    app_set_error(tui->app, "Cannot delete: table has no primary key");
    return false;
  }

  /* Build primary key values */
  LacePkValue *pk_values = calloc(pk_count, sizeof(LacePkValue));
  if (!pk_values) return false;

  for (size_t i = 0; i < pk_count; i++) {
    pk_values[i].column = tab->schema->columns[pk_indices[i]].name;
    pk_values[i].value = tab->data->rows[tab->cursor_row].cells[pk_indices[i]];
  }

  int err = lace_delete(tui->app->client, tab->conn_id, tab->table_name,
                        pk_values, pk_count);
  free(pk_values);

  if (err != LACE_OK) {
    app_set_error(tui->app, lace_client_error(tui->app->client));
    return false;
  }

  /* Adjust cursor if needed */
  if (tab->cursor_row > 0 && tab->cursor_row >= tab->data->num_rows - 1) {
    tab->cursor_row--;
  }

  app_refresh_data(tui->app);
  app_set_status(tui->app, "Row deleted");
  return true;
}

void edit_free(EditState *edit) {
  if (!edit) return;
  free(edit->buffer);
  memset(edit, 0, sizeof(EditState));
}
