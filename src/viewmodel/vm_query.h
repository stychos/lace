/*
 * Lace
 * Query ViewModel - Platform-independent SQL editor model
 *
 * Provides a clean interface for both TUI and GUI to access SQL editor state,
 * execution, and results.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_VM_QUERY_H
#define LACE_VM_QUERY_H

#include "../core/app_state.h"
#include "../db/db_types.h"
#include "vm_table.h"
#include <stdbool.h>
#include <stddef.h>

/* Forward declarations */
typedef struct VmQuery VmQuery;

/* ============================================================================
 * Change Flags
 * ============================================================================
 */

typedef enum {
  VM_QUERY_CHANGE_NONE = 0,
  VM_QUERY_CHANGE_TEXT = (1 << 0),      /* Query text changed */
  VM_QUERY_CHANGE_CURSOR = (1 << 1),    /* Cursor position changed */
  VM_QUERY_CHANGE_SELECTION = (1 << 2), /* Text selection changed */
  VM_QUERY_CHANGE_RESULTS = (1 << 3),   /* Query results changed */
  VM_QUERY_CHANGE_EXECUTING = (1 << 4), /* Execution state changed */
  VM_QUERY_CHANGE_ERROR = (1 << 5),     /* Error occurred */
  VM_QUERY_CHANGE_FOCUS = (1 << 6),     /* Focus changed (editor vs results) */
  VM_QUERY_CHANGE_ALL = 0xFF
} VmQueryChangeFlags;

/* ============================================================================
 * Execution State
 * ============================================================================
 */

typedef enum {
  VM_QUERY_IDLE,      /* Not executing */
  VM_QUERY_EXECUTING, /* Query running */
  VM_QUERY_CANCELLED, /* Execution cancelled */
  VM_QUERY_COMPLETE,  /* Execution complete (check results/error) */
} VmQueryExecState;

/* ============================================================================
 * Focus - Which panel has focus
 * ============================================================================
 */

typedef enum {
  VM_QUERY_FOCUS_EDITOR,  /* SQL editor has focus */
  VM_QUERY_FOCUS_RESULTS, /* Results grid has focus */
} VmQueryFocus;

/* ============================================================================
 * Callbacks
 * ============================================================================
 */

typedef struct {
  /* Called when query state changes */
  void (*on_change)(VmQuery *vm, VmQueryChangeFlags changes, void *ctx);

  /* Called when execution completes */
  void (*on_exec_complete)(VmQuery *vm, bool success, void *ctx);

  /* User context */
  void *context;
} VmQueryCallbacks;

/* ============================================================================
 * Text Selection
 * ============================================================================
 */

typedef struct {
  size_t start; /* Start position (inclusive) */
  size_t end;   /* End position (exclusive) */
  bool active;  /* Is there an active selection */
} VmTextSelection;

/* ============================================================================
 * VmQuery - Query ViewModel
 * ============================================================================
 */

struct VmQuery {
  /* Source (reference, not owned) */
  Tab *tab;
  AppState *app;

  /* Callbacks */
  VmQueryCallbacks callbacks;

  /* Text selection */
  VmTextSelection selection;

  /* Focus state */
  VmQueryFocus focus;

  /* Results viewmodel (lazily created) */
  VmTable *results_vm;

  /* Execution state */
  VmQueryExecState exec_state;
};

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create a query viewmodel bound to a query tab */
VmQuery *vm_query_create(AppState *app, Tab *tab,
                         const VmQueryCallbacks *callbacks);

/* Destroy viewmodel */
void vm_query_destroy(VmQuery *vm);

/* Rebind to different tab */
void vm_query_bind(VmQuery *vm, Tab *tab);

/* ============================================================================
 * Text Access (for native text controls)
 * ============================================================================
 */

/* Get query text */
const char *vm_query_get_text(const VmQuery *vm);
size_t vm_query_get_length(const VmQuery *vm);

/* Set query text (replaces all) */
void vm_query_set_text(VmQuery *vm, const char *text);

/* Text mutations */
void vm_query_insert_char(VmQuery *vm, char ch);
void vm_query_insert_text(VmQuery *vm, const char *text);
void vm_query_delete_char(VmQuery *vm);      /* Delete at cursor */
void vm_query_backspace(VmQuery *vm);        /* Delete before cursor */
void vm_query_delete_selection(VmQuery *vm); /* Delete selected text */
void vm_query_delete_line(VmQuery *vm);      /* Delete current line */
void vm_query_delete_to_end(VmQuery *vm);    /* Delete to end of line */

/* ============================================================================
 * Cursor
 * ============================================================================
 */

/* Get/set cursor position (character offset) */
size_t vm_query_get_cursor(const VmQuery *vm);
void vm_query_set_cursor(VmQuery *vm, size_t pos);

