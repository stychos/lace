/* The one write path: decide what an edit means, build the row key and the
 * verification snapshot, queue the verified UPDATE/DELETE and fold the answer
 * back into the page cache. Table tabs and query results share it. */
#ifndef NC_EDIT_OPS_H
#define NC_EDIT_OPS_H
#include "app.h"
#include "model/view.h"
#include "row_key.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  NcApp *app;
  NcConnection *conn;
  View *view;               /* view->cache, view->schema (or the source schema) */
  const char *table;
  bool unique_names;        /* query results: key columns must appear once */
  bool snapshot_all;        /* verify every loaded table column (query results) */
} EditContext;

/* false: the view is read-only; *why says so (a static string) */
bool edit_context_for_view(NcApp *app, View *v, EditContext *out, const char **why);

/* Decide and queue a verified UPDATE. text NULL with set_null = set NULL.
 * Returns false (with *why) when nothing is written. */
/* set_empty (the "set empty" actions only) writes '' even on a NULL cell.
 * gen is page_cache_generation() when the caller picked the row (the inline
 * edit, the editor or the confirmation started): a reload since then, or one
 * still landing, refuses the write, so it never reaches another row.
 * *why points to a static buffer (single thread: the main loop). */
bool edit_cell(EditContext *ctx, size_t row, size_t col, const char *text, bool set_null,
               bool set_empty, uint64_t gen, const char **why);
/* NULL when the cell can be edited now, else the reason (also what starting an
 * edit checks: still loading, not loaded, too large, computed) */
const char *edit_check_cell(EditContext *ctx, size_t row, size_t col);
/* Confirms per config, verifies, deletes, reloads; gen as for edit_cell */
void edit_delete_rows(EditContext *ctx, const size_t *rows, size_t n, uint64_t gen);
bool edit_paste_cell(EditContext *ctx, size_t row, size_t col, const char *text, uint64_t gen,
                     const char **why);

typedef enum { EDIT_NOTHING, EDIT_WRITE, EDIT_REFUSED } EditDecision;
/* What text + set_null mean for a cell whose value is orig. On EDIT_WRITE *out
 * is the value to write (caller frees); on EDIT_REFUSED *err is allocated. */
EditDecision edit_decide(const LaceValue *orig, LaceValueType col_type, const char *text,
                         bool set_null, bool set_empty, LaceValue *out, char **err);

/* Clipboard: the app's own copy, and OSC 52 to the terminal when enabled */
void app_clipboard_set(NcApp *app, const char *text);
const char *app_clipboard_get(const NcApp *app);
/* Base64 (exposed for the tests); allocated */
char *edit_base64(const unsigned char *data, size_t n);

/* ui/dialogs/cell_editor.c, add_row.c */
void cell_editor_open(NcApp *app, View *v, size_t row, size_t col);
void add_row_open(NcApp *app, View *v);
#endif
