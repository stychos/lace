#include "core/undo_stack.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

void undo_row_free(UndoRow *r) {
  for (size_t i = 0; i < r->count; i++) {
    free(r->names[i]);
    lace_value_free(&r->values[i]);
  }
  free(r->names);
  free(r->values);
  memset(r, 0, sizeof *r);
}

void undo_entry_free(UndoEntry *e) {
  free(e->table);
  lace_row_key_free(&e->key);
  free(e->column);
  lace_value_free(&e->before);
  lace_value_free(&e->after);
  for (size_t i = 0; i < e->num_rows; i++) undo_row_free(&e->rows[i]);
  free(e->rows);
  free(e->not_restorable);
  memset(e, 0, sizeof *e);
}

void undo_stack_clear(UndoStack *s) {
  for (size_t i = 0; i < s->n; i++) undo_entry_free(&s->items[i]);
  free(s->items);
  s->items = NULL;
  s->n = 0;
}

UndoEntry *undo_stack_top(UndoStack *s) { return s->n ? &s->items[s->n - 1] : NULL; }

void undo_stack_pop(UndoStack *s) {
  if (!s->n) return;
  undo_entry_free(&s->items[--s->n]);
}

/* Room for one more at the end: the oldest step goes when the stack is full */
static UndoEntry *push(UndoStack *s) {
  if (s->n >= UNDO_MAX) {
    undo_entry_free(&s->items[0]);
    memmove(s->items, s->items + 1, (s->n - 1) * sizeof *s->items);
    s->n--;
  }
  s->items = safe_realloc(s->items, (s->n + 1) * sizeof *s->items);
  UndoEntry *e = &s->items[s->n++];
  memset(e, 0, sizeof *e);
  return e;
}

void undo_stack_push_cell(UndoStack *s, const char *table, const LaceRowKey *key_after,
                          const char *column, const LaceValue *before, const LaceValue *after) {
  UndoEntry *e = push(s);
  e->kind = UNDO_CELL;
  e->table = str_dup(table);
  lace_row_key_copy(&e->key, key_after);
  e->column = str_dup(column);
  e->before = lace_value_copy(before);
  e->after = lace_value_copy(after);
}

static const LaceColumn *schema_column(const LaceSchema *s, const char *name) {
  for (size_t i = 0; s && i < s->num_columns; i++)
    if (s->columns[i].name && !strcmp(s->columns[i].name, name)) return &s->columns[i];
  return NULL;
}

bool undo_row_capture(const LaceResult *data, size_t row, const LaceSchema *schema, UndoRow *out,
                      bool *omitted) {
  memset(out, 0, sizeof *out);
  if (!data || !data->rows || row >= data->num_rows) return false;
  const LaceRow *r = &data->rows[row];
  out->names = safe_calloc(data->num_columns + 1, sizeof *out->names);
  out->values = safe_calloc(data->num_columns + 1, sizeof *out->values);
  for (size_t c = 0; c < data->num_columns && c < r->num_cells; c++) {
    const char *name = data->columns[c].name;
    const LaceColumn *sc = name ? schema_column(schema, name) : NULL;
    if (!sc || sc->generated) continue; /* not a table column, or the database fills it */
    if (r->cells[c].omitted) *omitted = true;
    out->names[out->count] = str_dup(name);
    out->values[out->count++] = lace_value_copy(&r->cells[c]);
  }
  return true;
}

void undo_stack_push_delete(UndoStack *s, const char *table, UndoRow *rows, size_t n,
                            char *not_restorable) {
  UndoEntry *e = push(s);
  e->kind = UNDO_DELETE;
  e->table = str_dup(table);
  e->rows = rows;
  e->num_rows = n;
  e->not_restorable = not_restorable;
}

void undo_stack_merge_last_deletes(UndoStack *s, size_t n) {
  if (n < 2 || n > s->n) return;
  UndoEntry *first = &s->items[s->n - n];
  size_t total = 0;
  for (size_t i = 0; i < n; i++) {
    const UndoEntry *e = &first[i];
    if (e->kind != UNDO_DELETE || !e->table || strcmp(e->table, first->table) != 0) return;
    total += e->num_rows;
  }
  UndoRow *rows = safe_calloc(total + 1, sizeof *rows);
  size_t k = 0;
  char *not_restorable = NULL;
  for (size_t i = 0; i < n; i++) {
    UndoEntry *e = &first[i];
    for (size_t j = 0; j < e->num_rows; j++) rows[k++] = e->rows[j];
    free(e->rows); /* the rows moved */
    e->rows = NULL;
    e->num_rows = 0;
    if (e->not_restorable && !not_restorable) {
      not_restorable = e->not_restorable;
      e->not_restorable = NULL;
    }
  }
  char *table = str_dup(first->table);
  for (size_t i = 0; i < n; i++) undo_entry_free(&first[i]);
  s->n -= n;
  UndoEntry *e = push(s);
  e->kind = UNDO_DELETE;
  e->table = table;
  e->rows = rows;
  e->num_rows = total;
  e->not_restorable = not_restorable;
}
