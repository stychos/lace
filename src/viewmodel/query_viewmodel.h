/*
 * Lace
 * QueryViewModel - SQL query editor view model
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_QUERY_VIEWMODEL_H
#define LACE_QUERY_VIEWMODEL_H

#include "../core/app_state.h"
#include "../db/db_types.h"
#include "viewmodel.h"
#include "table_viewmodel.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct QueryViewModel QueryViewModel;

#define QUERY_VM_CHANGE_TEXT      (1u << 8)
#define QUERY_VM_CHANGE_RESULTS   (1u << 9)
#define QUERY_VM_CHANGE_EXECUTING (1u << 10)
#define QUERY_VM_CHANGE_ERROR     (1u << 11)

typedef enum {
  QUERY_EXEC_IDLE,
  QUERY_EXEC_RUNNING,
  QUERY_EXEC_CANCELLED,
  QUERY_EXEC_COMPLETE,
} QueryExecState;

typedef enum {
  QUERY_FOCUS_EDITOR,
  QUERY_FOCUS_RESULTS,
} QueryFocusPanel;

typedef struct QuerySelection {
  size_t start, end;
  bool active;
  size_t anchor;
} QuerySelection;

typedef struct QueryViewModelCallbacks {
  void (*on_exec_complete)(QueryViewModel *vm, bool success, void *ctx);
  void (*on_text_change)(QueryViewModel *vm, void *ctx);
  void *context;
} QueryViewModelCallbacks;

struct QueryViewModel {
  ViewModel base;
  Tab *tab;
  AppState *app;
  QueryViewModelCallbacks query_callbacks;
  size_t cursor_offset;
  QuerySelection selection;
  QueryFocusPanel focus_panel;
  QueryExecState exec_state;
  char *error_msg;
  TableViewModel *results_widget;
};

/* Lifecycle */
QueryViewModel *query_vm_create(AppState *app, Tab *tab);
void query_vm_destroy(QueryViewModel *vm);
void query_vm_bind(QueryViewModel *vm, Tab *tab);
void query_vm_set_callbacks(QueryViewModel *vm, const QueryViewModelCallbacks *callbacks);

/* Text Access */
const char *query_vm_get_text(const QueryViewModel *vm);
size_t query_vm_get_length(const QueryViewModel *vm);
void query_vm_set_text(QueryViewModel *vm, const char *text);
void query_vm_insert_char(QueryViewModel *vm, char ch);
void query_vm_insert_text(QueryViewModel *vm, const char *text);
void query_vm_delete_char(QueryViewModel *vm);
void query_vm_backspace(QueryViewModel *vm);
void query_vm_delete_selection(QueryViewModel *vm);
void query_vm_delete_line(QueryViewModel *vm);

/* Cursor */
size_t query_vm_get_cursor(const QueryViewModel *vm);
void query_vm_set_cursor(QueryViewModel *vm, size_t offset);
void query_vm_move_left(QueryViewModel *vm);
void query_vm_move_right(QueryViewModel *vm);
void query_vm_move_up(QueryViewModel *vm);
void query_vm_move_down(QueryViewModel *vm);
void query_vm_move_word_left(QueryViewModel *vm);
void query_vm_move_word_right(QueryViewModel *vm);
void query_vm_home(QueryViewModel *vm);
void query_vm_end(QueryViewModel *vm);
void query_vm_doc_start(QueryViewModel *vm);
void query_vm_doc_end(QueryViewModel *vm);
void query_vm_get_cursor_pos(const QueryViewModel *vm, size_t *line, size_t *col);

/* Selection */
bool query_vm_has_selection(const QueryViewModel *vm);
void query_vm_get_selection(const QueryViewModel *vm, size_t *start, size_t *end);
char *query_vm_get_selected_text(const QueryViewModel *vm);
void query_vm_set_selection(QueryViewModel *vm, size_t start, size_t end);
void query_vm_select_all(QueryViewModel *vm);
void query_vm_clear_selection(QueryViewModel *vm);
void query_vm_extend_selection_to(QueryViewModel *vm, size_t pos);

/* Line Information */
size_t query_vm_line_count(const QueryViewModel *vm);
const char *query_vm_line_at(const QueryViewModel *vm, size_t line, size_t *length);
size_t query_vm_line_offset(const QueryViewModel *vm, size_t line);

/* Focus Panel */
QueryFocusPanel query_vm_get_focus_panel(const QueryViewModel *vm);
void query_vm_set_focus_panel(QueryViewModel *vm, QueryFocusPanel panel);
void query_vm_toggle_focus_panel(QueryViewModel *vm);
bool query_vm_editor_focused(const QueryViewModel *vm);
bool query_vm_results_focused(const QueryViewModel *vm);

