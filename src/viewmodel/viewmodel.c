/*
 * Lace
 * ViewModel - Base abstraction for all view models
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "viewmodel.h"
#include <string.h>

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================
 */

void vm_init(ViewModel *vm, const ViewModelOps *ops) {
  if (!vm)
    return;

  memset(vm, 0, sizeof(*vm));
  vm->ops = ops;
  vm->state.visible = true; /* Visible by default */
}

void vm_cleanup(ViewModel *vm) {
  if (!vm)
    return;

  /* Call viewmodel-specific cleanup */
  if (vm->ops && vm->ops->destroy) {
    vm->ops->destroy(vm);
  }

  /* Clear callbacks */
  memset(&vm->callbacks, 0, sizeof(vm->callbacks));
}

void vm_set_callbacks(ViewModel *vm, const ViewModelCallbacks *callbacks) {
  if (!vm)
    return;

  if (callbacks) {
    vm->callbacks = *callbacks;
  } else {
    memset(&vm->callbacks, 0, sizeof(vm->callbacks));
  }
}

/* ============================================================================
 * Cursor Functions
 * ============================================================================
 */

void vm_get_cursor(const ViewModel *vm, size_t *row, size_t *col) {
  if (!vm) {
    if (row)
      *row = 0;
    if (col)
      *col = 0;
    return;
  }

  if (row)
    *row = vm->state.cursor_row;
  if (col)
    *col = vm->state.cursor_col;
}

void vm_set_cursor(ViewModel *vm, size_t row, size_t col) {
  if (!vm)
    return;

  /* Clamp to valid range */
  size_t max_row = vm_row_count(vm);
  size_t max_col = vm_col_count(vm);

  if (max_row > 0 && row >= max_row)
    row = max_row - 1;
  if (max_col > 0 && col >= max_col)
    col = max_col - 1;

  /* Check if changed */
  if (vm->state.cursor_row == row && vm->state.cursor_col == col)
    return;

  vm->state.cursor_row = row;
  vm->state.cursor_col = col;

  vm_notify(vm, VM_CHANGE_CURSOR);
}

void vm_move_cursor(ViewModel *vm, int row_delta, int col_delta) {
  if (!vm)
    return;

  size_t row = vm->state.cursor_row;
  size_t col = vm->state.cursor_col;

  /* Calculate new position with underflow/overflow protection */
  if (row_delta < 0) {
    size_t delta = (size_t)(-row_delta);
    row = (row > delta) ? row - delta : 0;
  } else {
    row += (size_t)row_delta;
  }

  if (col_delta < 0) {
    size_t delta = (size_t)(-col_delta);
    col = (col > delta) ? col - delta : 0;
  } else {
    col += (size_t)col_delta;
  }

  vm_set_cursor(vm, row, col);
}

void vm_goto_first_row(ViewModel *vm) {
  if (!vm)
    return;
  vm_set_cursor(vm, 0, vm->state.cursor_col);
}

void vm_goto_last_row(ViewModel *vm) {
  if (!vm)
    return;

  size_t max_row = vm_row_count(vm);
  size_t row = (max_row > 0) ? max_row - 1 : 0;
  vm_set_cursor(vm, row, vm->state.cursor_col);
}

void vm_goto_first_col(ViewModel *vm) {
  if (!vm)
    return;
  vm_set_cursor(vm, vm->state.cursor_row, 0);
}

void vm_goto_last_col(ViewModel *vm) {
  if (!vm)
    return;

  size_t max_col = vm_col_count(vm);
  size_t col = (max_col > 0) ? max_col - 1 : 0;
  vm_set_cursor(vm, vm->state.cursor_row, col);
}

/* ============================================================================
 * Scroll Functions
 * ============================================================================
 */

