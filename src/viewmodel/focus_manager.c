/*
 * Lace
 * FocusManager - Widget focus routing and management
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "focus_manager.h"
#include <string.h>

/* ============================================================================
 * Helper: Find widget index in widgets array
 * ============================================================================
 */

static int find_widget_index(const FocusManager *fm, Widget *widget) {
  if (!fm || !widget)
    return -1;

  for (size_t i = 0; i < fm->num_widgets; i++) {
    if (fm->widgets[i] == widget)
      return (int)i;
  }
  return -1;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

void focus_manager_init(FocusManager *fm) {
  if (!fm)
    return;

  memset(fm, 0, sizeof(FocusManager));
}

void focus_manager_clear(FocusManager *fm) {
  if (!fm)
    return;

  /* Clear focus on all widgets */
  for (size_t i = 0; i < fm->num_widgets; i++) {
    if (fm->widgets[i]) {
      widget_set_focus(fm->widgets[i], false);
    }
  }

  /* Reset state */
  memset(fm->widgets, 0, sizeof(fm->widgets));
  fm->num_widgets = 0;
  fm->primary_focus = NULL;
  memset(fm->focus_stack, 0, sizeof(fm->focus_stack));
  fm->focus_stack_size = 0;
  memset(fm->focus_order, 0, sizeof(fm->focus_order));
  fm->focus_order_size = 0;
}

void focus_manager_set_callbacks(FocusManager *fm,
                                 const FocusManagerCallbacks *callbacks) {
  if (!fm)
    return;

  if (callbacks) {
    fm->callbacks = *callbacks;
  } else {
    memset(&fm->callbacks, 0, sizeof(fm->callbacks));
  }
}

/* ============================================================================
 * Widget Registration
 * ============================================================================
 */

bool focus_manager_add_widget(FocusManager *fm, Widget *widget) {
  if (!fm || !widget)
    return false;

  /* Check capacity */
  if (fm->num_widgets >= FOCUS_MAX_WIDGETS)
    return false;

  /* Check if already registered */
  if (find_widget_index(fm, widget) >= 0)
    return true; /* Already registered, not an error */

  /* Add widget */
  fm->widgets[fm->num_widgets] = widget;
  fm->focus_order[fm->focus_order_size] = fm->num_widgets;
  fm->num_widgets++;
  fm->focus_order_size++;

  return true;
}

void focus_manager_remove_widget(FocusManager *fm, Widget *widget) {
  if (!fm || !widget)
    return;

  int idx = find_widget_index(fm, widget);
  if (idx < 0)
    return;

  /* If this widget has focus, clear it */
  if (fm->primary_focus == widget) {
    focus_manager_clear_focus(fm);
  }

  /* Remove from focus stack */
  for (size_t i = 0; i < fm->focus_stack_size; i++) {
    if (fm->focus_stack[i] == widget) {
      /* Shift stack down */
      memmove(&fm->focus_stack[i], &fm->focus_stack[i + 1],
              (fm->focus_stack_size - i - 1) * sizeof(Widget *));
      fm->focus_stack_size--;
      break;
    }
  }

  /* Shift widgets array down */
  memmove(&fm->widgets[idx], &fm->widgets[idx + 1],
          (fm->num_widgets - (size_t)idx - 1) * sizeof(Widget *));
  fm->num_widgets--;

  /* Update focus order to remove this widget and adjust indices */
  size_t new_order_size = 0;
  for (size_t i = 0; i < fm->focus_order_size; i++) {
    if (fm->focus_order[i] == (size_t)idx)
      continue; /* Skip removed widget */

    if (fm->focus_order[i] > (size_t)idx) {
      fm->focus_order[new_order_size] = fm->focus_order[i] - 1;
    } else {
      fm->focus_order[new_order_size] = fm->focus_order[i];
    }
    new_order_size++;
  }
  fm->focus_order_size = new_order_size;
}

bool focus_manager_has_widget(FocusManager *fm, Widget *widget) {
  return find_widget_index(fm, widget) >= 0;
}

/* ============================================================================
 * Focus Management
 * ============================================================================
 */

Widget *focus_manager_get_focus(const FocusManager *fm) {
  return fm ? fm->primary_focus : NULL;
}

void focus_manager_set_focus(FocusManager *fm, Widget *widget) {
  if (!fm)
    return;

  /* No change? */
  if (fm->primary_focus == widget)
    return;

  Widget *old_focus = fm->primary_focus;

  /* Clear old focus */
  if (old_focus) {
    widget_set_focus(old_focus, false);
  }

  /* Set new focus */
  fm->primary_focus = widget;
  if (widget) {
    widget_set_focus(widget, true);
  }

  /* Notify callback */
  if (fm->callbacks.on_focus_change) {
    fm->callbacks.on_focus_change(fm, old_focus, widget, fm->callbacks.context);
  }
}

void focus_manager_cycle_next(FocusManager *fm) {
  if (!fm || fm->focus_order_size == 0)
    return;

  /* Find current focus position in order */
  size_t current_pos = 0;
  for (size_t i = 0; i < fm->focus_order_size; i++) {
    size_t widget_idx = fm->focus_order[i];
    if (widget_idx < fm->num_widgets &&
        fm->widgets[widget_idx] == fm->primary_focus) {
      current_pos = i;
      break;
    }
  }

  /* Find next visible widget */
  for (size_t i = 1; i <= fm->focus_order_size; i++) {
    size_t next_pos = (current_pos + i) % fm->focus_order_size;
    size_t widget_idx = fm->focus_order[next_pos];
    if (widget_idx < fm->num_widgets) {
      Widget *widget = fm->widgets[widget_idx];
      if (widget && widget->state.visible) {
        focus_manager_set_focus(fm, widget);
        return;
      }
    }
  }
}

void focus_manager_cycle_prev(FocusManager *fm) {
  if (!fm || fm->focus_order_size == 0)
    return;

  /* Find current focus position in order */
  size_t current_pos = 0;
  for (size_t i = 0; i < fm->focus_order_size; i++) {
    size_t widget_idx = fm->focus_order[i];
    if (widget_idx < fm->num_widgets &&
        fm->widgets[widget_idx] == fm->primary_focus) {
      current_pos = i;
      break;
    }
  }

  /* Find previous visible widget */
  for (size_t i = 1; i <= fm->focus_order_size; i++) {
    size_t prev_pos =
        (current_pos + fm->focus_order_size - i) % fm->focus_order_size;
    size_t widget_idx = fm->focus_order[prev_pos];
    if (widget_idx < fm->num_widgets) {
      Widget *widget = fm->widgets[widget_idx];
      if (widget && widget->state.visible) {
        focus_manager_set_focus(fm, widget);
        return;
      }
    }
  }
}

void focus_manager_clear_focus(FocusManager *fm) {
  if (!fm)
    return;

  focus_manager_set_focus(fm, NULL);
}

/* ============================================================================
 * Split-View Focus Stack
 * ============================================================================
 */

bool focus_manager_push_focus(FocusManager *fm, Widget *widget) {
  if (!fm || !widget)
    return false;

  if (fm->focus_stack_size >= FOCUS_MAX_STACK)
    return false;

  /* Don't push duplicates */
  if (focus_manager_in_stack(fm, widget))
    return true;

  fm->focus_stack[fm->focus_stack_size++] = widget;
  widget_set_focus(widget, true);

  /* Set as primary if first on stack */
  if (fm->focus_stack_size == 1) {
    fm->primary_focus = widget;
  }

  /* Notify callback */
  if (fm->callbacks.on_stack_change) {
    fm->callbacks.on_stack_change(fm, fm->callbacks.context);
  }

  return true;
}

Widget *focus_manager_pop_focus(FocusManager *fm) {
  if (!fm || fm->focus_stack_size == 0)
    return NULL;

  Widget *widget = fm->focus_stack[--fm->focus_stack_size];
  fm->focus_stack[fm->focus_stack_size] = NULL;

  widget_set_focus(widget, false);

  /* Update primary focus */
  if (fm->focus_stack_size > 0) {
    fm->primary_focus = fm->focus_stack[fm->focus_stack_size - 1];
  } else {
    fm->primary_focus = NULL;
  }

  /* Notify callback */
  if (fm->callbacks.on_stack_change) {
    fm->callbacks.on_stack_change(fm, fm->callbacks.context);
  }

  return widget;
}

size_t focus_manager_stack_size(const FocusManager *fm) {
  return fm ? fm->focus_stack_size : 0;
}

bool focus_manager_in_stack(const FocusManager *fm, Widget *widget) {
  if (!fm || !widget)
    return false;

  for (size_t i = 0; i < fm->focus_stack_size; i++) {
    if (fm->focus_stack[i] == widget)
      return true;
  }
  return false;
}

/* ============================================================================
 * Event Routing
 * ============================================================================
 */

bool focus_manager_route_event(FocusManager *fm, const UiEvent *event) {
  if (!fm || !event)
    return false;

  /* If focus stack has entries, route to all (for split-view) */
  if (fm->focus_stack_size > 0) {
    for (size_t i = 0; i < fm->focus_stack_size; i++) {
      Widget *widget = fm->focus_stack[i];
      if (widget && widget->ops && widget->ops->handle_event) {
        if (widget->ops->handle_event(widget, event)) {
          return true; /* Event consumed */
        }
      }
    }
    return false;
  }

  /* Otherwise route to primary focus */
  if (fm->primary_focus && fm->primary_focus->ops &&
      fm->primary_focus->ops->handle_event) {
    return fm->primary_focus->ops->handle_event(fm->primary_focus, event);
  }

  return false;
}

bool focus_manager_send_event(FocusManager *fm, Widget *widget,
                              const UiEvent *event) {
  if (!fm || !widget || !event)
    return false;

  if (widget->ops && widget->ops->handle_event) {
    return widget->ops->handle_event(widget, event);
  }

  return false;
}

/* ============================================================================
 * Focus Order
 * ============================================================================
 */

void focus_manager_set_order(FocusManager *fm, const size_t *order,
                             size_t count) {
  if (!fm || !order || count == 0)
    return;

  size_t copy_count = count < FOCUS_MAX_WIDGETS ? count : FOCUS_MAX_WIDGETS;
  memcpy(fm->focus_order, order, copy_count * sizeof(size_t));
  fm->focus_order_size = copy_count;
}

void focus_manager_reset_order(FocusManager *fm) {
  if (!fm)
    return;

  /* Reset to registration order */
  for (size_t i = 0; i < fm->num_widgets && i < FOCUS_MAX_WIDGETS; i++) {
    fm->focus_order[i] = i;
  }
  fm->focus_order_size = fm->num_widgets;
}

/* ============================================================================
 * Visibility Integration
 * ============================================================================
 */

Widget *focus_manager_next_visible(FocusManager *fm) {
  if (!fm || fm->focus_order_size == 0)
    return NULL;

  /* Find current focus position in order */
  size_t current_pos = 0;
  for (size_t i = 0; i < fm->focus_order_size; i++) {
    size_t widget_idx = fm->focus_order[i];
    if (widget_idx < fm->num_widgets &&
        fm->widgets[widget_idx] == fm->primary_focus) {
      current_pos = i;
      break;
    }
  }

  /* Find next visible widget */
  for (size_t i = 1; i <= fm->focus_order_size; i++) {
    size_t next_pos = (current_pos + i) % fm->focus_order_size;
    size_t widget_idx = fm->focus_order[next_pos];
    if (widget_idx < fm->num_widgets) {
      Widget *widget = fm->widgets[widget_idx];
      if (widget && widget->state.visible) {
        return widget;
      }
    }
  }

  return NULL;
}

Widget *focus_manager_prev_visible(FocusManager *fm) {
  if (!fm || fm->focus_order_size == 0)
    return NULL;

  /* Find current focus position in order */
  size_t current_pos = 0;
  for (size_t i = 0; i < fm->focus_order_size; i++) {
    size_t widget_idx = fm->focus_order[i];
    if (widget_idx < fm->num_widgets &&
        fm->widgets[widget_idx] == fm->primary_focus) {
      current_pos = i;
      break;
    }
  }

  /* Find previous visible widget */
  for (size_t i = 1; i <= fm->focus_order_size; i++) {
    size_t prev_pos =
        (current_pos + fm->focus_order_size - i) % fm->focus_order_size;
    size_t widget_idx = fm->focus_order[prev_pos];
    if (widget_idx < fm->num_widgets) {
      Widget *widget = fm->widgets[widget_idx];
      if (widget && widget->state.visible) {
        return widget;
      }
    }
  }

  return NULL;
}

void focus_manager_focus_first_visible(FocusManager *fm) {
  if (!fm || fm->num_widgets == 0)
    return;

  /* Find first visible widget in order */
  for (size_t i = 0; i < fm->focus_order_size; i++) {
    size_t widget_idx = fm->focus_order[i];
    if (widget_idx < fm->num_widgets) {
      Widget *widget = fm->widgets[widget_idx];
      if (widget && widget->state.visible) {
        focus_manager_set_focus(fm, widget);
        return;
      }
    }
  }
}
