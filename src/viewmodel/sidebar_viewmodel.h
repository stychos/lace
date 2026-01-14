/*
 * Lace
 * SidebarViewModel - Table list sidebar view model
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_SIDEBAR_VIEWMODEL_H
#define LACE_SIDEBAR_VIEWMODEL_H

#include "../core/app_state.h"
#include "viewmodel.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct SidebarViewModel SidebarViewModel;

#define SIDEBAR_VM_CHANGE_TABLES  (1u << 8)
#define SIDEBAR_VM_CHANGE_FILTER  (1u << 9)
#define SIDEBAR_VM_CHANGE_LOADING (1u << 10)

typedef struct SidebarViewModelCallbacks {
  void (*on_table_select)(SidebarViewModel *vm, size_t index, const char *name,
                          void *ctx);
  void (*on_table_open)(SidebarViewModel *vm, size_t index, const char *name,
                        void *ctx);
  void *context;
} SidebarViewModelCallbacks;

#define SIDEBAR_FILTER_MAX 64

struct SidebarViewModel {
  ViewModel base;
  AppState *app;
  Connection *connection;
  SidebarViewModelCallbacks sidebar_callbacks;
  char filter[SIDEBAR_FILTER_MAX];
  size_t filter_len;
  bool filter_active;
  size_t *filtered_indices;
  size_t filtered_count;
  size_t filtered_capacity;
  bool is_loading;
};

/* Lifecycle */
SidebarViewModel *sidebar_vm_create(AppState *app);
void sidebar_vm_destroy(SidebarViewModel *vm);
void sidebar_vm_bind(SidebarViewModel *vm, Connection *conn);
void sidebar_vm_set_callbacks(SidebarViewModel *vm,
                              const SidebarViewModelCallbacks *callbacks);

/* Table List Access */
size_t sidebar_vm_count(const SidebarViewModel *vm);
size_t sidebar_vm_total_count(const SidebarViewModel *vm);
const char *sidebar_vm_table_at(const SidebarViewModel *vm, size_t index);
size_t sidebar_vm_original_index(const SidebarViewModel *vm, size_t filtered_index);
size_t sidebar_vm_find_table(const SidebarViewModel *vm, const char *name);

/* Selection */
const char *sidebar_vm_selected_name(const SidebarViewModel *vm);
size_t sidebar_vm_selected_original_index(const SidebarViewModel *vm);
void sidebar_vm_ensure_visible(SidebarViewModel *vm, size_t visible_count);

/* Filtering */
const char *sidebar_vm_get_filter(const SidebarViewModel *vm);
void sidebar_vm_set_filter(SidebarViewModel *vm, const char *filter);
void sidebar_vm_filter_append(SidebarViewModel *vm, char ch);
void sidebar_vm_filter_backspace(SidebarViewModel *vm);
void sidebar_vm_filter_clear(SidebarViewModel *vm);
bool sidebar_vm_filter_active(const SidebarViewModel *vm);

/* Actions */
void sidebar_vm_open_selected(SidebarViewModel *vm);
void sidebar_vm_refresh(SidebarViewModel *vm);

/* State */
bool sidebar_vm_is_loading(const SidebarViewModel *vm);
bool sidebar_vm_valid(const SidebarViewModel *vm);
const char *sidebar_vm_connection_name(const SidebarViewModel *vm);

const ViewModelOps *sidebar_vm_ops(void);

/* Backward Compatibility */
typedef SidebarViewModel SidebarWidget;
typedef SidebarViewModelCallbacks SidebarWidgetCallbacks;
#define SIDEBAR_CHANGE_TABLES  SIDEBAR_VM_CHANGE_TABLES
#define SIDEBAR_CHANGE_FILTER  SIDEBAR_VM_CHANGE_FILTER
#define SIDEBAR_CHANGE_LOADING SIDEBAR_VM_CHANGE_LOADING
#define sidebar_widget_create            sidebar_vm_create
#define sidebar_widget_destroy           sidebar_vm_destroy
#define sidebar_widget_bind              sidebar_vm_bind
#define sidebar_widget_set_callbacks     sidebar_vm_set_callbacks
#define sidebar_widget_count             sidebar_vm_count
#define sidebar_widget_total_count       sidebar_vm_total_count
#define sidebar_widget_table_at          sidebar_vm_table_at
#define sidebar_widget_original_index    sidebar_vm_original_index
#define sidebar_widget_find_table        sidebar_vm_find_table
#define sidebar_widget_selected_name     sidebar_vm_selected_name
#define sidebar_widget_selected_original_index sidebar_vm_selected_original_index
#define sidebar_widget_ensure_visible    sidebar_vm_ensure_visible
#define sidebar_widget_get_filter        sidebar_vm_get_filter
#define sidebar_widget_set_filter        sidebar_vm_set_filter
#define sidebar_widget_filter_append     sidebar_vm_filter_append
#define sidebar_widget_filter_backspace  sidebar_vm_filter_backspace
#define sidebar_widget_filter_clear      sidebar_vm_filter_clear
#define sidebar_widget_filter_active     sidebar_vm_filter_active
#define sidebar_widget_open_selected     sidebar_vm_open_selected
#define sidebar_widget_refresh           sidebar_vm_refresh
#define sidebar_widget_is_loading        sidebar_vm_is_loading
#define sidebar_widget_valid             sidebar_vm_valid
#define sidebar_widget_connection_name   sidebar_vm_connection_name
#define sidebar_widget_ops               sidebar_vm_ops

#endif /* LACE_SIDEBAR_VIEWMODEL_H */
