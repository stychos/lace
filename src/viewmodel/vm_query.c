/*
 * Lace
 * Query ViewModel - Implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "vm_query.h"
#include "../db/db.h"
#include "../util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

static void notify_change(VmQuery *vm, VmQueryChangeFlags flags) {
  if (vm->callbacks.on_change) {
    vm->callbacks.on_change(vm, flags, vm->callbacks.context);
  }
}

/* Ensure query buffer has capacity */
static bool ensure_capacity(Tab *tab, size_t needed) {
  if (needed <= tab->query_capacity)
    return true;

  size_t new_cap = tab->query_capacity ? tab->query_capacity * 2 : 1024;
  while (new_cap < needed)
    new_cap *= 2;

  char *new_buf = realloc(tab->query_text, new_cap);
  if (!new_buf)
    return false;

  tab->query_text = new_buf;
  tab->query_capacity = new_cap;
  return true;
}

/* Find line start for a given offset */
static size_t find_line_start(const char *text, size_t offset) {
  if (offset == 0)
    return 0;

  size_t pos = offset - 1;
  while (pos > 0 && text[pos] != '\n')
    pos--;

  return text[pos] == '\n' ? pos + 1 : pos;
}

/* Find line end for a given offset */
static size_t find_line_end(const char *text, size_t len, size_t offset) {
  size_t pos = offset;
  while (pos < len && text[pos] != '\n')
    pos++;
  return pos;
}

/* Count lines up to offset */
static size_t count_lines_to(const char *text, size_t offset) {
  size_t lines = 0;
  for (size_t i = 0; i < offset; i++) {
    if (text[i] == '\n')
      lines++;
  }
  return lines;
}

