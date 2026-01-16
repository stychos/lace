/*
 * Lace
 * FiltersViewModel - Column filters panel view model
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "filters_viewmodel.h"
#include "../util/mem.h"
#include "../util/str.h"
#include <stdlib.h>
#include <string.h>

static bool filters_vm_handle_event(ViewModel *vm, const UiEvent *event);
static size_t filters_vm_get_row_count(const ViewModel *vm);
static size_t filters_vm_get_col_count(const ViewModel *vm);
static void filters_vm_on_focus_in(ViewModel *vm);
static void filters_vm_on_focus_out(ViewModel *vm);
static void filters_vm_validate_cursor_impl(ViewModel *vm);
static void filters_vm_ops_destroy(ViewModel *vm);

static const ViewModelOps s_filters_vm_ops = {
    .type_name = "FiltersViewModel",
    .handle_event = filters_vm_handle_event,
    .get_row_count = filters_vm_get_row_count,
    .get_col_count = filters_vm_get_col_count,
    .on_focus_in = filters_vm_on_focus_in,
    .on_focus_out = filters_vm_on_focus_out,
    .validate_cursor = filters_vm_validate_cursor_impl,
    .destroy = filters_vm_ops_destroy,
};

const ViewModelOps *filters_vm_ops(void) { return &s_filters_vm_ops; }

static bool filters_vm_handle_event(ViewModel *vm, const UiEvent *event) {
  (void)vm; (void)event;
  return false;
}

static size_t filters_vm_get_row_count(const ViewModel *vm) {
  return filters_vm_count((const FiltersViewModel *)vm);
}

static size_t filters_vm_get_col_count(const ViewModel *vm) {
  (void)vm;
  return FILTER_FIELD_COUNT;
}

static void filters_vm_on_focus_in(ViewModel *vm) { (void)vm; }

static void filters_vm_on_focus_out(ViewModel *vm) {
  FiltersViewModel *fvm = (FiltersViewModel *)vm;
  if (fvm->edit.active) filters_vm_cancel_edit(fvm);
  fvm->operator_menu_active = false;
  fvm->column_menu_active = false;
}

static void filters_vm_validate_cursor_impl(ViewModel *vm) {
  FiltersViewModel *fvm = (FiltersViewModel *)vm;
  size_t count = filters_vm_count(fvm);
  if (count > 0 && vm->state.cursor_row >= count) {
    vm->state.cursor_row = count - 1;
    vm_mark_dirty(vm, VM_CHANGE_CURSOR);
  }
  if (vm->state.cursor_col > FILTER_FIELD_VALUE) {
    vm->state.cursor_col = FILTER_FIELD_VALUE;
    vm_mark_dirty(vm, VM_CHANGE_CURSOR);
  }
}

static void filters_vm_ops_destroy(ViewModel *vm) {
  FiltersViewModel *fvm = (FiltersViewModel *)vm;
  fvm->edit.active = false;
  fvm->edit.buffer[0] = '\0';
  fvm->edit.buffer_len = 0;
  fvm->operator_menu_active = false;
  fvm->column_menu_active = false;
  memset(&fvm->filters_callbacks, 0, sizeof(fvm->filters_callbacks));
  fvm->filters = NULL;
  fvm->schema = NULL;
}

FiltersViewModel *filters_vm_create(void) {
  FiltersViewModel *vm = safe_calloc(1, sizeof(FiltersViewModel));
  vm_init(&vm->base, &s_filters_vm_ops);
  return vm;
}

void filters_vm_destroy(FiltersViewModel *vm) {
  if (!vm) return;
  vm_cleanup(&vm->base);
  free(vm);
}

void filters_vm_bind(FiltersViewModel *vm, TableFilters *filters, TableSchema *schema) {
  if (!vm) return;
  if (vm->edit.active) filters_vm_cancel_edit(vm);
  vm->filters = filters;
  vm->schema = schema;
  vm->base.state.cursor_row = 0;
  vm->base.state.cursor_col = 0;
  vm->base.state.scroll_row = 0;
  vm_notify(&vm->base, VM_CHANGE_DATA);
}

void filters_vm_set_callbacks(FiltersViewModel *vm, const FiltersViewModelCallbacks *callbacks) {
  if (!vm) return;
  if (callbacks) vm->filters_callbacks = *callbacks;
  else memset(&vm->filters_callbacks, 0, sizeof(vm->filters_callbacks));
}

size_t filters_vm_count(const FiltersViewModel *vm) {
  if (!vm || !vm->filters) return 0;
  return vm->filters->num_filters;
}

const ColumnFilter *filters_vm_filter_at(const FiltersViewModel *vm, size_t index) {
  if (!vm || !vm->filters || index >= vm->filters->num_filters) return NULL;
  return &vm->filters->filters[index];
}

const char *filters_vm_column_name(const FiltersViewModel *vm, size_t index) {
  const ColumnFilter *f = filters_vm_filter_at(vm, index);
  if (!f || !vm->schema) return NULL;
  if (f->column_index >= vm->schema->num_columns) return NULL;
  return vm->schema->columns[f->column_index].name;
}

const char *filters_vm_operator_name(const FiltersViewModel *vm, size_t index) {
  const ColumnFilter *f = filters_vm_filter_at(vm, index);
  if (!f) return NULL;
  return filter_op_name(f->op);
}

int filters_vm_add(FiltersViewModel *vm, size_t column_index) {
  if (!vm || !vm->filters) return -1;
  if (!filters_add(vm->filters, column_index, FILTER_OP_EQ, "")) return -1;
  size_t new_index = vm->filters->num_filters - 1;
  vm->base.state.cursor_row = new_index;
  vm->base.state.cursor_col = FILTER_FIELD_VALUE;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_FILTER_LIST | VM_CHANGE_CURSOR);
  if (vm->filters_callbacks.on_filters_changed)
    vm->filters_callbacks.on_filters_changed(vm, vm->filters_callbacks.context);
  return (int)new_index;
}

void filters_vm_remove(FiltersViewModel *vm, size_t index) {
  if (!vm || !vm->filters) return;
  filters_remove(vm->filters, index);
  if (vm->base.state.cursor_row >= vm->filters->num_filters && vm->filters->num_filters > 0)
    vm->base.state.cursor_row = vm->filters->num_filters - 1;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_FILTER_LIST | VM_CHANGE_CURSOR);
  if (vm->filters_callbacks.on_filters_changed)
    vm->filters_callbacks.on_filters_changed(vm, vm->filters_callbacks.context);
}

void filters_vm_remove_selected(FiltersViewModel *vm) {
  if (!vm) return;
  filters_vm_remove(vm, vm->base.state.cursor_row);
}

void filters_vm_clear_all(FiltersViewModel *vm) {
  if (!vm || !vm->filters) return;
  filters_clear(vm->filters);
  vm->base.state.cursor_row = 0;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_FILTER_LIST | VM_CHANGE_CURSOR);
  if (vm->filters_callbacks.on_filters_changed)
    vm->filters_callbacks.on_filters_changed(vm, vm->filters_callbacks.context);
}

bool filters_vm_start_edit(FiltersViewModel *vm) {
  if (!vm || !vm->filters || vm->filters->num_filters == 0) return false;
  size_t filter_idx = vm->base.state.cursor_row;
  if (filter_idx >= vm->filters->num_filters) return false;
  ColumnFilter *f = &vm->filters->filters[filter_idx];
  FilterEditField field = (FilterEditField)vm->base.state.cursor_col;
  if (field == FILTER_FIELD_COLUMN) {
    vm->column_menu_active = true;
    vm->column_menu_selection = f->column_index;
    vm_notify(&vm->base, FILTERS_VM_CHANGE_EDIT_MODE);
    return true;
  }
  if (field == FILTER_FIELD_OPERATOR) {
    vm->operator_menu_active = true;
    vm->operator_menu_selection = (size_t)f->op;
    vm_notify(&vm->base, FILTERS_VM_CHANGE_EDIT_MODE);
    return true;
  }
  const char *current_value = "";
  if (field == FILTER_FIELD_VALUE) current_value = f->value;
  else if (field == FILTER_FIELD_VALUE2) current_value = f->value2;
  vm->edit.active = true;
  vm->edit.filter_index = filter_idx;
  vm->edit.field = field;
  vm->edit.buffer_len = strlen(current_value);
  if (vm->edit.buffer_len >= sizeof(vm->edit.buffer))
    vm->edit.buffer_len = sizeof(vm->edit.buffer) - 1;
  memcpy(vm->edit.buffer, current_value, vm->edit.buffer_len);
  vm->edit.buffer[vm->edit.buffer_len] = '\0';
  vm->edit.cursor_pos = vm->edit.buffer_len;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_EDIT_MODE | VM_CHANGE_EDITING);
  return true;
}

void filters_vm_edit_insert_char(FiltersViewModel *vm, char ch) {
  if (!vm || !vm->edit.active) return;
  if (vm->edit.buffer_len >= sizeof(vm->edit.buffer) - 1) return;
  memmove(&vm->edit.buffer[vm->edit.cursor_pos + 1], &vm->edit.buffer[vm->edit.cursor_pos],
          vm->edit.buffer_len - vm->edit.cursor_pos + 1);
  vm->edit.buffer[vm->edit.cursor_pos] = ch;
  vm->edit.cursor_pos++;
  vm->edit.buffer_len++;
  vm_notify(&vm->base, VM_CHANGE_EDITING);
}

void filters_vm_edit_backspace(FiltersViewModel *vm) {
  if (!vm || !vm->edit.active || vm->edit.cursor_pos == 0) return;
  vm->edit.cursor_pos--;
  memmove(&vm->edit.buffer[vm->edit.cursor_pos], &vm->edit.buffer[vm->edit.cursor_pos + 1],
          vm->edit.buffer_len - vm->edit.cursor_pos);
  vm->edit.buffer_len--;
  vm_notify(&vm->base, VM_CHANGE_EDITING);
}

void filters_vm_edit_clear(FiltersViewModel *vm) {
  if (!vm || !vm->edit.active) return;
  vm->edit.buffer[0] = '\0';
  vm->edit.buffer_len = 0;
  vm->edit.cursor_pos = 0;
  vm_notify(&vm->base, VM_CHANGE_EDITING);
}

void filters_vm_edit_move_cursor(FiltersViewModel *vm, int delta) {
  if (!vm || !vm->edit.active) return;
  if (delta < 0) {
    size_t d = (size_t)(-delta);
    vm->edit.cursor_pos = (vm->edit.cursor_pos > d) ? vm->edit.cursor_pos - d : 0;
  } else {
    vm->edit.cursor_pos += (size_t)delta;
    if (vm->edit.cursor_pos > vm->edit.buffer_len)
      vm->edit.cursor_pos = vm->edit.buffer_len;
  }
  vm_notify(&vm->base, VM_CHANGE_EDITING);
}

bool filters_vm_commit_edit(FiltersViewModel *vm) {
  if (!vm || !vm->edit.active || !vm->filters) return false;
  if (vm->edit.filter_index >= vm->filters->num_filters) return false;
  ColumnFilter *f = &vm->filters->filters[vm->edit.filter_index];
  if (vm->edit.field == FILTER_FIELD_VALUE) {
    strncpy(f->value, vm->edit.buffer, sizeof(f->value) - 1);
    f->value[sizeof(f->value) - 1] = '\0';
  } else if (vm->edit.field == FILTER_FIELD_VALUE2) {
    strncpy(f->value2, vm->edit.buffer, sizeof(f->value2) - 1);
    f->value2[sizeof(f->value2) - 1] = '\0';
  }
  vm->edit.active = false;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_FILTER_LIST | VM_CHANGE_EDITING);
  if (vm->filters_callbacks.on_edit_complete)
    vm->filters_callbacks.on_edit_complete(vm, true, vm->filters_callbacks.context);
  if (vm->filters_callbacks.on_filters_changed)
    vm->filters_callbacks.on_filters_changed(vm, vm->filters_callbacks.context);
  return true;
}

void filters_vm_cancel_edit(FiltersViewModel *vm) {
  if (!vm) return;
  bool was_editing = vm->edit.active;
  vm->edit.active = false;
  if (was_editing) {
    vm_notify(&vm->base, VM_CHANGE_EDITING);
    if (vm->filters_callbacks.on_edit_complete)
      vm->filters_callbacks.on_edit_complete(vm, false, vm->filters_callbacks.context);
  }
}

bool filters_vm_is_editing(const FiltersViewModel *vm) {
  return vm ? vm->edit.active : false;
}

const char *filters_vm_edit_buffer(const FiltersViewModel *vm) {
  if (!vm || !vm->edit.active) return NULL;
  return vm->edit.buffer;
}

size_t filters_vm_edit_cursor(const FiltersViewModel *vm) {
  if (!vm || !vm->edit.active) return 0;
  return vm->edit.cursor_pos;
}

FilterEditField filters_vm_edit_field(const FiltersViewModel *vm) {
  if (!vm || !vm->edit.active) return FILTER_FIELD_COLUMN;
  return vm->edit.field;
}

bool filters_vm_operator_menu_active(const FiltersViewModel *vm) {
  return vm ? vm->operator_menu_active : false;
}

size_t filters_vm_operator_selection(const FiltersViewModel *vm) {
  return vm ? vm->operator_menu_selection : 0;
}

void filters_vm_operator_next(FiltersViewModel *vm) {
  if (!vm || !vm->operator_menu_active) return;
  vm->operator_menu_selection = (vm->operator_menu_selection + 1) % FILTER_OP_COUNT;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_EDIT_MODE);
}

void filters_vm_operator_prev(FiltersViewModel *vm) {
  if (!vm || !vm->operator_menu_active) return;
  if (vm->operator_menu_selection == 0) vm->operator_menu_selection = FILTER_OP_COUNT - 1;
  else vm->operator_menu_selection--;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_EDIT_MODE);
}

void filters_vm_operator_apply(FiltersViewModel *vm) {
  if (!vm || !vm->operator_menu_active || !vm->filters) return;
  size_t filter_idx = vm->base.state.cursor_row;
  if (filter_idx >= vm->filters->num_filters) return;
  vm->filters->filters[filter_idx].op = (FilterOperator)vm->operator_menu_selection;
  vm->operator_menu_active = false;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_FILTER_LIST | FILTERS_VM_CHANGE_EDIT_MODE);
  if (vm->filters_callbacks.on_filters_changed)
    vm->filters_callbacks.on_filters_changed(vm, vm->filters_callbacks.context);
}

void filters_vm_operator_cancel(FiltersViewModel *vm) {
  if (!vm) return;
  vm->operator_menu_active = false;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_EDIT_MODE);
}

bool filters_vm_column_menu_active(const FiltersViewModel *vm) {
  return vm ? vm->column_menu_active : false;
}

size_t filters_vm_column_selection(const FiltersViewModel *vm) {
  return vm ? vm->column_menu_selection : 0;
}

void filters_vm_column_next(FiltersViewModel *vm) {
  if (!vm || !vm->column_menu_active || !vm->schema) return;
  vm->column_menu_selection = (vm->column_menu_selection + 1) % vm->schema->num_columns;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_EDIT_MODE);
}

void filters_vm_column_prev(FiltersViewModel *vm) {
  if (!vm || !vm->column_menu_active || !vm->schema) return;
  if (vm->column_menu_selection == 0) vm->column_menu_selection = vm->schema->num_columns - 1;
  else vm->column_menu_selection--;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_EDIT_MODE);
}

void filters_vm_column_apply(FiltersViewModel *vm) {
  if (!vm || !vm->column_menu_active || !vm->filters) return;
  size_t filter_idx = vm->base.state.cursor_row;
  if (filter_idx >= vm->filters->num_filters) return;
  vm->filters->filters[filter_idx].column_index = vm->column_menu_selection;
  vm->column_menu_active = false;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_FILTER_LIST | FILTERS_VM_CHANGE_EDIT_MODE);
  if (vm->filters_callbacks.on_filters_changed)
    vm->filters_callbacks.on_filters_changed(vm, vm->filters_callbacks.context);
}

void filters_vm_column_cancel(FiltersViewModel *vm) {
  if (!vm) return;
  vm->column_menu_active = false;
  vm_notify(&vm->base, FILTERS_VM_CHANGE_EDIT_MODE);
}

bool filters_vm_valid(const FiltersViewModel *vm) {
  return vm && vm->filters && vm->schema;
}

FilterEditField filters_vm_current_field(const FiltersViewModel *vm) {
  if (!vm) return FILTER_FIELD_COLUMN;
  return (FilterEditField)vm->base.state.cursor_col;
}
