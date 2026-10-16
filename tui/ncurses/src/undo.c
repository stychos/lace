/* Undo in this TUI: the tab's stack (core/undo_stack.h, `Tab.undo`, filled
 * by edit.c as writes succeed) and the writes that revert its last step,
 * done here and now like every other write of this TUI: a verified UPDATE
 * back to the old value (refused when the row changed since), or INSERTs of
 * the deleted rows as they were on screen. After a restore the tables that
 * reference this one are looked at: rows the database removed or changed
 * with the delete (ON DELETE CASCADE, SET NULL, SET DEFAULT) do not come
 * back, and the status says so. */
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include "core/undo_stack.h"
#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>

static bool rule_touches_rows(const char *on_delete) {
  if (!on_delete) return false;
  return !strcmp(on_delete, "CASCADE") || !strcmp(on_delete, "SET NULL") || !strcmp(on_delete, "SET DEFAULT");
}

/* The referencing tables whose rows a delete of ours changes, as a list */
static char *dependents_list(DbConnection *conn, const char *table) {
  LaceDependent *deps = NULL;
  size_t n = 0;
  char *err = NULL;
  if (!db_table_dependents(conn, table, &deps, &n, &err)) {
    free(err);
    return NULL;
  }
  char *list = NULL;
  for (size_t i = 0; i < n; i++) {
    if (!rule_touches_rows(deps[i].on_delete)) continue;
    if (list && strstr(list, deps[i].table)) continue;
    char *next = str_printf("%s%s%s", list ? list : "", list ? ", " : "", deps[i].table);
    free(list);
    list = next;
  }
  lace_dependents_free(deps, n);
  return list;
}

static void undo_cell(TuiState *state, const EditTarget *t, UndoEntry *e) {
  const char **names = (const char **)e->key.names;
  const char *cols[1] = {e->column};
  char *why = NULL;
  if (!db_verify_row(t->conn, e->table, names, e->key.values, e->key.count, cols, &e->after, 1, &why)) {
    tui_set_error(state, "Not undone: %s", why ? why : "the row changed since");
    free(why);
    return;
  }
  char *err = NULL;
  if (!db_update_cell(t->conn, e->table, names, e->key.values, e->key.count, e->column, &e->before, &err)) {
    tui_set_error(state, "Not undone: %s", err ? err : "unknown error");
    free(err);
    return;
  }
  app_mark_table_tabs_dirty(state->app, t->tab->connection_index, e->table, t->tab);
  if (t->tab->type == TAB_TYPE_TABLE) tui_refresh_table(state); /* its own status comes first */
  char *text = e->before.is_null ? NULL : db_value_to_string(&e->before);
  tui_set_status(state, "Undone: %s back to %s", e->column, e->before.is_null ? "NULL" : text ? text : "");
  free(text);
}

static void undo_delete(TuiState *state, const EditTarget *t, UndoEntry *e) {
  if (e->not_restorable) {
    tui_set_error(state, "Cannot restore the deleted row%s: %s", e->num_rows == 1 ? "" : "s", e->not_restorable);
    return;
  }
  if (!t->schema) {
    tui_set_error(state, "Not undone: the table's structure is not loaded");
    return;
  }
  size_t done = 0, failed = 0;
  char *first_error = NULL;
  for (size_t i = 0; i < e->num_rows; i++) {
    const UndoRow *r = &e->rows[i];
    /* db_insert_row takes the columns themselves: the schema's, by name */
    LaceColumn *cols = safe_calloc(r->count + 1, sizeof *cols);
    LaceValue *vals = safe_calloc(r->count + 1, sizeof *vals);
    size_t n = 0;
    for (size_t c = 0; c < r->count; c++) {
      size_t at = db_schema_column_index(t->schema, r->names[c]);
      if (at == SIZE_MAX) continue;
      cols[n] = t->schema->columns[at]; /* borrowed: never freed through cols */
      vals[n++] = r->values[c];
    }
    char *err = NULL;
    if (db_insert_row(t->conn, e->table, cols, vals, n, &err)) {
      done++;
    } else {
      failed++;
      if (!first_error) first_error = err ? err : str_dup("unknown error");
      else free(err);
    }
    free(cols);
    free(vals);
  }
  if (done) {
    app_mark_table_tabs_dirty(state->app, t->tab->connection_index, e->table, t->tab);
    if (t->tab->type == TAB_TYPE_TABLE) tui_refresh_table(state); /* its own status comes first */
  }
  if (failed) {
    tui_set_error(state, "Not undone: %s", first_error ? first_error : "error");
  } else {
    char *list = dependents_list(t->conn, e->table);
    if (list)
      tui_set_error(state, "Restored %zu row%s; rows of %s the database removed or changed with them are not restored",
                    done, done == 1 ? "" : "s", list);
    else
      tui_set_status(state, "Restored %zu row%s", done, done == 1 ? "" : "s");
    free(list);
  }
  free(first_error);
}

void tui_undo(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  EditTarget t;
  if (tab && tab->type == TAB_TYPE_TABLE) {
    if (!tui_edit_target_table(state, &t)) return;
  } else if (tab && tab->type == TAB_TYPE_QUERY && tab->query_results) {
    tui_edit_target_query(state, tab, &t);
  } else {
    tui_set_status(state, "Nothing to undo");
    return;
  }
  UndoEntry *e = undo_stack_top(&t.tab->undo);
  if (!e) {
    tui_set_status(state, "Nothing to undo");
    return;
  }
  if (!t.conn) {
    tui_set_error(state, "Not undone: not connected");
    return;
  }
  if (e->kind == UNDO_CELL) undo_cell(state, &t, e);
  else undo_delete(state, &t, e);
  undo_stack_pop(&t.tab->undo); /* one chance: the step is consumed whether or not the write landed */
}