void vm_get_scroll(const ViewModel *vm, size_t *row, size_t *col) {
  if (!vm) {
    if (row)
      *row = 0;
    if (col)
      *col = 0;
    return;
  }

  if (row)
    *row = vm->state.scroll_row;
  if (col)
    *col = vm->state.scroll_col;
}

void vm_set_scroll(ViewModel *vm, size_t row, size_t col) {
  if (!vm)
    return;

  /* Check if changed */
  if (vm->state.scroll_row == row && vm->state.scroll_col == col)
    return;

  vm->state.scroll_row = row;
  vm->state.scroll_col = col;

  vm_notify(vm, VM_CHANGE_SCROLL);
}

void vm_scroll_to_cursor(ViewModel *vm, size_t visible_rows,
                         size_t visible_cols) {
  if (!vm || visible_rows == 0)
    return;

  size_t scroll_row = vm->state.scroll_row;
  size_t scroll_col = vm->state.scroll_col;
  size_t cursor_row = vm->state.cursor_row;
  size_t cursor_col = vm->state.cursor_col;

  /* Adjust vertical scroll */
  if (cursor_row < scroll_row) {
    scroll_row = cursor_row;
  } else if (cursor_row >= scroll_row + visible_rows) {
    scroll_row = cursor_row - visible_rows + 1;
  }

  /* Adjust horizontal scroll */
  if (visible_cols > 0) {
    if (cursor_col < scroll_col) {
      scroll_col = cursor_col;
    } else if (cursor_col >= scroll_col + visible_cols) {
      scroll_col = cursor_col - visible_cols + 1;
    }
  }

  vm_set_scroll(vm, scroll_row, scroll_col);
}

void vm_page_up(ViewModel *vm, size_t page_size) {
  if (!vm || page_size == 0)
    return;

  size_t cursor_row = vm->state.cursor_row;
  size_t scroll_row = vm->state.scroll_row;

  /* Move cursor up by page size */
  if (cursor_row > page_size) {
    cursor_row -= page_size;
  } else {
    cursor_row = 0;
  }

  /* Move scroll up by page size */
  if (scroll_row > page_size) {
    scroll_row -= page_size;
  } else {
    scroll_row = 0;
  }

  /* Update state (will trigger notifications) */
  vm->state.scroll_row = scroll_row;
  vm->state.scroll_col = vm->state.scroll_col;
  vm_set_cursor(vm, cursor_row, vm->state.cursor_col);

  /* Ensure scroll contains cursor */
  vm_scroll_to_cursor(vm, page_size, 0);
}

void vm_page_down(ViewModel *vm, size_t page_size) {
  if (!vm || page_size == 0)
    return;

  size_t cursor_row = vm->state.cursor_row;
  size_t scroll_row = vm->state.scroll_row;
  size_t max_row = vm_row_count(vm);

  if (max_row == 0)
    return;

  /* Move cursor down by page size */
  cursor_row += page_size;
  if (cursor_row >= max_row) {
    cursor_row = max_row - 1;
  }

  /* Move scroll down by page size */
  scroll_row += page_size;
  size_t max_scroll = (max_row > page_size) ? max_row - page_size : 0;
  if (scroll_row > max_scroll) {
    scroll_row = max_scroll;
  }

  /* Update state */
  vm->state.scroll_row = scroll_row;
  vm_set_cursor(vm, cursor_row, vm->state.cursor_col);

  /* Ensure scroll contains cursor */
  vm_scroll_to_cursor(vm, page_size, 0);
}

/* ============================================================================
 * Focus Functions
 * ============================================================================
 */

bool vm_is_focused(const ViewModel *vm) {
  return vm ? vm->state.focused : false;
}