/* Move cursor */
void vm_query_move_cursor(VmQuery *vm, int delta);
void vm_query_move_up(VmQuery *vm);
void vm_query_move_down(VmQuery *vm);
void vm_query_move_left(VmQuery *vm);
void vm_query_move_right(VmQuery *vm);
void vm_query_move_word_left(VmQuery *vm);
void vm_query_move_word_right(VmQuery *vm);
void vm_query_home(VmQuery *vm);      /* Start of line */
void vm_query_end(VmQuery *vm);       /* End of line */
void vm_query_doc_start(VmQuery *vm); /* Start of document */
void vm_query_doc_end(VmQuery *vm);   /* End of document */

/* Get cursor position as line/column */
void vm_query_get_cursor_pos(const VmQuery *vm, size_t *line, size_t *col);
void vm_query_set_cursor_pos(VmQuery *vm, size_t line, size_t col);

/* ============================================================================
 * Scroll
 * ============================================================================
 */

/* Get/set scroll position (line, column) */
void vm_query_get_scroll(const VmQuery *vm, size_t *line, size_t *col);
void vm_query_set_scroll(VmQuery *vm, size_t line, size_t col);

/* Ensure cursor is visible */
void vm_query_ensure_cursor_visible(VmQuery *vm, size_t visible_lines,
                                    size_t visible_cols);

/* ============================================================================
 * Selection
 * ============================================================================
 */

/* Get selection */
bool vm_query_has_selection(const VmQuery *vm);
void vm_query_get_selection(const VmQuery *vm, size_t *start, size_t *end);
char *vm_query_get_selected_text(const VmQuery *vm);

/* Set selection */
void vm_query_set_selection(VmQuery *vm, size_t start, size_t end);
void vm_query_select_all(VmQuery *vm);
void vm_query_clear_selection(VmQuery *vm);

/* Extend selection (shift+movement) */
void vm_query_extend_selection_left(VmQuery *vm);
void vm_query_extend_selection_right(VmQuery *vm);
void vm_query_extend_selection_up(VmQuery *vm);
void vm_query_extend_selection_down(VmQuery *vm);
void vm_query_extend_selection_to(VmQuery *vm, size_t pos);

/* ============================================================================
 * Line Information
 * ============================================================================
 */

/* Get number of lines */
size_t vm_query_line_count(const VmQuery *vm);

/* Get line at index (returns pointer into buffer, not a copy) */
const char *vm_query_line_at(const VmQuery *vm, size_t line, size_t *length);

/* Get offset of line start */
size_t vm_query_line_offset(const VmQuery *vm, size_t line);

/* ============================================================================
 * Focus
 * ============================================================================
 */

/* Get/set focus */
VmQueryFocus vm_query_get_focus(const VmQuery *vm);
void vm_query_set_focus(VmQuery *vm, VmQueryFocus focus);
void vm_query_toggle_focus(VmQuery *vm);

/* ============================================================================
 * Execution
 * ============================================================================
 */

/* Execute query (async) */
void vm_query_execute(VmQuery *vm);
void vm_query_execute_selected(VmQuery *vm); /* Execute selected text only */

/* Cancel execution */
void vm_query_cancel(VmQuery *vm);

/* Get execution state */
VmQueryExecState vm_query_exec_state(const VmQuery *vm);
bool vm_query_is_executing(const VmQuery *vm);

/* ============================================================================
 * Results
 * ============================================================================
 */

/* Check if results are available */
bool vm_query_has_results(const VmQuery *vm);

/* Get results (NULL if none) */
const ResultSet *vm_query_get_results(const VmQuery *vm);

/* Get results as VmTable for unified grid handling */
VmTable *vm_query_get_results_vm(VmQuery *vm);

/* Get affected row count (for INSERT/UPDATE/DELETE) */
int64_t vm_query_affected_rows(const VmQuery *vm);

/* Get error message (NULL if no error) */
const char *vm_query_get_error(const VmQuery *vm);

/* Clear results */
void vm_query_clear_results(VmQuery *vm);

/* ============================================================================
 * History
 * ============================================================================
 */

/* Navigate query history */
void vm_query_history_prev(VmQuery *vm);
void vm_query_history_next(VmQuery *vm);

/* ============================================================================
 * Clipboard
 * ============================================================================
 */

/* Copy selected text (returns allocated string) */
char *vm_query_copy(const VmQuery *vm);

/* Cut selected text (returns allocated string, removes from buffer) */
char *vm_query_cut(VmQuery *vm);

/* Paste text at cursor */
void vm_query_paste(VmQuery *vm, const char *text);

/* ============================================================================
 * Utility
 * ============================================================================
 */

/* Get connection */
DbConnection *vm_query_connection(const VmQuery *vm);

/* Check if viewmodel is valid */
bool vm_query_valid(const VmQuery *vm);

#endif /* LACE_VM_QUERY_H */
