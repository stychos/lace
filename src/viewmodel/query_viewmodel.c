/*
 * Lace
 * QueryViewModel - SQL query editor view model
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "query_viewmodel.h"
#include "../util/mem.h"
#include "../util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool query_vm_handle_event(ViewModel *vm, const UiEvent *event);
static size_t query_vm_get_row_count(const ViewModel *vm);
static size_t query_vm_get_col_count(const ViewModel *vm);
static void query_vm_on_focus_in(ViewModel *vm);
static void query_vm_on_focus_out(ViewModel *vm);
static void query_vm_validate_cursor_impl(ViewModel *vm);
static void query_vm_ops_destroy(ViewModel *vm);

static const ViewModelOps s_query_vm_ops = {
    .type_name = "QueryViewModel",
    .handle_event = query_vm_handle_event,
    .get_row_count = query_vm_get_row_count,
    .get_col_count = query_vm_get_col_count,
    .on_focus_in = query_vm_on_focus_in,
    .on_focus_out = query_vm_on_focus_out,
    .validate_cursor = query_vm_validate_cursor_impl,
    .destroy = query_vm_ops_destroy,
};

const ViewModelOps *query_vm_ops(void) { return &s_query_vm_ops; }

static char *get_text(const QueryViewModel *vm) {
  if (!vm || !vm->tab) return NULL;
  return vm->tab->query_text;
}

static size_t get_length(const QueryViewModel *vm) {
  if (!vm || !vm->tab || !vm->tab->query_text) return 0;
  return vm->tab->query_len;
}

static size_t count_lines_before(const char *text, size_t pos) {
  size_t lines = 0;
  for (size_t i = 0; i < pos && text[i]; i++)
    if (text[i] == '\n') lines++;
  return lines;
}

static size_t find_line_start(const char *text, size_t pos) {
  if (!text || pos == 0) return 0;
  size_t i = pos;
  while (i > 0 && text[i - 1] != '\n') i--;
  return i;
}

static size_t find_line_end(const char *text, size_t len, size_t pos) {
  if (!text) return 0;
  size_t i = pos;
  while (i < len && text[i] != '\n') i++;
  return i;
}

static size_t find_line_offset(const char *text, size_t len, size_t line_num) {
  if (!text) return 0;
  size_t current_line = 0, offset = 0;
  while (offset < len && current_line < line_num) {
    if (text[offset] == '\n') current_line++;
    offset++;
  }
  return offset;
}

static bool query_vm_handle_event(ViewModel *vm, const UiEvent *event) {
  (void)vm; (void)event;
  return false;
}

static size_t query_vm_get_row_count(const ViewModel *vm) {
  return query_vm_line_count((const QueryViewModel *)vm);
}

static size_t query_vm_get_col_count(const ViewModel *vm) {
  (void)vm;
  return 1;
}

static void query_vm_on_focus_in(ViewModel *vm) { (void)vm; }
static void query_vm_on_focus_out(ViewModel *vm) { (void)vm; }

static void query_vm_validate_cursor_impl(ViewModel *vm) {
  QueryViewModel *qvm = (QueryViewModel *)vm;
  size_t len = get_length(qvm);
  if (qvm->cursor_offset > len) qvm->cursor_offset = len;
}

static void query_vm_ops_destroy(ViewModel *vm) {
  QueryViewModel *qvm = (QueryViewModel *)vm;
  if (qvm->results_widget) {
    table_vm_destroy(qvm->results_widget);
    qvm->results_widget = NULL;
  }
  free(qvm->error_msg);
  qvm->error_msg = NULL;
  qvm->selection.active = false;
  memset(&qvm->query_callbacks, 0, sizeof(qvm->query_callbacks));
  qvm->tab = NULL;
  qvm->app = NULL;
}

QueryViewModel *query_vm_create(AppState *app, Tab *tab) {
  QueryViewModel *vm = safe_calloc(1, sizeof(QueryViewModel));
  vm_init(&vm->base, &s_query_vm_ops);
  vm->app = app;
  vm->focus_panel = QUERY_FOCUS_EDITOR;
  vm->exec_state = QUERY_EXEC_IDLE;
  query_vm_bind(vm, tab);
  return vm;
}

void query_vm_destroy(QueryViewModel *vm) {
  if (!vm) return;
  vm_cleanup(&vm->base);
  free(vm);
}

void query_vm_bind(QueryViewModel *vm, Tab *tab) {
  if (!vm) return;
  vm->tab = tab;
  vm->cursor_offset = 0;
  vm->selection.active = false;
  if (vm->results_widget) {
    table_vm_destroy(vm->results_widget);
    vm->results_widget = NULL;
  }
  free(vm->error_msg);
  vm->error_msg = NULL;
  vm->exec_state = QUERY_EXEC_IDLE;
  vm_notify(&vm->base, VM_CHANGE_DATA);
}

void query_vm_set_callbacks(QueryViewModel *vm, const QueryViewModelCallbacks *callbacks) {
  if (!vm) return;
  if (callbacks) vm->query_callbacks = *callbacks;
  else memset(&vm->query_callbacks, 0, sizeof(vm->query_callbacks));
}

const char *query_vm_get_text(const QueryViewModel *vm) { return get_text(vm); }
size_t query_vm_get_length(const QueryViewModel *vm) { return get_length(vm); }

void query_vm_set_text(QueryViewModel *vm, const char *text) {
  if (!vm || !vm->tab) return;
  size_t len = text ? strlen(text) : 0;
  if (len + 1 > vm->tab->query_capacity) {
    size_t new_cap = len + 256;
    vm->tab->query_text = safe_realloc(vm->tab->query_text, new_cap);
    vm->tab->query_capacity = new_cap;
  }
  if (text) memcpy(vm->tab->query_text, text, len);
  vm->tab->query_text[len] = '\0';
  vm->tab->query_len = len;
  vm->cursor_offset = 0;
  vm->selection.active = false;
  vm_notify(&vm->base, QUERY_VM_CHANGE_TEXT);
  if (vm->query_callbacks.on_text_change)
    vm->query_callbacks.on_text_change(vm, vm->query_callbacks.context);
}

void query_vm_insert_char(QueryViewModel *vm, char ch) {
  if (!vm || !vm->tab) return;
  if (vm->selection.active) query_vm_delete_selection(vm);
  size_t len = vm->tab->query_len;
  size_t pos = vm->cursor_offset;
  if (len + 2 > vm->tab->query_capacity) {
    size_t new_cap = vm->tab->query_capacity * 2;
    if (new_cap < 256) new_cap = 256;
    vm->tab->query_text = safe_realloc(vm->tab->query_text, new_cap);
    vm->tab->query_capacity = new_cap;
  }
  memmove(&vm->tab->query_text[pos + 1], &vm->tab->query_text[pos], len - pos + 1);
  vm->tab->query_text[pos] = ch;
  vm->tab->query_len++;
  vm->cursor_offset++;
  vm_notify(&vm->base, QUERY_VM_CHANGE_TEXT | VM_CHANGE_CURSOR);
  if (vm->query_callbacks.on_text_change)
    vm->query_callbacks.on_text_change(vm, vm->query_callbacks.context);
}

void query_vm_insert_text(QueryViewModel *vm, const char *text) {
  if (!vm || !text) return;
  for (const char *p = text; *p; p++) query_vm_insert_char(vm, *p);
}

void query_vm_delete_char(QueryViewModel *vm) {
  if (!vm || !vm->tab || !vm->tab->query_text) return;
  size_t len = vm->tab->query_len;
  size_t pos = vm->cursor_offset;
  if (pos >= len) return;
  memmove(&vm->tab->query_text[pos], &vm->tab->query_text[pos + 1], len - pos);
  vm->tab->query_len--;
  vm_notify(&vm->base, QUERY_VM_CHANGE_TEXT);
  if (vm->query_callbacks.on_text_change)
    vm->query_callbacks.on_text_change(vm, vm->query_callbacks.context);
}

void query_vm_backspace(QueryViewModel *vm) {
  if (!vm) return;
  if (vm->selection.active) { query_vm_delete_selection(vm); return; }
  if (vm->cursor_offset == 0) return;
  vm->cursor_offset--;
  query_vm_delete_char(vm);
}

void query_vm_delete_selection(QueryViewModel *vm) {
  if (!vm || !vm->selection.active || !vm->tab || !vm->tab->query_text) return;
  size_t start = vm->selection.start;
  size_t end = vm->selection.end;
  if (start > end) { size_t tmp = start; start = end; end = tmp; }
  size_t len = vm->tab->query_len;
  if (end > len) end = len;
  size_t delete_len = end - start;
  memmove(&vm->tab->query_text[start], &vm->tab->query_text[end], len - end + 1);
  vm->tab->query_len -= delete_len;
  vm->cursor_offset = start;
  vm->selection.active = false;
  vm_notify(&vm->base, QUERY_VM_CHANGE_TEXT | VM_CHANGE_CURSOR);
  if (vm->query_callbacks.on_text_change)
    vm->query_callbacks.on_text_change(vm, vm->query_callbacks.context);
}

void query_vm_delete_line(QueryViewModel *vm) {
  if (!vm || !vm->tab || !vm->tab->query_text) return;
  char *text = vm->tab->query_text;
  size_t len = vm->tab->query_len;
  size_t pos = vm->cursor_offset;
  size_t line_start = find_line_start(text, pos);
  size_t line_end = find_line_end(text, len, pos);
  if (line_end < len && text[line_end] == '\n') line_end++;
  size_t delete_len = line_end - line_start;
  memmove(&text[line_start], &text[line_end], len - line_end + 1);
  vm->tab->query_len -= delete_len;
  vm->cursor_offset = line_start;
  vm_notify(&vm->base, QUERY_VM_CHANGE_TEXT | VM_CHANGE_CURSOR);
  if (vm->query_callbacks.on_text_change)
    vm->query_callbacks.on_text_change(vm, vm->query_callbacks.context);
}

size_t query_vm_get_cursor(const QueryViewModel *vm) {
  return vm ? vm->cursor_offset : 0;
}

void query_vm_set_cursor(QueryViewModel *vm, size_t offset) {
  if (!vm) return;
  size_t len = get_length(vm);
  if (offset > len) offset = len;
  if (vm->cursor_offset != offset) {
    vm->cursor_offset = offset;
    vm->selection.active = false;
    vm_notify(&vm->base, VM_CHANGE_CURSOR);
  }
}

void query_vm_move_left(QueryViewModel *vm) {
  if (!vm || vm->cursor_offset == 0) return;
  vm->cursor_offset--;
  vm->selection.active = false;
  vm_notify(&vm->base, VM_CHANGE_CURSOR);
}

void query_vm_move_right(QueryViewModel *vm) {
  if (!vm) return;
  size_t len = get_length(vm);
  if (vm->cursor_offset < len) {
    vm->cursor_offset++;
    vm->selection.active = false;
    vm_notify(&vm->base, VM_CHANGE_CURSOR);
  }
}

void query_vm_move_up(QueryViewModel *vm) {
  if (!vm) return;
  char *text = get_text(vm);
  if (!text) return;
  size_t line_start = find_line_start(text, vm->cursor_offset);
  if (line_start == 0) return;
  size_t col = vm->cursor_offset - line_start;
  size_t prev_line_start = find_line_start(text, line_start - 1);
  size_t prev_line_len = line_start - 1 - prev_line_start;
  if (col > prev_line_len) col = prev_line_len;
  vm->cursor_offset = prev_line_start + col;
  vm->selection.active = false;
  vm_notify(&vm->base, VM_CHANGE_CURSOR);
}

void query_vm_move_down(QueryViewModel *vm) {
  if (!vm) return;
  char *text = get_text(vm);
  size_t len = get_length(vm);
  if (!text) return;
  size_t line_start = find_line_start(text, vm->cursor_offset);
  size_t line_end = find_line_end(text, len, vm->cursor_offset);
  if (line_end >= len) return;
  size_t col = vm->cursor_offset - line_start;
  size_t next_line_start = line_end + 1;
  size_t next_line_end = find_line_end(text, len, next_line_start);
  size_t next_line_len = next_line_end - next_line_start;
  if (col > next_line_len) col = next_line_len;
  vm->cursor_offset = next_line_start + col;
  vm->selection.active = false;
  vm_notify(&vm->base, VM_CHANGE_CURSOR);
}

void query_vm_move_word_left(QueryViewModel *vm) {
  if (!vm || vm->cursor_offset == 0) return;
  char *text = get_text(vm);
  if (!text) return;
  size_t pos = vm->cursor_offset;
  while (pos > 0 && isspace((unsigned char)text[pos - 1])) pos--;
  while (pos > 0 && !isspace((unsigned char)text[pos - 1])) pos--;
  vm->cursor_offset = pos;
  vm->selection.active = false;
  vm_notify(&vm->base, VM_CHANGE_CURSOR);
}

void query_vm_move_word_right(QueryViewModel *vm) {
  if (!vm) return;
  char *text = get_text(vm);
  size_t len = get_length(vm);
  if (!text) return;
  size_t pos = vm->cursor_offset;
  while (pos < len && !isspace((unsigned char)text[pos])) pos++;
  while (pos < len && isspace((unsigned char)text[pos])) pos++;
  vm->cursor_offset = pos;
  vm->selection.active = false;
  vm_notify(&vm->base, VM_CHANGE_CURSOR);
}

void query_vm_home(QueryViewModel *vm) {
  if (!vm) return;
  char *text = get_text(vm);
  if (!text) return;
  vm->cursor_offset = find_line_start(text, vm->cursor_offset);
  vm->selection.active = false;
  vm_notify(&vm->base, VM_CHANGE_CURSOR);
}

void query_vm_end(QueryViewModel *vm) {
  if (!vm) return;
  char *text = get_text(vm);
  size_t len = get_length(vm);
  if (!text) return;
  vm->cursor_offset = find_line_end(text, len, vm->cursor_offset);
  vm->selection.active = false;
  vm_notify(&vm->base, VM_CHANGE_CURSOR);
}

void query_vm_doc_start(QueryViewModel *vm) {
  if (!vm) return;
  vm->cursor_offset = 0;
  vm->selection.active = false;
  vm_notify(&vm->base, VM_CHANGE_CURSOR);
}

void query_vm_doc_end(QueryViewModel *vm) {
  if (!vm) return;
  vm->cursor_offset = get_length(vm);
  vm->selection.active = false;
  vm_notify(&vm->base, VM_CHANGE_CURSOR);
}

void query_vm_get_cursor_pos(const QueryViewModel *vm, size_t *line, size_t *col) {
  if (!vm) { if (line) *line = 0; if (col) *col = 0; return; }
  char *text = get_text(vm);
  if (!text) { if (line) *line = 0; if (col) *col = 0; return; }
  if (line) *line = count_lines_before(text, vm->cursor_offset);
  if (col) {
    size_t line_start = find_line_start(text, vm->cursor_offset);
    *col = vm->cursor_offset - line_start;
  }
}

bool query_vm_has_selection(const QueryViewModel *vm) {
  return vm && vm->selection.active && vm->selection.start != vm->selection.end;
}

void query_vm_get_selection(const QueryViewModel *vm, size_t *start, size_t *end) {
  if (!vm || !vm->selection.active) { if (start) *start = 0; if (end) *end = 0; return; }
  if (start) *start = vm->selection.start;
  if (end) *end = vm->selection.end;
}

char *query_vm_get_selected_text(const QueryViewModel *vm) {
  if (!query_vm_has_selection(vm)) return NULL;
  char *text = get_text(vm);
  if (!text) return NULL;
  size_t start = vm->selection.start, end = vm->selection.end;
  if (start > end) { size_t tmp = start; start = end; end = tmp; }
  size_t len = end - start;
  char *result = safe_malloc(len + 1);
  memcpy(result, &text[start], len);
  result[len] = '\0';
  return result;
}

void query_vm_set_selection(QueryViewModel *vm, size_t start, size_t end) {
  if (!vm) return;
  size_t len = get_length(vm);
  if (start > len) start = len;
  if (end > len) end = len;
  vm->selection.start = start;
  vm->selection.end = end;
  vm->selection.active = (start != end);
  vm->selection.anchor = start;
  vm_notify(&vm->base, VM_CHANGE_SELECTION);
}

void query_vm_select_all(QueryViewModel *vm) {
  if (!vm) return;
  query_vm_set_selection(vm, 0, get_length(vm));
}

void query_vm_clear_selection(QueryViewModel *vm) {
  if (!vm) return;
  if (vm->selection.active) {
    vm->selection.active = false;
    vm_notify(&vm->base, VM_CHANGE_SELECTION);
  }
}

void query_vm_extend_selection_to(QueryViewModel *vm, size_t pos) {
  if (!vm) return;
  size_t len = get_length(vm);
  if (pos > len) pos = len;
  if (!vm->selection.active) {
    vm->selection.anchor = vm->cursor_offset;
    vm->selection.active = true;
  }
  if (pos < vm->selection.anchor) {
    vm->selection.start = pos;
    vm->selection.end = vm->selection.anchor;
  } else {
    vm->selection.start = vm->selection.anchor;
    vm->selection.end = pos;
  }
  vm->cursor_offset = pos;
  vm_notify(&vm->base, VM_CHANGE_SELECTION | VM_CHANGE_CURSOR);
}

size_t query_vm_line_count(const QueryViewModel *vm) {
  if (!vm) return 0;
  char *text = get_text(vm);
  size_t len = get_length(vm);
  if (!text || len == 0) return 1;
  size_t lines = 1;
  for (size_t i = 0; i < len; i++) if (text[i] == '\n') lines++;
  return lines;
}

const char *query_vm_line_at(const QueryViewModel *vm, size_t line, size_t *length) {
  if (!vm) { if (length) *length = 0; return NULL; }
  char *text = get_text(vm);
  size_t len = get_length(vm);
  if (!text) { if (length) *length = 0; return NULL; }
  size_t offset = find_line_offset(text, len, line);
  size_t line_end = find_line_end(text, len, offset);
  if (length) *length = line_end - offset;
  return &text[offset];
}

size_t query_vm_line_offset(const QueryViewModel *vm, size_t line) {
  if (!vm) return 0;
  char *text = get_text(vm);
  size_t len = get_length(vm);
  return find_line_offset(text, len, line);
}

QueryFocusPanel query_vm_get_focus_panel(const QueryViewModel *vm) {
  return vm ? vm->focus_panel : QUERY_FOCUS_EDITOR;
}

void query_vm_set_focus_panel(QueryViewModel *vm, QueryFocusPanel panel) {
  if (!vm || vm->focus_panel == panel) return;
  vm->focus_panel = panel;
  vm_notify(&vm->base, VM_CHANGE_FOCUS);
}

void query_vm_toggle_focus_panel(QueryViewModel *vm) {
  if (!vm) return;
  QueryFocusPanel new_panel = (vm->focus_panel == QUERY_FOCUS_EDITOR) ? QUERY_FOCUS_RESULTS : QUERY_FOCUS_EDITOR;
  query_vm_set_focus_panel(vm, new_panel);
}

bool query_vm_editor_focused(const QueryViewModel *vm) {
  return vm && vm->focus_panel == QUERY_FOCUS_EDITOR;
}

bool query_vm_results_focused(const QueryViewModel *vm) {
  return vm && vm->focus_panel == QUERY_FOCUS_RESULTS;
}

void query_vm_execute(QueryViewModel *vm) {
  if (!vm) return;
  vm->exec_state = QUERY_EXEC_RUNNING;
  vm_notify(&vm->base, QUERY_VM_CHANGE_EXECUTING);
}

void query_vm_execute_selected(QueryViewModel *vm) {
  if (!vm) return;
  vm->exec_state = QUERY_EXEC_RUNNING;
  vm_notify(&vm->base, QUERY_VM_CHANGE_EXECUTING);
}

void query_vm_cancel(QueryViewModel *vm) {
  if (!vm || vm->exec_state != QUERY_EXEC_RUNNING) return;
  vm->exec_state = QUERY_EXEC_CANCELLED;
  vm_notify(&vm->base, QUERY_VM_CHANGE_EXECUTING);
}

QueryExecState query_vm_exec_state(const QueryViewModel *vm) {
  return vm ? vm->exec_state : QUERY_EXEC_IDLE;
}

bool query_vm_is_executing(const QueryViewModel *vm) {
  return vm && vm->exec_state == QUERY_EXEC_RUNNING;
}

bool query_vm_has_results(const QueryViewModel *vm) {
  return vm && vm->tab && vm->tab->query_results && vm->tab->query_results->num_rows > 0;
}

const ResultSet *query_vm_get_results(const QueryViewModel *vm) {
  if (!vm || !vm->tab) return NULL;
  return vm->tab->query_results;
}

TableViewModel *query_vm_get_results_widget(QueryViewModel *vm) {
  if (!vm) return NULL;
  if (!vm->results_widget && vm->tab && vm->tab->query_results) {
    vm->results_widget = table_vm_create(vm->app, NULL);
    if (vm->results_widget) {
      vm->results_widget->data = vm->tab->query_results;
      table_vm_recalc_column_widths(vm->results_widget);
    }
  }
  return vm->results_widget;
}

int64_t query_vm_affected_rows(const QueryViewModel *vm) {
  if (!vm || !vm->tab || !vm->tab->query_results) return 0;
  return vm->tab->query_results->rows_affected;
}

const char *query_vm_get_error(const QueryViewModel *vm) {
  if (!vm) return NULL;
  if (vm->error_msg) return vm->error_msg;
  if (vm->tab && vm->tab->query_results && vm->tab->query_results->error)
    return vm->tab->query_results->error;
  return NULL;
}

void query_vm_clear_results(QueryViewModel *vm) {
  if (!vm) return;
  if (vm->results_widget) {
    table_vm_destroy(vm->results_widget);
    vm->results_widget = NULL;
  }
  free(vm->error_msg);
  vm->error_msg = NULL;
  vm->exec_state = QUERY_EXEC_IDLE;
  vm_notify(&vm->base, QUERY_VM_CHANGE_RESULTS);
}

char *query_vm_copy(const QueryViewModel *vm) {
  return query_vm_get_selected_text(vm);
}

char *query_vm_cut(QueryViewModel *vm) {
  char *text = query_vm_get_selected_text(vm);
  if (text) query_vm_delete_selection(vm);
  return text;
}

void query_vm_paste(QueryViewModel *vm, const char *text) {
  if (!vm || !text) return;
  if (vm->selection.active) query_vm_delete_selection(vm);
  query_vm_insert_text(vm, text);
}

bool query_vm_valid(const QueryViewModel *vm) { return vm && vm->tab; }

DbConnection *query_vm_connection(const QueryViewModel *vm) {
  if (!vm || !vm->app || !vm->tab) return NULL;
  size_t conn_idx = vm->tab->connection_index;
  if (conn_idx >= vm->app->num_connections) return NULL;
  Connection *conn = &vm->app->connections[conn_idx];
  return conn->active ? conn->conn : NULL;
}
