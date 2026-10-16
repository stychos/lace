/* The undo stack of a view (a table tab, a query tab's results): the cell
 * edits and row deletes that succeeded, newest last, each reverted by one
 * `undo`. Runtime only: it lives with the view and is gone at quit. Shared by
 * the TUIs; what reverting a step does (a verified UPDATE back, INSERTs of
 * the deleted rows) is each frontend's. */
#ifndef CORE_UNDO_STACK_H
#define CORE_UNDO_STACK_H
#include "row_key.h"
#include "types.h"
#include <stdbool.h>
#include <stddef.h>

#define UNDO_MAX 200 /* steps kept per view; the oldest go first */

typedef enum { UNDO_CELL, UNDO_DELETE } UndoKind;

typedef struct {
  char **names; /* the table columns of the row (generated ones left out) */
  LaceValue *values;
  size_t count;
} UndoRow;

typedef struct {
  UndoKind kind;
  char *table;
  /* UNDO_CELL: the row's key as it is after the edit, the column, the value
   * before and the value written (what the verification expects to find) */
  LaceRowKey key;
  char *column;
  LaceValue before, after;
  /* UNDO_DELETE: the rows, as they were loaded */
  UndoRow *rows;
  size_t num_rows;
  char *not_restorable; /* why the rows cannot come back (a value too large to load), or NULL */
} UndoEntry;

typedef struct {
  UndoEntry *items;
  size_t n;
} UndoStack;

/* Record a successful cell write (copies what it is given) */
void undo_stack_push_cell(UndoStack *s, const char *table, const LaceRowKey *key_after,
                          const char *column, const LaceValue *before, const LaceValue *after);
/* Capture row `row` of data for a delete, before the DELETE runs: the
 * columns of schema that data holds, generated ones left out. false when the
 * row is not there; *omitted is set when a value was too large to load (the
 * row cannot come back whole). */
bool undo_row_capture(const LaceResult *data, size_t row, const LaceSchema *schema, UndoRow *out,
                      bool *omitted);
void undo_row_free(UndoRow *r);
/* Record deleted rows as one step (takes the array and its rows; not_restorable
 * is taken too, or NULL) */
void undo_stack_push_delete(UndoStack *s, const char *table, UndoRow *rows, size_t n,
                            char *not_restorable);
/* The last n steps, when they are deletes of one table, become one step
 * (a bulk delete recorded row by row) */
void undo_stack_merge_last_deletes(UndoStack *s, size_t n);

UndoEntry *undo_stack_top(UndoStack *s); /* NULL when empty */
void undo_stack_pop(UndoStack *s);       /* frees the top */
void undo_entry_free(UndoEntry *e);
void undo_stack_clear(UndoStack *s);
#endif