/* Execution */
void query_vm_execute(QueryViewModel *vm);
void query_vm_execute_selected(QueryViewModel *vm);
void query_vm_cancel(QueryViewModel *vm);
QueryExecState query_vm_exec_state(const QueryViewModel *vm);
bool query_vm_is_executing(const QueryViewModel *vm);

/* Results */
bool query_vm_has_results(const QueryViewModel *vm);
const ResultSet *query_vm_get_results(const QueryViewModel *vm);
TableViewModel *query_vm_get_results_widget(QueryViewModel *vm);
int64_t query_vm_affected_rows(const QueryViewModel *vm);
const char *query_vm_get_error(const QueryViewModel *vm);
void query_vm_clear_results(QueryViewModel *vm);

/* Clipboard */
char *query_vm_copy(const QueryViewModel *vm);
char *query_vm_cut(QueryViewModel *vm);
void query_vm_paste(QueryViewModel *vm, const char *text);

/* State */
bool query_vm_valid(const QueryViewModel *vm);
DbConnection *query_vm_connection(const QueryViewModel *vm);

const ViewModelOps *query_vm_ops(void);

/* Backward Compatibility */
typedef QueryViewModel QueryWidget;
typedef QueryViewModelCallbacks QueryWidgetCallbacks;
#define QUERY_CHANGE_TEXT      QUERY_VM_CHANGE_TEXT
#define QUERY_CHANGE_RESULTS   QUERY_VM_CHANGE_RESULTS
#define QUERY_CHANGE_EXECUTING QUERY_VM_CHANGE_EXECUTING
#define QUERY_CHANGE_ERROR     QUERY_VM_CHANGE_ERROR
#define query_widget_create             query_vm_create
#define query_widget_destroy            query_vm_destroy
#define query_widget_bind               query_vm_bind
#define query_widget_set_callbacks      query_vm_set_callbacks
#define query_widget_get_text           query_vm_get_text
#define query_widget_get_length         query_vm_get_length
#define query_widget_set_text           query_vm_set_text
#define query_widget_insert_char        query_vm_insert_char
#define query_widget_insert_text        query_vm_insert_text
#define query_widget_delete_char        query_vm_delete_char
#define query_widget_backspace          query_vm_backspace
#define query_widget_delete_selection   query_vm_delete_selection
#define query_widget_delete_line        query_vm_delete_line
#define query_widget_get_cursor         query_vm_get_cursor
#define query_widget_set_cursor         query_vm_set_cursor
#define query_widget_move_left          query_vm_move_left
#define query_widget_move_right         query_vm_move_right
#define query_widget_move_up            query_vm_move_up
#define query_widget_move_down          query_vm_move_down
#define query_widget_move_word_left     query_vm_move_word_left
#define query_widget_move_word_right    query_vm_move_word_right
#define query_widget_home               query_vm_home
#define query_widget_end                query_vm_end
#define query_widget_doc_start          query_vm_doc_start
#define query_widget_doc_end            query_vm_doc_end
#define query_widget_get_cursor_pos     query_vm_get_cursor_pos
#define query_widget_has_selection      query_vm_has_selection
#define query_widget_get_selection      query_vm_get_selection
#define query_widget_get_selected_text  query_vm_get_selected_text
#define query_widget_set_selection      query_vm_set_selection
#define query_widget_select_all         query_vm_select_all
#define query_widget_clear_selection    query_vm_clear_selection
#define query_widget_extend_selection_to query_vm_extend_selection_to
#define query_widget_line_count         query_vm_line_count
#define query_widget_line_at            query_vm_line_at
#define query_widget_line_offset        query_vm_line_offset
#define query_widget_get_focus_panel    query_vm_get_focus_panel
#define query_widget_set_focus_panel    query_vm_set_focus_panel
#define query_widget_toggle_focus_panel query_vm_toggle_focus_panel
#define query_widget_editor_focused     query_vm_editor_focused
#define query_widget_results_focused    query_vm_results_focused
#define query_widget_execute            query_vm_execute
#define query_widget_execute_selected   query_vm_execute_selected
#define query_widget_cancel             query_vm_cancel
#define query_widget_exec_state         query_vm_exec_state
#define query_widget_is_executing       query_vm_is_executing
#define query_widget_has_results        query_vm_has_results
#define query_widget_get_results        query_vm_get_results
#define query_widget_get_results_widget query_vm_get_results_widget
#define query_widget_affected_rows      query_vm_affected_rows
#define query_widget_get_error          query_vm_get_error
#define query_widget_clear_results      query_vm_clear_results
#define query_widget_copy               query_vm_copy
#define query_widget_cut                query_vm_cut
#define query_widget_paste              query_vm_paste
#define query_widget_valid              query_vm_valid
#define query_widget_connection         query_vm_connection
#define query_widget_ops                query_vm_ops

#endif /* LACE_QUERY_VIEWMODEL_H */
