/*
 * Lace
 * SidebarViewModel - Table list sidebar view model
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "sidebar_viewmodel.h"
#include "../util/mem.h"
#include "../util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool sidebar_vm_handle_event(ViewModel *vm, const UiEvent *event);
static size_t sidebar_vm_get_row_count(const ViewModel *vm);
static size_t sidebar_vm_get_col_count(const ViewModel *vm);
static void sidebar_vm_on_focus_in(ViewModel *vm);
static void sidebar_vm_on_focus_out(ViewModel *vm);
static void sidebar_vm_validate_cursor_impl(ViewModel *vm);
static void sidebar_vm_ops_destroy(ViewModel *vm);

static const ViewModelOps s_sidebar_vm_ops = {
    .type_name = "SidebarViewModel",
    .handle_event = sidebar_vm_handle_event,
    .get_row_count = sidebar_vm_get_row_count,
    .get_col_count = sidebar_vm_get_col_count,
    .on_focus_in = sidebar_vm_on_focus_in,
    .on_focus_out = sidebar_vm_on_focus_out,
    .validate_cursor = sidebar_vm_validate_cursor_impl,
    .destroy = sidebar_vm_ops_destroy,
};

const ViewModelOps *sidebar_vm_ops(void) { return &s_sidebar_vm_ops; }

static bool str_icontains(const char *haystack, const char *needle) {
  if (!haystack || !needle) return false;
  if (!*needle) return true;
  size_t needle_len = strlen(needle);
  size_t hay_len = strlen(haystack);
  if (needle_len > hay_len) return false;
  for (size_t i = 0; i <= hay_len - needle_len; i++) {
    bool match = true;
    for (size_t j = 0; j < needle_len; j++) {
      if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

static void rebuild_filter(SidebarViewModel *vm) {
  if (!vm || !vm->connection) return;
  size_t num_tables = vm->connection->num_tables;
  char **tables = vm->connection->tables;
  if (vm->filtered_capacity < num_tables) {
    size_t new_cap = num_tables > 0 ? num_tables : 16;
    vm->filtered_indices = safe_reallocarray(vm->filtered_indices, new_cap, sizeof(size_t));
    vm->filtered_capacity = new_cap;
  }
  vm->filtered_count = 0;
  for (size_t i = 0; i < num_tables; i++) {
    if (tables[i] && (vm->filter_len == 0 || str_icontains(tables[i], vm->filter))) {
      vm->filtered_indices[vm->filtered_count++] = i;
    }
  }
  vm_validate_cursor(&vm->base);
}

static bool sidebar_vm_handle_event(ViewModel *vm, const UiEvent *event) {
  (void)vm; (void)event;
  return false;
}

static size_t sidebar_vm_get_row_count(const ViewModel *vm) {
  return sidebar_vm_count((const SidebarViewModel *)vm);
}

static size_t sidebar_vm_get_col_count(const ViewModel *vm) {
  (void)vm;
  return 1;
}

static void sidebar_vm_on_focus_in(ViewModel *vm) { (void)vm; }
static void sidebar_vm_on_focus_out(ViewModel *vm) { (void)vm; }

static void sidebar_vm_validate_cursor_impl(ViewModel *vm) {
  SidebarViewModel *svm = (SidebarViewModel *)vm;
  size_t count = sidebar_vm_count(svm);
  if (count > 0 && vm->state.cursor_row >= count) {
    vm->state.cursor_row = count - 1;
    vm_mark_dirty(vm, VM_CHANGE_CURSOR);
  }
}

static void sidebar_vm_ops_destroy(ViewModel *vm) {
  SidebarViewModel *svm = (SidebarViewModel *)vm;
  free(svm->filtered_indices);
  svm->filtered_indices = NULL;
  svm->filtered_count = 0;
  svm->filtered_capacity = 0;
  svm->filter[0] = '\0';
  svm->filter_len = 0;
  svm->filter_active = false;
  memset(&svm->sidebar_callbacks, 0, sizeof(svm->sidebar_callbacks));
  svm->app = NULL;
  svm->connection = NULL;
}

SidebarViewModel *sidebar_vm_create(AppState *app) {
  SidebarViewModel *vm = safe_calloc(1, sizeof(SidebarViewModel));
  vm_init(&vm->base, &s_sidebar_vm_ops);
  vm->app = app;
  return vm;
}

void sidebar_vm_destroy(SidebarViewModel *vm) {
  if (!vm) return;
  vm_cleanup(&vm->base);
  free(vm);
}

void sidebar_vm_bind(SidebarViewModel *vm, Connection *conn) {
  if (!vm) return;
  vm->connection = conn;
  vm->filter[0] = '\0';
  vm->filter_len = 0;
  vm->filter_active = false;
  rebuild_filter(vm);
  vm->base.state.cursor_row = 0;
  vm->base.state.scroll_row = 0;
  vm_notify(&vm->base, VM_CHANGE_DATA | SIDEBAR_VM_CHANGE_TABLES);
}

void sidebar_vm_set_callbacks(SidebarViewModel *vm, const SidebarViewModelCallbacks *callbacks) {
  if (!vm) return;
  if (callbacks) vm->sidebar_callbacks = *callbacks;
  else memset(&vm->sidebar_callbacks, 0, sizeof(vm->sidebar_callbacks));
}

size_t sidebar_vm_count(const SidebarViewModel *vm) {
  return vm ? vm->filtered_count : 0;
}

size_t sidebar_vm_total_count(const SidebarViewModel *vm) {
  if (!vm || !vm->connection) return 0;
  return vm->connection->num_tables;
}

const char *sidebar_vm_table_at(const SidebarViewModel *vm, size_t index) {
  if (!vm || !vm->connection || index >= vm->filtered_count) return NULL;
  size_t orig_index = vm->filtered_indices[index];
  if (orig_index >= vm->connection->num_tables) return NULL;
  return vm->connection->tables[orig_index];
}

size_t sidebar_vm_original_index(const SidebarViewModel *vm, size_t filtered_index) {
  if (!vm || filtered_index >= vm->filtered_count) return 0;
  return vm->filtered_indices[filtered_index];
}

size_t sidebar_vm_find_table(const SidebarViewModel *vm, const char *name) {
  if (!vm || !name) return vm ? vm->filtered_count : 0;
  for (size_t i = 0; i < vm->filtered_count; i++) {
    const char *table_name = sidebar_vm_table_at(vm, i);
    if (table_name && strcmp(table_name, name) == 0) return i;
  }
  return vm->filtered_count;
}

const char *sidebar_vm_selected_name(const SidebarViewModel *vm) {
  if (!vm) return NULL;
  return sidebar_vm_table_at(vm, vm->base.state.cursor_row);
}

size_t sidebar_vm_selected_original_index(const SidebarViewModel *vm) {
  if (!vm) return 0;
  return sidebar_vm_original_index(vm, vm->base.state.cursor_row);
}

void sidebar_vm_ensure_visible(SidebarViewModel *vm, size_t visible_count) {
  if (!vm || visible_count == 0) return;
  vm_scroll_to_cursor(&vm->base, visible_count, 1);
}

const char *sidebar_vm_get_filter(const SidebarViewModel *vm) {
  return vm ? vm->filter : "";
}

void sidebar_vm_set_filter(SidebarViewModel *vm, const char *filter) {
  if (!vm) return;
  if (filter) {
    size_t len = strlen(filter);
    if (len >= SIDEBAR_FILTER_MAX) len = SIDEBAR_FILTER_MAX - 1;
    memcpy(vm->filter, filter, len);
    vm->filter[len] = '\0';
    vm->filter_len = len;
    vm->filter_active = (len > 0);
  } else {
    vm->filter[0] = '\0';
    vm->filter_len = 0;
    vm->filter_active = false;
  }
  rebuild_filter(vm);
  vm->base.state.cursor_row = 0;
  vm->base.state.scroll_row = 0;
  vm_notify(&vm->base, SIDEBAR_VM_CHANGE_FILTER | VM_CHANGE_CURSOR);
}

void sidebar_vm_filter_append(SidebarViewModel *vm, char ch) {
  if (!vm || vm->filter_len >= SIDEBAR_FILTER_MAX - 1) return;
  vm->filter[vm->filter_len++] = ch;
  vm->filter[vm->filter_len] = '\0';
  vm->filter_active = true;
  rebuild_filter(vm);
  vm->base.state.cursor_row = 0;
  vm->base.state.scroll_row = 0;
  vm_notify(&vm->base, SIDEBAR_VM_CHANGE_FILTER | VM_CHANGE_CURSOR);
}

void sidebar_vm_filter_backspace(SidebarViewModel *vm) {
  if (!vm || vm->filter_len == 0) return;
  vm->filter[--vm->filter_len] = '\0';
  vm->filter_active = (vm->filter_len > 0);
  rebuild_filter(vm);
  vm_notify(&vm->base, SIDEBAR_VM_CHANGE_FILTER);
}

void sidebar_vm_filter_clear(SidebarViewModel *vm) {
  if (!vm || vm->filter_len == 0) return;
  vm->filter[0] = '\0';
  vm->filter_len = 0;
  vm->filter_active = false;
  rebuild_filter(vm);
  vm_notify(&vm->base, SIDEBAR_VM_CHANGE_FILTER);
}

bool sidebar_vm_filter_active(const SidebarViewModel *vm) {
  return vm ? vm->filter_active : false;
}

void sidebar_vm_open_selected(SidebarViewModel *vm) {
  if (!vm) return;
  const char *name = sidebar_vm_selected_name(vm);
  size_t orig_index = sidebar_vm_selected_original_index(vm);
  if (vm->sidebar_callbacks.on_table_open) {
    vm->sidebar_callbacks.on_table_open(vm, orig_index, name, vm->sidebar_callbacks.context);
  }
}

void sidebar_vm_refresh(SidebarViewModel *vm) {
  if (!vm) return;
  vm->is_loading = true;
  vm_notify(&vm->base, SIDEBAR_VM_CHANGE_LOADING);
}

bool sidebar_vm_is_loading(const SidebarViewModel *vm) {
  return vm ? vm->is_loading : false;
}

bool sidebar_vm_valid(const SidebarViewModel *vm) {
  return vm && vm->connection && vm->connection->tables && vm->connection->num_tables > 0;
}

const char *sidebar_vm_connection_name(const SidebarViewModel *vm) {
  if (!vm || !vm->connection) return NULL;
  return vm->connection->connstr;
}
