/*
 * Lace
 * Sidebar ViewModel - Implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "vm_sidebar.h"
#include "../util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

static void notify_change(VmSidebar *vm, VmSidebarChangeFlags flags) {
  if (vm->callbacks.on_change) {
    vm->callbacks.on_change(vm, flags, vm->callbacks.context);
  }
}

/* Case-insensitive substring match */
static bool str_contains_ci(const char *haystack, const char *needle) {
  if (!haystack || !needle)
    return false;
  if (!*needle)
    return true;

  size_t needle_len = strlen(needle);
  size_t haystack_len = strlen(haystack);

  if (needle_len > haystack_len)
    return false;

  for (size_t i = 0; i <= haystack_len - needle_len; i++) {
    bool match = true;
    for (size_t j = 0; j < needle_len; j++) {
      if (tolower((unsigned char)haystack[i + j]) !=
          tolower((unsigned char)needle[j])) {
        match = false;
        break;
      }
    }
    if (match)
      return true;
  }

  return false;
}

/* Rebuild filtered list based on current filter */
static void rebuild_filtered(VmSidebar *vm) {
  vm->filtered_count = 0;

  if (!vm->connection || !vm->connection->tables)
    return;

  size_t total = vm->connection->num_tables;

  /* Ensure capacity */
  if (vm->filtered_capacity < total) {
    size_t *new_indices = realloc(vm->filtered_indices, total * sizeof(size_t));
    if (!new_indices)
      return;
    vm->filtered_indices = new_indices;
    vm->filtered_capacity = total;
  }

  /* Build filtered list */
  for (size_t i = 0; i < total; i++) {
    const char *name = vm->connection->tables[i];
    if (!name)
      continue;

    if (!vm->filter_active || vm->filter_len == 0 ||
        str_contains_ci(name, vm->filter)) {
      vm->filtered_indices[vm->filtered_count++] = i;
    }
  }

  /* Adjust selection if it's out of bounds */
  if (vm->filtered_count > 0 && vm->selection >= vm->filtered_count) {
    vm->selection = vm->filtered_count - 1;
  }
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

VmSidebar *vm_sidebar_create(AppState *app, const VmSidebarCallbacks *callbacks) {
  VmSidebar *vm = calloc(1, sizeof(VmSidebar));
  if (!vm)
    return NULL;

  vm->app = app;

  if (callbacks) {
    vm->callbacks = *callbacks;
  }

  return vm;
}

void vm_sidebar_destroy(VmSidebar *vm) {
  if (!vm)
    return;

  free(vm->filtered_indices);
  free(vm);
}

void vm_sidebar_bind(VmSidebar *vm, Connection *conn) {
  if (!vm)
    return;

  vm->connection = conn;
  vm->selection = 0;
  vm->scroll = 0;

  rebuild_filtered(vm);
  notify_change(vm, VM_SIDEBAR_CHANGE_ALL);
}

bool vm_sidebar_valid(const VmSidebar *vm) {
  return vm && vm->connection && vm->connection->tables &&
         vm->connection->num_tables > 0;
}

/* ============================================================================
 * Table List Access
 * ============================================================================
 */

size_t vm_sidebar_count(const VmSidebar *vm) {
  if (!vm)
    return 0;
  return vm->filtered_count;
}

size_t vm_sidebar_total_count(const VmSidebar *vm) {
  if (!vm || !vm->connection)
    return 0;
  return vm->connection->num_tables;
}

const char *vm_sidebar_table_at(const VmSidebar *vm, size_t index) {
  if (!vm || !vm->connection || index >= vm->filtered_count)
    return NULL;

  size_t orig_index = vm->filtered_indices[index];
  if (orig_index >= vm->connection->num_tables)
    return NULL;

  return vm->connection->tables[orig_index];
}

size_t vm_sidebar_original_index(const VmSidebar *vm, size_t filtered_index) {
  if (!vm || filtered_index >= vm->filtered_count)
    return 0;
  return vm->filtered_indices[filtered_index];
}

bool vm_sidebar_matches_filter(const VmSidebar *vm, size_t original_index) {
  if (!vm || !vm->connection || original_index >= vm->connection->num_tables)
    return false;

  if (!vm->filter_active || vm->filter_len == 0)
    return true;

  const char *name = vm->connection->tables[original_index];
  return name && str_contains_ci(name, vm->filter);
}

/* ============================================================================
 * Selection
 * ============================================================================
 */

size_t vm_sidebar_get_selection(const VmSidebar *vm) {
  if (!vm)
    return 0;
  return vm->selection;
}

void vm_sidebar_set_selection(VmSidebar *vm, size_t index) {
  if (!vm)
    return;

  if (index >= vm->filtered_count && vm->filtered_count > 0)
    index = vm->filtered_count - 1;

  if (vm->selection != index) {
    vm->selection = index;
    notify_change(vm, VM_SIDEBAR_CHANGE_SELECTION);
  }
}

void vm_sidebar_select_next(VmSidebar *vm) {
  if (!vm || vm->filtered_count == 0)
    return;

  if (vm->selection < vm->filtered_count - 1) {
    vm->selection++;
    notify_change(vm, VM_SIDEBAR_CHANGE_SELECTION);
  }
}

void vm_sidebar_select_prev(VmSidebar *vm) {
  if (!vm || vm->selection == 0)
    return;

  vm->selection--;
  notify_change(vm, VM_SIDEBAR_CHANGE_SELECTION);
}

void vm_sidebar_select_first(VmSidebar *vm) {
  vm_sidebar_set_selection(vm, 0);
}

void vm_sidebar_select_last(VmSidebar *vm) {
  if (!vm || vm->filtered_count == 0)
    return;
  vm_sidebar_set_selection(vm, vm->filtered_count - 1);
}

const char *vm_sidebar_selected_name(const VmSidebar *vm) {
  return vm_sidebar_table_at(vm, vm_sidebar_get_selection(vm));
}

size_t vm_sidebar_selected_original_index(const VmSidebar *vm) {
  if (!vm || vm->filtered_count == 0)
    return 0;
  return vm_sidebar_original_index(vm, vm->selection);
}

/* ============================================================================
 * Scroll
 * ============================================================================
 */

size_t vm_sidebar_get_scroll(const VmSidebar *vm) {
  if (!vm)
    return 0;
  return vm->scroll;
}

void vm_sidebar_set_scroll(VmSidebar *vm, size_t scroll) {
  if (!vm)
    return;

  if (scroll > vm->filtered_count)
    scroll = vm->filtered_count > 0 ? vm->filtered_count - 1 : 0;

  if (vm->scroll != scroll) {
    vm->scroll = scroll;
    notify_change(vm, VM_SIDEBAR_CHANGE_SCROLL);
  }
}

void vm_sidebar_ensure_visible(VmSidebar *vm, size_t visible_count) {
  if (!vm || visible_count == 0)
    return;

  /* If selection is above scroll, scroll up */
  if (vm->selection < vm->scroll) {
    vm_sidebar_set_scroll(vm, vm->selection);
  }
  /* If selection is below visible area, scroll down */
  else if (vm->selection >= vm->scroll + visible_count) {
    vm_sidebar_set_scroll(vm, vm->selection - visible_count + 1);
  }
}

/* ============================================================================
 * Filtering
 * ============================================================================
 */

const char *vm_sidebar_get_filter(const VmSidebar *vm) {
  if (!vm)
    return "";
  return vm->filter;
}

void vm_sidebar_set_filter(VmSidebar *vm, const char *filter) {
  if (!vm)
    return;

  if (!filter) {
    vm->filter[0] = '\0';
    vm->filter_len = 0;
    vm->filter_active = false;
  } else {
    size_t len = strlen(filter);
    if (len >= sizeof(vm->filter))
      len = sizeof(vm->filter) - 1;

    memcpy(vm->filter, filter, len);
    vm->filter[len] = '\0';
    vm->filter_len = len;
    vm->filter_active = (len > 0);
  }

  /* Save current selection's original index */
  size_t old_orig = 0;
  if (vm->filtered_count > 0) {
    old_orig = vm->filtered_indices[vm->selection];
  }

  rebuild_filtered(vm);

  /* Try to find the same table in new filtered list */
  for (size_t i = 0; i < vm->filtered_count; i++) {
    if (vm->filtered_indices[i] == old_orig) {
      vm->selection = i;
      break;
    }
  }

  notify_change(vm, VM_SIDEBAR_CHANGE_FILTER | VM_SIDEBAR_CHANGE_TABLES);
}

void vm_sidebar_filter_append(VmSidebar *vm, char ch) {
  if (!vm || vm->filter_len >= sizeof(vm->filter) - 1)
    return;

  vm->filter[vm->filter_len++] = ch;
  vm->filter[vm->filter_len] = '\0';
  vm->filter_active = true;

  rebuild_filtered(vm);
  notify_change(vm, VM_SIDEBAR_CHANGE_FILTER | VM_SIDEBAR_CHANGE_TABLES);
}

void vm_sidebar_filter_backspace(VmSidebar *vm) {
  if (!vm || vm->filter_len == 0)
    return;

  vm->filter_len--;
  vm->filter[vm->filter_len] = '\0';
  vm->filter_active = (vm->filter_len > 0);

  rebuild_filtered(vm);
  notify_change(vm, VM_SIDEBAR_CHANGE_FILTER | VM_SIDEBAR_CHANGE_TABLES);
}

void vm_sidebar_filter_clear(VmSidebar *vm) {
  vm_sidebar_set_filter(vm, NULL);
}

bool vm_sidebar_filter_active(const VmSidebar *vm) {
  return vm && vm->filter_active;
}

/* ============================================================================
 * Actions
 * ============================================================================
 */

void vm_sidebar_open_selected(VmSidebar *vm) {
  if (!vm || vm->filtered_count == 0)
    return;

  const char *name = vm_sidebar_selected_name(vm);
  size_t index = vm_sidebar_selected_original_index(vm);

  if (vm->callbacks.on_table_open) {
    vm->callbacks.on_table_open(vm, index, name, vm->callbacks.context);
  }
}

void vm_sidebar_refresh(VmSidebar *vm) {
  if (!vm)
    return;

  vm->loading = true;
  notify_change(vm, VM_SIDEBAR_CHANGE_LOADING);

  /* Actual refresh will be handled by the UI layer calling db_list_tables */
  /* When complete, call vm_sidebar_bind() again */
}

/* ============================================================================
 * State
 * ============================================================================
 */

bool vm_sidebar_is_loading(const VmSidebar *vm) {
  return vm && vm->loading;
}

const char *vm_sidebar_connection_name(const VmSidebar *vm) {
  if (!vm || !vm->connection)
    return NULL;
  return vm->connection->connstr;
}