void vm_set_focus(ViewModel *vm, bool focused) {
  if (!vm)
    return;

  if (vm->state.focused == focused)
    return;

  vm->state.focused = focused;

  /* Call ops callbacks */
  if (vm->ops) {
    if (focused && vm->ops->on_focus_in) {
      vm->ops->on_focus_in(vm);
    } else if (!focused && vm->ops->on_focus_out) {
      vm->ops->on_focus_out(vm);
    }
  }

  /* Call user callbacks */
  if (focused && vm->callbacks.on_focus) {
    vm->callbacks.on_focus(vm, vm->callbacks.context);
  } else if (!focused && vm->callbacks.on_blur) {
    vm->callbacks.on_blur(vm, vm->callbacks.context);
  }

  vm_notify(vm, VM_CHANGE_FOCUS);
}

/* ============================================================================
 * Visibility Functions
 * ============================================================================
 */

bool vm_is_visible(const ViewModel *vm) {
  return vm ? vm->state.visible : false;
}

void vm_set_visible(ViewModel *vm, bool visible) {
  if (!vm)
    return;

  if (vm->state.visible == visible)
    return;

  vm->state.visible = visible;

  /* Lose focus when hidden */
  if (!visible && vm->state.focused) {
    vm_set_focus(vm, false);
  }

  vm_notify(vm, VM_CHANGE_VISIBLE);
}

/* ============================================================================
 * Notification Functions
 * ============================================================================
 */

void vm_notify(ViewModel *vm, VMChangeFlags flags) {
  if (!vm || flags == VM_CHANGE_NONE)
    return;

  /* Mark dirty */
  vm->state.dirty |= flags;

  /* Call user callback */
  if (vm->callbacks.on_change) {
    vm->callbacks.on_change(vm, flags, vm->callbacks.context);
  }
}

void vm_mark_dirty(ViewModel *vm, VMChangeFlags flags) {
  if (!vm)
    return;
  vm->state.dirty |= flags;
}

void vm_clear_dirty(ViewModel *vm) {
  if (!vm)
    return;
  vm->state.dirty = VM_CHANGE_NONE;
}

bool vm_is_dirty(const ViewModel *vm) {
  return vm ? (vm->state.dirty != VM_CHANGE_NONE) : false;
}

/* ============================================================================
 * Input Handling
 * ============================================================================
 */

bool vm_handle_event(ViewModel *vm, const UiEvent *event) {
  if (!vm || !event)
    return false;

  if (vm->ops && vm->ops->handle_event) {
    return vm->ops->handle_event(vm, event);
  }

  return false;
}

/* ============================================================================
 * Dimension Queries
 * ============================================================================
 */

size_t vm_row_count(const ViewModel *vm) {
  if (!vm || !vm->ops || !vm->ops->get_row_count)
    return 0;
  return vm->ops->get_row_count(vm);
}

size_t vm_col_count(const ViewModel *vm) {
  if (!vm || !vm->ops || !vm->ops->get_col_count)
    return 0;
  return vm->ops->get_col_count(vm);
}

/* ============================================================================
 * Utility Functions
 * ============================================================================
 */

void vm_validate_cursor(ViewModel *vm) {
  if (!vm)
    return;

  /* Use viewmodel-specific validation if available */
  if (vm->ops && vm->ops->validate_cursor) {
    vm->ops->validate_cursor(vm);
    return;
  }

  /* Default: clamp to bounds */
  size_t max_row = vm_row_count(vm);
  size_t max_col = vm_col_count(vm);

  if (max_row > 0 && vm->state.cursor_row >= max_row) {
    vm->state.cursor_row = max_row - 1;
    vm_mark_dirty(vm, VM_CHANGE_CURSOR);
  }

  if (max_col > 0 && vm->state.cursor_col >= max_col) {
    vm->state.cursor_col = max_col - 1;
    vm_mark_dirty(vm, VM_CHANGE_CURSOR);
  }
}

const char *vm_type_name(const ViewModel *vm) {
  if (!vm || !vm->ops)
    return "Unknown";
  return vm->ops->type_name ? vm->ops->type_name : "ViewModel";
}

bool vm_valid(const ViewModel *vm) { return vm && vm->ops; }