/* Get offset for line number */
static size_t offset_for_line(const char *text, size_t len, size_t line) {
  size_t current_line = 0;
  size_t offset = 0;

  while (offset < len && current_line < line) {
    if (text[offset] == '\n')
      current_line++;
    offset++;
  }

  return offset;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

VmQuery *vm_query_create(AppState *app, Tab *tab,
                         const VmQueryCallbacks *callbacks) {
  if (!tab || tab->type != TAB_TYPE_QUERY)
    return NULL;

  VmQuery *vm = calloc(1, sizeof(VmQuery));
  if (!vm)
    return NULL;

  vm->app = app;
  vm->tab = tab;
  vm->focus = VM_QUERY_FOCUS_EDITOR;
  vm->exec_state = VM_QUERY_IDLE;

  if (callbacks) {
    vm->callbacks = *callbacks;
  }

  return vm;
}

void vm_query_destroy(VmQuery *vm) {
  if (!vm)
    return;

  if (vm->results_vm) {
    vm_table_destroy(vm->results_vm);
  }

  free(vm);
}

void vm_query_bind(VmQuery *vm, Tab *tab) {
  if (!vm)
    return;

  vm->tab = tab;
  vm->selection.active = false;
  vm->exec_state = VM_QUERY_IDLE;

  /* Destroy old results VM */
  if (vm->results_vm) {
    vm_table_destroy(vm->results_vm);
    vm->results_vm = NULL;
  }

  notify_change(vm, VM_QUERY_CHANGE_ALL);
}

bool vm_query_valid(const VmQuery *vm) {
  return vm && vm->tab && vm->tab->type == TAB_TYPE_QUERY;
}

/* ============================================================================
 * Text Access
 * ============================================================================
 */

const char *vm_query_get_text(const VmQuery *vm) {
  if (!vm_query_valid(vm))
    return "";
  return vm->tab->query_text ? vm->tab->query_text : "";
}

size_t vm_query_get_length(const VmQuery *vm) {
  if (!vm_query_valid(vm))
    return 0;
  return vm->tab->query_len;
}

void vm_query_set_text(VmQuery *vm, const char *text) {
  if (!vm_query_valid(vm))
    return;

  Tab *tab = vm->tab;
  size_t len = text ? strlen(text) : 0;

  if (!ensure_capacity(tab, len + 1))
    return;

  if (text) {
    memcpy(tab->query_text, text, len);
  }
  tab->query_text[len] = '\0';
  tab->query_len = len;
  tab->query_cursor = len;

  vm->selection.active = false;

  notify_change(vm, VM_QUERY_CHANGE_TEXT | VM_QUERY_CHANGE_CURSOR);
}

void vm_query_insert_char(VmQuery *vm, char ch) {
  if (!vm_query_valid(vm))
    return;

  Tab *tab = vm->tab;

  /* Delete selection first if any */
  if (vm->selection.active) {
    vm_query_delete_selection(vm);
  }

  if (!ensure_capacity(tab, tab->query_len + 2))
    return;

  /* Insert at cursor */
  size_t cursor = tab->query_cursor;
  memmove(tab->query_text + cursor + 1, tab->query_text + cursor,
          tab->query_len - cursor + 1);
  tab->query_text[cursor] = ch;
  tab->query_len++;
  tab->query_cursor++;

  notify_change(vm, VM_QUERY_CHANGE_TEXT | VM_QUERY_CHANGE_CURSOR);
}

void vm_query_insert_text(VmQuery *vm, const char *text) {
  if (!vm_query_valid(vm) || !text)
    return;

  Tab *tab = vm->tab;
  size_t len = strlen(text);

  if (len == 0)
    return;

  /* Delete selection first if any */
  if (vm->selection.active) {
    vm_query_delete_selection(vm);
  }

  if (!ensure_capacity(tab, tab->query_len + len + 1))
    return;

  size_t cursor = tab->query_cursor;
  memmove(tab->query_text + cursor + len, tab->query_text + cursor,
          tab->query_len - cursor + 1);
  memcpy(tab->query_text + cursor, text, len);
  tab->query_len += len;
  tab->query_cursor += len;

  notify_change(vm, VM_QUERY_CHANGE_TEXT | VM_QUERY_CHANGE_CURSOR);
}

void vm_query_delete_char(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  Tab *tab = vm->tab;

  if (vm->selection.active) {
    vm_query_delete_selection(vm);
    return;
  }

  if (tab->query_cursor >= tab->query_len)
    return;

  memmove(tab->query_text + tab->query_cursor,
          tab->query_text + tab->query_cursor + 1,
          tab->query_len - tab->query_cursor);
  tab->query_len--;

  notify_change(vm, VM_QUERY_CHANGE_TEXT);
}

void vm_query_backspace(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  Tab *tab = vm->tab;

  if (vm->selection.active) {
    vm_query_delete_selection(vm);
    return;
  }

  if (tab->query_cursor == 0)
    return;

  tab->query_cursor--;
  vm_query_delete_char(vm);
}

void vm_query_delete_selection(VmQuery *vm) {
  if (!vm_query_valid(vm) || !vm->selection.active)
    return;

  Tab *tab = vm->tab;
  size_t start = vm->selection.start;
  size_t end = vm->selection.end;

  if (start > end) {
    size_t tmp = start;
    start = end;
    end = tmp;
  }

  if (end > tab->query_len)
    end = tab->query_len;

  size_t del_len = end - start;
  memmove(tab->query_text + start, tab->query_text + end,
          tab->query_len - end + 1);
  tab->query_len -= del_len;
  tab->query_cursor = start;

  vm->selection.active = false;

  notify_change(vm, VM_QUERY_CHANGE_TEXT | VM_QUERY_CHANGE_CURSOR |
                    VM_QUERY_CHANGE_SELECTION);
}

void vm_query_delete_line(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  Tab *tab = vm->tab;
  const char *text = tab->query_text;
  size_t len = tab->query_len;
  size_t cursor = tab->query_cursor;

  size_t line_start = find_line_start(text, cursor);
  size_t line_end = find_line_end(text, len, cursor);

  /* Include newline if present */
  if (line_end < len && text[line_end] == '\n')
    line_end++;

  size_t del_len = line_end - line_start;
  memmove(tab->query_text + line_start, tab->query_text + line_end,
          len - line_end + 1);
  tab->query_len -= del_len;
  tab->query_cursor = line_start;

  notify_change(vm, VM_QUERY_CHANGE_TEXT | VM_QUERY_CHANGE_CURSOR);
}

void vm_query_delete_to_end(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  Tab *tab = vm->tab;
  size_t cursor = tab->query_cursor;
  size_t line_end = find_line_end(tab->query_text, tab->query_len, cursor);

  if (line_end == cursor)
    return; /* Already at end */

  size_t del_len = line_end - cursor;
  memmove(tab->query_text + cursor, tab->query_text + line_end,
          tab->query_len - line_end + 1);
  tab->query_len -= del_len;

  notify_change(vm, VM_QUERY_CHANGE_TEXT);
}

/* ============================================================================
 * Cursor
 * ============================================================================
 */

size_t vm_query_get_cursor(const VmQuery *vm) {
  if (!vm_query_valid(vm))
    return 0;
  return vm->tab->query_cursor;
}

void vm_query_set_cursor(VmQuery *vm, size_t pos) {
  if (!vm_query_valid(vm))
    return;

  if (pos > vm->tab->query_len)
    pos = vm->tab->query_len;

  if (vm->tab->query_cursor != pos) {
    vm->tab->query_cursor = pos;
    notify_change(vm, VM_QUERY_CHANGE_CURSOR);
  }
}

void vm_query_move_cursor(VmQuery *vm, int delta) {
  size_t cursor = vm_query_get_cursor(vm);

  if (delta < 0 && (size_t)(-delta) > cursor) {
    vm_query_set_cursor(vm, 0);
  } else {
    vm_query_set_cursor(vm, cursor + delta);
  }
}

void vm_query_move_left(VmQuery *vm) {
  if (vm->selection.active) {
    /* Move to start of selection */
    size_t start = vm->selection.start < vm->selection.end
                       ? vm->selection.start : vm->selection.end;
    vm->selection.active = false;
    vm_query_set_cursor(vm, start);
    notify_change(vm, VM_QUERY_CHANGE_SELECTION);
  } else {
    vm_query_move_cursor(vm, -1);
  }
}

void vm_query_move_right(VmQuery *vm) {
  if (vm->selection.active) {
    /* Move to end of selection */
    size_t end = vm->selection.start > vm->selection.end
                     ? vm->selection.start : vm->selection.end;
    vm->selection.active = false;
    vm_query_set_cursor(vm, end);
    notify_change(vm, VM_QUERY_CHANGE_SELECTION);
  } else {
    vm_query_move_cursor(vm, 1);
  }
}

void vm_query_move_up(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  Tab *tab = vm->tab;
  const char *text = tab->query_text;
  size_t cursor = tab->query_cursor;

  /* Find current line start and column */
  size_t line_start = find_line_start(text, cursor);
  size_t col = cursor - line_start;

  if (line_start == 0)
    return; /* Already on first line */

  /* Find previous line */
  size_t prev_line_end = line_start - 1; /* Before newline */
  size_t prev_line_start = find_line_start(text, prev_line_end);
  size_t prev_line_len = prev_line_end - prev_line_start;

  /* Move to same column (or end of line) */
  if (col > prev_line_len)
    col = prev_line_len;

  vm->selection.active = false;
  vm_query_set_cursor(vm, prev_line_start + col);
  notify_change(vm, VM_QUERY_CHANGE_SELECTION);
}

void vm_query_move_down(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  Tab *tab = vm->tab;
  const char *text = tab->query_text;
  size_t len = tab->query_len;
  size_t cursor = tab->query_cursor;

  /* Find current line start and column */
  size_t line_start = find_line_start(text, cursor);
  size_t col = cursor - line_start;

  /* Find current line end */
  size_t line_end = find_line_end(text, len, cursor);

  if (line_end >= len)
    return; /* Already on last line */

  /* Find next line */
  size_t next_line_start = line_end + 1;
  size_t next_line_end = find_line_end(text, len, next_line_start);
  size_t next_line_len = next_line_end - next_line_start;

  /* Move to same column (or end of line) */
  if (col > next_line_len)
    col = next_line_len;

  vm->selection.active = false;
  vm_query_set_cursor(vm, next_line_start + col);
  notify_change(vm, VM_QUERY_CHANGE_SELECTION);
}

void vm_query_move_word_left(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  Tab *tab = vm->tab;
  const char *text = tab->query_text;
  size_t cursor = tab->query_cursor;

  if (cursor == 0)
    return;

  cursor--;

  /* Skip whitespace */
  while (cursor > 0 && isspace((unsigned char)text[cursor]))
    cursor--;

  /* Skip word characters */
  while (cursor > 0 && !isspace((unsigned char)text[cursor - 1]))
    cursor--;

  vm->selection.active = false;
  vm_query_set_cursor(vm, cursor);
  notify_change(vm, VM_QUERY_CHANGE_SELECTION);
}

void vm_query_move_word_right(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  Tab *tab = vm->tab;
  const char *text = tab->query_text;
  size_t len = tab->query_len;
  size_t cursor = tab->query_cursor;

  /* Skip word characters */
  while (cursor < len && !isspace((unsigned char)text[cursor]))
    cursor++;

  /* Skip whitespace */
  while (cursor < len && isspace((unsigned char)text[cursor]))
    cursor++;

  vm->selection.active = false;
  vm_query_set_cursor(vm, cursor);
  notify_change(vm, VM_QUERY_CHANGE_SELECTION);
}

void vm_query_home(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  size_t cursor = vm->tab->query_cursor;
  size_t line_start = find_line_start(vm->tab->query_text, cursor);

  vm->selection.active = false;
  vm_query_set_cursor(vm, line_start);
  notify_change(vm, VM_QUERY_CHANGE_SELECTION);
}

void vm_query_end(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  size_t cursor = vm->tab->query_cursor;
  size_t line_end = find_line_end(vm->tab->query_text, vm->tab->query_len, cursor);

  vm->selection.active = false;
  vm_query_set_cursor(vm, line_end);
  notify_change(vm, VM_QUERY_CHANGE_SELECTION);
}

void vm_query_doc_start(VmQuery *vm) {
  vm->selection.active = false;
  vm_query_set_cursor(vm, 0);
  notify_change(vm, VM_QUERY_CHANGE_SELECTION);
}

void vm_query_doc_end(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  vm->selection.active = false;
  vm_query_set_cursor(vm, vm->tab->query_len);
  notify_change(vm, VM_QUERY_CHANGE_SELECTION);
}

void vm_query_get_cursor_pos(const VmQuery *vm, size_t *line, size_t *col) {
  if (!vm_query_valid(vm)) {
    if (line) *line = 0;
    if (col) *col = 0;
    return;
  }

  const char *text = vm->tab->query_text;
  size_t cursor = vm->tab->query_cursor;

  if (line) {
    *line = count_lines_to(text, cursor);
  }

  if (col) {
    size_t line_start = find_line_start(text, cursor);
    *col = cursor - line_start;
  }
}

void vm_query_set_cursor_pos(VmQuery *vm, size_t line, size_t col) {
  if (!vm_query_valid(vm))
    return;

  const char *text = vm->tab->query_text;
  size_t len = vm->tab->query_len;

  size_t line_offset = offset_for_line(text, len, line);
  size_t line_end = find_line_end(text, len, line_offset);
  size_t line_len = line_end - line_offset;

  if (col > line_len)
    col = line_len;

  vm_query_set_cursor(vm, line_offset + col);
}

/* ============================================================================
 * Scroll
 * ============================================================================
 */

void vm_query_get_scroll(const VmQuery *vm, size_t *line, size_t *col) {
  if (!vm_query_valid(vm)) {
    if (line) *line = 0;
    if (col) *col = 0;
    return;
  }

  if (line) *line = vm->tab->query_scroll_line;
  if (col) *col = vm->tab->query_scroll_col;
}

void vm_query_set_scroll(VmQuery *vm, size_t line, size_t col) {
  if (!vm_query_valid(vm))
    return;

  vm->tab->query_scroll_line = line;
  vm->tab->query_scroll_col = col;
}

void vm_query_ensure_cursor_visible(VmQuery *vm, size_t visible_lines,
                                    size_t visible_cols) {
  if (!vm_query_valid(vm))
    return;

  size_t cursor_line, cursor_col;
  vm_query_get_cursor_pos(vm, &cursor_line, &cursor_col);

  size_t scroll_line = vm->tab->query_scroll_line;
  size_t scroll_col = vm->tab->query_scroll_col;

  /* Vertical scroll */
  if (cursor_line < scroll_line) {
    scroll_line = cursor_line;
  } else if (visible_lines > 0 && cursor_line >= scroll_line + visible_lines) {
    scroll_line = cursor_line - visible_lines + 1;
  }

  /* Horizontal scroll */
  if (cursor_col < scroll_col) {
    scroll_col = cursor_col;
  } else if (visible_cols > 0 && cursor_col >= scroll_col + visible_cols) {
    scroll_col = cursor_col - visible_cols + 1;
  }

  vm_query_set_scroll(vm, scroll_line, scroll_col);
}

/* ============================================================================
 * Selection
 * ============================================================================
 */

bool vm_query_has_selection(const VmQuery *vm) {
  return vm && vm->selection.active && vm->selection.start != vm->selection.end;
}

void vm_query_get_selection(const VmQuery *vm, size_t *start, size_t *end) {
  if (!vm || !vm->selection.active) {
    if (start) *start = 0;
    if (end) *end = 0;
    return;
  }

  size_t s = vm->selection.start;
  size_t e = vm->selection.end;

  if (s > e) {
    size_t tmp = s;
    s = e;
    e = tmp;
  }

  if (start) *start = s;
  if (end) *end = e;
}

char *vm_query_get_selected_text(const VmQuery *vm) {
  if (!vm_query_has_selection(vm))
    return NULL;

  size_t start, end;
  vm_query_get_selection(vm, &start, &end);

  size_t len = end - start;
  char *text = malloc(len + 1);
  if (!text)
    return NULL;

  memcpy(text, vm->tab->query_text + start, len);
  text[len] = '\0';

  return text;
}

void vm_query_set_selection(VmQuery *vm, size_t start, size_t end) {
  if (!vm_query_valid(vm))
    return;

  size_t len = vm->tab->query_len;
  if (start > len) start = len;
  if (end > len) end = len;

  vm->selection.start = start;
  vm->selection.end = end;
  vm->selection.active = (start != end);

  notify_change(vm, VM_QUERY_CHANGE_SELECTION);
}

void vm_query_select_all(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  vm_query_set_selection(vm, 0, vm->tab->query_len);
}

void vm_query_clear_selection(VmQuery *vm) {
  if (!vm)
    return;

  if (vm->selection.active) {
    vm->selection.active = false;
    notify_change(vm, VM_QUERY_CHANGE_SELECTION);
  }
}

void vm_query_extend_selection_left(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  size_t cursor = vm->tab->query_cursor;
  if (cursor == 0)
    return;

  if (!vm->selection.active) {
    vm->selection.start = cursor;
    vm->selection.active = true;
  }

  vm->tab->query_cursor--;
  vm->selection.end = vm->tab->query_cursor;

  notify_change(vm, VM_QUERY_CHANGE_CURSOR | VM_QUERY_CHANGE_SELECTION);
}

void vm_query_extend_selection_right(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  size_t cursor = vm->tab->query_cursor;
  if (cursor >= vm->tab->query_len)
    return;

  if (!vm->selection.active) {
    vm->selection.start = cursor;
    vm->selection.active = true;
  }

  vm->tab->query_cursor++;
  vm->selection.end = vm->tab->query_cursor;

  notify_change(vm, VM_QUERY_CHANGE_CURSOR | VM_QUERY_CHANGE_SELECTION);
}

void vm_query_extend_selection_up(VmQuery *vm) {
  /* TODO: Implement line-based selection extension */
  (void)vm;
}

void vm_query_extend_selection_down(VmQuery *vm) {
  /* TODO: Implement line-based selection extension */
  (void)vm;
}

void vm_query_extend_selection_to(VmQuery *vm, size_t pos) {
  if (!vm_query_valid(vm))
    return;

  if (!vm->selection.active) {
    vm->selection.start = vm->tab->query_cursor;
    vm->selection.active = true;
  }

  if (pos > vm->tab->query_len)
    pos = vm->tab->query_len;

  vm->selection.end = pos;
  vm->tab->query_cursor = pos;

  notify_change(vm, VM_QUERY_CHANGE_CURSOR | VM_QUERY_CHANGE_SELECTION);
}

/* ============================================================================
 * Line Information
 * ============================================================================
 */

size_t vm_query_line_count(const VmQuery *vm) {
  if (!vm_query_valid(vm) || !vm->tab->query_text)
    return 1;

  return count_lines_to(vm->tab->query_text, vm->tab->query_len) + 1;
}

const char *vm_query_line_at(const VmQuery *vm, size_t line, size_t *length) {
  if (!vm_query_valid(vm)) {
    if (length) *length = 0;
    return "";
  }

  const char *text = vm->tab->query_text;
  size_t len = vm->tab->query_len;

  size_t line_start = offset_for_line(text, len, line);
  size_t line_end = find_line_end(text, len, line_start);

  if (length) *length = line_end - line_start;

  return text + line_start;
}

size_t vm_query_line_offset(const VmQuery *vm, size_t line) {
  if (!vm_query_valid(vm))
    return 0;

  return offset_for_line(vm->tab->query_text, vm->tab->query_len, line);
}

/* ============================================================================
 * Focus
 * ============================================================================
 */

VmQueryFocus vm_query_get_focus(const VmQuery *vm) {
  if (!vm)
    return VM_QUERY_FOCUS_EDITOR;
  return vm->focus;
}

void vm_query_set_focus(VmQuery *vm, VmQueryFocus focus) {
  if (!vm)
    return;

  if (vm->focus != focus) {
    vm->focus = focus;
    notify_change(vm, VM_QUERY_CHANGE_FOCUS);
  }
}

void vm_query_toggle_focus(VmQuery *vm) {
  if (!vm)
    return;

  VmQueryFocus new_focus = (vm->focus == VM_QUERY_FOCUS_EDITOR)
                               ? VM_QUERY_FOCUS_RESULTS
                               : VM_QUERY_FOCUS_EDITOR;
  vm_query_set_focus(vm, new_focus);
}

/* ============================================================================
 * Execution
 * ============================================================================
 */

void vm_query_execute(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  /* Clear previous error */
  free(vm->tab->query_error);
  vm->tab->query_error = NULL;

  /* Get connection */
  Connection *conn = app_get_tab_connection(vm->app, vm->tab);
  if (!conn || !conn->conn) {
    vm->tab->query_error = str_dup("No database connection");
    notify_change(vm, VM_QUERY_CHANGE_ERROR);
    return;
  }

  /* Get query text */
  const char *sql = vm->tab->query_text;
  if (!sql || !*sql) {
    vm->tab->query_error = str_dup("Empty query");
    notify_change(vm, VM_QUERY_CHANGE_ERROR);
    return;
  }

  vm->exec_state = VM_QUERY_EXECUTING;
  notify_change(vm, VM_QUERY_CHANGE_EXECUTING);

  /* Execute query */
  char *err = NULL;
  ResultSet *results = db_query(conn->conn, sql, &err);

  if (results) {
    /* Free old results */
    if (vm->tab->query_results) {
      db_result_free(vm->tab->query_results);
    }
    vm->tab->query_results = results;
    vm->tab->query_affected = results->rows_affected;

    /* Destroy old results VM */
    if (vm->results_vm) {
      vm_table_destroy(vm->results_vm);
      vm->results_vm = NULL;
    }

    vm->exec_state = VM_QUERY_COMPLETE;
    notify_change(vm, VM_QUERY_CHANGE_RESULTS | VM_QUERY_CHANGE_EXECUTING);

    if (vm->callbacks.on_exec_complete) {
      vm->callbacks.on_exec_complete(vm, true, vm->callbacks.context);
    }
  } else {
    vm->tab->query_error = err ? err : str_dup("Query failed");
    vm->exec_state = VM_QUERY_COMPLETE;
    notify_change(vm, VM_QUERY_CHANGE_ERROR | VM_QUERY_CHANGE_EXECUTING);

    if (vm->callbacks.on_exec_complete) {
      vm->callbacks.on_exec_complete(vm, false, vm->callbacks.context);
    }
  }
}

void vm_query_execute_selected(VmQuery *vm) {
  if (!vm_query_has_selection(vm)) {
    vm_query_execute(vm);
    return;
  }

  /* Get selected text and execute it */
  char *selected = vm_query_get_selected_text(vm);
  if (!selected) {
    vm_query_execute(vm);
    return;
  }

  /* Temporarily replace query text */
  char *original = str_dup(vm->tab->query_text);
  vm_query_set_text(vm, selected);
  vm_query_execute(vm);

  /* Restore original text */
  if (original) {
    vm_query_set_text(vm, original);
    free(original);
  }
  free(selected);
}

void vm_query_cancel(VmQuery *vm) {
  if (!vm || vm->exec_state != VM_QUERY_EXECUTING)
    return;

  /* TODO: Implement query cancellation with async operation */
  vm->exec_state = VM_QUERY_CANCELLED;
  notify_change(vm, VM_QUERY_CHANGE_EXECUTING);
}

VmQueryExecState vm_query_exec_state(const VmQuery *vm) {
  if (!vm)
    return VM_QUERY_IDLE;
  return vm->exec_state;
}

bool vm_query_is_executing(const VmQuery *vm) {
  return vm && vm->exec_state == VM_QUERY_EXECUTING;
}

/* ============================================================================
 * Results
 * ============================================================================
 */

bool vm_query_has_results(const VmQuery *vm) {
  return vm_query_valid(vm) && vm->tab->query_results != NULL;
}

const ResultSet *vm_query_get_results(const VmQuery *vm) {
  if (!vm_query_valid(vm))
    return NULL;
  return vm->tab->query_results;
}

VmTable *vm_query_get_results_vm(VmQuery *vm) {
  if (!vm_query_valid(vm) || !vm->tab->query_results)
    return NULL;

  /* Create results VM lazily */
  if (!vm->results_vm) {
    /* Create a temporary tab-like wrapper for results */
    /* For now, return NULL - full implementation requires refactoring */
    return NULL;
  }

  return vm->results_vm;
}

int64_t vm_query_affected_rows(const VmQuery *vm) {
  if (!vm_query_valid(vm))
    return 0;
  return vm->tab->query_affected;
}

const char *vm_query_get_error(const VmQuery *vm) {
  if (!vm_query_valid(vm))
    return NULL;
  return vm->tab->query_error;
}

void vm_query_clear_results(VmQuery *vm) {
  if (!vm_query_valid(vm))
    return;

  if (vm->tab->query_results) {
    db_result_free(vm->tab->query_results);
    vm->tab->query_results = NULL;
  }

  if (vm->results_vm) {
    vm_table_destroy(vm->results_vm);
    vm->results_vm = NULL;
  }

  free(vm->tab->query_error);
  vm->tab->query_error = NULL;

  vm->tab->query_affected = 0;

  notify_change(vm, VM_QUERY_CHANGE_RESULTS);
}

/* ============================================================================
 * History
 * ============================================================================
 */

void vm_query_history_prev(VmQuery *vm) {
  /* TODO: Implement query history */
  (void)vm;
}

void vm_query_history_next(VmQuery *vm) {
  /* TODO: Implement query history */
  (void)vm;
}

/* ============================================================================
 * Clipboard
 * ============================================================================
 */

char *vm_query_copy(const VmQuery *vm) {
  return vm_query_get_selected_text(vm);
}

char *vm_query_cut(VmQuery *vm) {
  char *text = vm_query_get_selected_text(vm);
  if (text) {
    vm_query_delete_selection(vm);
  }
  return text;
}

void vm_query_paste(VmQuery *vm, const char *text) {
  vm_query_insert_text(vm, text);
}

/* ============================================================================
 * Utility
 * ============================================================================
 */

DbConnection *vm_query_connection(const VmQuery *vm) {
  if (!vm || !vm->app || !vm->tab)
    return NULL;

  Connection *conn = app_get_tab_connection(vm->app, vm->tab);
  return conn ? conn->conn : NULL;
}
