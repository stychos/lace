/*
 * Lace
 * FocusManager - Widget focus routing and management
 *
 * Provides centralized focus management for widgets:
 * - Routes input events to focused widget(s)
 * - Manages focus stack for split-view scenarios
 * - Focus cycling (Tab key) and directional navigation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FOCUS_MANAGER_H
#define LACE_FOCUS_MANAGER_H

#include "viewmodel.h"
#include <stdbool.h>
#include <stddef.h>

/* Maximum widgets in a focus group */
#define FOCUS_MAX_WIDGETS 8

/* Maximum focus stack depth (for split-view) */
#define FOCUS_MAX_STACK 4

/* Forward declaration */
typedef struct FocusManager FocusManager;

/* ============================================================================
 * FocusManager Callbacks
 * ============================================================================
 */

typedef struct FocusManagerCallbacks {
  /* Called when focus changes between widgets */
  void (*on_focus_change)(FocusManager *fm, Widget *old_widget,
                          Widget *new_widget, void *ctx);

  /* Called when focus stack changes (split-view) */
  void (*on_stack_change)(FocusManager *fm, void *ctx);

  /* User context */
  void *context;
} FocusManagerCallbacks;

/* ============================================================================
 * FocusManager
 * ============================================================================
 */

struct FocusManager {
  /* All widgets in the focus group */
  Widget *widgets[FOCUS_MAX_WIDGETS];
  size_t num_widgets;

  /* Primary focused widget */
  Widget *primary_focus;

  /* Focus stack for split-view (multiple widgets with input focus) */
  Widget *focus_stack[FOCUS_MAX_STACK];
  size_t focus_stack_size;

  /* Focus order (indices into widgets[] for Tab cycling) */
  size_t focus_order[FOCUS_MAX_WIDGETS];
  size_t focus_order_size;

  /* Callbacks */
  FocusManagerCallbacks callbacks;
};

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Initialize focus manager */
void focus_manager_init(FocusManager *fm);

/* Clear focus manager state */
void focus_manager_clear(FocusManager *fm);

/* Set callbacks */
void focus_manager_set_callbacks(FocusManager *fm,
                                 const FocusManagerCallbacks *callbacks);

/* ============================================================================
 * Widget Registration
 * ============================================================================
 */

/* Add widget to focus group. Returns false if at capacity. */
bool focus_manager_add_widget(FocusManager *fm, Widget *widget);

/* Remove widget from focus group */
void focus_manager_remove_widget(FocusManager *fm, Widget *widget);

/* Check if widget is in focus group */
bool focus_manager_has_widget(FocusManager *fm, Widget *widget);

/* ============================================================================
 * Focus Management
 * ============================================================================
 */

/* Get the primary focused widget */
Widget *focus_manager_get_focus(const FocusManager *fm);

/* Set focus to a specific widget */
void focus_manager_set_focus(FocusManager *fm, Widget *widget);

/* Cycle focus to next widget (Tab key behavior) */
void focus_manager_cycle_next(FocusManager *fm);

/* Cycle focus to previous widget (Shift+Tab behavior) */
void focus_manager_cycle_prev(FocusManager *fm);

/* Clear focus (no widget has focus) */
void focus_manager_clear_focus(FocusManager *fm);

/* ============================================================================
 * Split-View Focus Stack
 * ============================================================================
 */

/* Push widget onto focus stack (for split-view with multiple focused widgets)
 */
bool focus_manager_push_focus(FocusManager *fm, Widget *widget);

/* Pop widget from focus stack */
Widget *focus_manager_pop_focus(FocusManager *fm);

/* Get focus stack size */
size_t focus_manager_stack_size(const FocusManager *fm);

/* Check if widget is in focus stack */
bool focus_manager_in_stack(const FocusManager *fm, Widget *widget);

/* ============================================================================
 * Event Routing
 * ============================================================================
 */

/* Route event to focused widget(s).
 * Returns true if event was consumed by a widget. */
bool focus_manager_route_event(FocusManager *fm, const UiEvent *event);

/* Route event to specific widget (bypasses focus) */
bool focus_manager_send_event(FocusManager *fm, Widget *widget,
                              const UiEvent *event);

/* ============================================================================
 * Focus Order
 * ============================================================================
 */

/* Set custom focus order for Tab cycling.
 * order[] contains indices into the widgets array. */
void focus_manager_set_order(FocusManager *fm, const size_t *order,
                             size_t count);

/* Reset focus order to widget registration order */
void focus_manager_reset_order(FocusManager *fm);

/* ============================================================================
 * Visibility Integration
 * ============================================================================
 */

/* Get next visible widget in focus order (skips hidden widgets) */
Widget *focus_manager_next_visible(FocusManager *fm);

/* Get previous visible widget in focus order (skips hidden widgets) */
Widget *focus_manager_prev_visible(FocusManager *fm);

/* Focus first visible widget */
void focus_manager_focus_first_visible(FocusManager *fm);

#endif /* LACE_FOCUS_MANAGER_H */
