/*
 * Lace
 * FiltersViewModel - Column filters panel view model
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FILTERS_VIEWMODEL_H
#define LACE_FILTERS_VIEWMODEL_H

#include "../core/app_state.h"
#include "viewmodel.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct FiltersViewModel FiltersViewModel;

#define FILTERS_VM_CHANGE_FILTER_LIST (1u << 8)
#define FILTERS_VM_CHANGE_EDIT_MODE   (1u << 9)
#define FILTERS_VM_CHANGE_APPLIED     (1u << 10)

typedef enum {
  FILTER_FIELD_COLUMN = 0,
  FILTER_FIELD_OPERATOR = 1,
  FILTER_FIELD_VALUE = 2,
  FILTER_FIELD_VALUE2 = 3,
  FILTER_FIELD_COUNT = 4
} FilterEditField;

typedef struct FiltersEditState {
  bool active;
  size_t filter_index;
  FilterEditField field;
  char buffer[512];
  size_t buffer_len;
  size_t cursor_pos;
} FiltersEditState;

typedef struct FiltersViewModelCallbacks {
  void (*on_filters_changed)(FiltersViewModel *vm, void *ctx);
  void (*on_edit_complete)(FiltersViewModel *vm, bool committed, void *ctx);
  void *context;
} FiltersViewModelCallbacks;

struct FiltersViewModel {
  ViewModel base;
  TableFilters *filters;
  TableSchema *schema;
  FiltersViewModelCallbacks filters_callbacks;
  FiltersEditState edit;
  size_t operator_menu_selection;
  bool operator_menu_active;
  size_t column_menu_selection;
  bool column_menu_active;
};

/* Lifecycle */
FiltersViewModel *filters_vm_create(void);
void filters_vm_destroy(FiltersViewModel *vm);
void filters_vm_bind(FiltersViewModel *vm, TableFilters *filters, TableSchema *schema);
void filters_vm_set_callbacks(FiltersViewModel *vm, const FiltersViewModelCallbacks *callbacks);

/* Filter List Access */
size_t filters_vm_count(const FiltersViewModel *vm);
const ColumnFilter *filters_vm_filter_at(const FiltersViewModel *vm, size_t index);
const char *filters_vm_column_name(const FiltersViewModel *vm, size_t index);
const char *filters_vm_operator_name(const FiltersViewModel *vm, size_t index);

/* Filter Operations */
int filters_vm_add(FiltersViewModel *vm, size_t column_index);
void filters_vm_remove(FiltersViewModel *vm, size_t index);
void filters_vm_remove_selected(FiltersViewModel *vm);
void filters_vm_clear_all(FiltersViewModel *vm);

/* Editing */
bool filters_vm_start_edit(FiltersViewModel *vm);
void filters_vm_edit_insert_char(FiltersViewModel *vm, char ch);
void filters_vm_edit_backspace(FiltersViewModel *vm);
void filters_vm_edit_clear(FiltersViewModel *vm);
void filters_vm_edit_move_cursor(FiltersViewModel *vm, int delta);
bool filters_vm_commit_edit(FiltersViewModel *vm);
void filters_vm_cancel_edit(FiltersViewModel *vm);
bool filters_vm_is_editing(const FiltersViewModel *vm);
const char *filters_vm_edit_buffer(const FiltersViewModel *vm);
size_t filters_vm_edit_cursor(const FiltersViewModel *vm);
FilterEditField filters_vm_edit_field(const FiltersViewModel *vm);

/* Operator Menu */
bool filters_vm_operator_menu_active(const FiltersViewModel *vm);
size_t filters_vm_operator_selection(const FiltersViewModel *vm);
void filters_vm_operator_next(FiltersViewModel *vm);
void filters_vm_operator_prev(FiltersViewModel *vm);
void filters_vm_operator_apply(FiltersViewModel *vm);
void filters_vm_operator_cancel(FiltersViewModel *vm);

/* Column Menu */
bool filters_vm_column_menu_active(const FiltersViewModel *vm);
size_t filters_vm_column_selection(const FiltersViewModel *vm);
void filters_vm_column_next(FiltersViewModel *vm);
void filters_vm_column_prev(FiltersViewModel *vm);
void filters_vm_column_apply(FiltersViewModel *vm);
void filters_vm_column_cancel(FiltersViewModel *vm);

/* State */
bool filters_vm_valid(const FiltersViewModel *vm);
FilterEditField filters_vm_current_field(const FiltersViewModel *vm);

const ViewModelOps *filters_vm_ops(void);

/* Backward Compatibility */
typedef FiltersViewModel FiltersWidget;
typedef FiltersViewModelCallbacks FiltersWidgetCallbacks;
#define FILTERS_CHANGE_FILTER_LIST FILTERS_VM_CHANGE_FILTER_LIST
#define FILTERS_CHANGE_EDIT_MODE   FILTERS_VM_CHANGE_EDIT_MODE
#define FILTERS_CHANGE_APPLIED     FILTERS_VM_CHANGE_APPLIED
#define filters_widget_create            filters_vm_create
#define filters_widget_destroy           filters_vm_destroy
#define filters_widget_bind              filters_vm_bind
#define filters_widget_set_callbacks     filters_vm_set_callbacks
#define filters_widget_count             filters_vm_count
#define filters_widget_filter_at         filters_vm_filter_at
#define filters_widget_column_name       filters_vm_column_name
#define filters_widget_operator_name     filters_vm_operator_name
#define filters_widget_add               filters_vm_add
#define filters_widget_remove            filters_vm_remove
#define filters_widget_remove_selected   filters_vm_remove_selected
#define filters_widget_clear_all         filters_vm_clear_all
#define filters_widget_start_edit        filters_vm_start_edit
#define filters_widget_edit_insert_char  filters_vm_edit_insert_char
#define filters_widget_edit_backspace    filters_vm_edit_backspace
#define filters_widget_edit_clear        filters_vm_edit_clear
#define filters_widget_edit_move_cursor  filters_vm_edit_move_cursor
#define filters_widget_commit_edit       filters_vm_commit_edit
#define filters_widget_cancel_edit       filters_vm_cancel_edit
#define filters_widget_is_editing        filters_vm_is_editing
#define filters_widget_edit_buffer       filters_vm_edit_buffer
#define filters_widget_edit_cursor       filters_vm_edit_cursor
#define filters_widget_edit_field        filters_vm_edit_field
#define filters_widget_operator_menu_active filters_vm_operator_menu_active
#define filters_widget_operator_selection filters_vm_operator_selection
#define filters_widget_operator_next     filters_vm_operator_next
#define filters_widget_operator_prev     filters_vm_operator_prev
#define filters_widget_operator_apply    filters_vm_operator_apply
#define filters_widget_operator_cancel   filters_vm_operator_cancel
#define filters_widget_column_menu_active filters_vm_column_menu_active
#define filters_widget_column_selection  filters_vm_column_selection
#define filters_widget_column_next       filters_vm_column_next
#define filters_widget_column_prev       filters_vm_column_prev
#define filters_widget_column_apply      filters_vm_column_apply
#define filters_widget_column_cancel     filters_vm_column_cancel
#define filters_widget_valid             filters_vm_valid
#define filters_widget_current_field     filters_vm_current_field
#define filters_widget_ops               filters_vm_ops

#endif /* LACE_FILTERS_VIEWMODEL_H */
