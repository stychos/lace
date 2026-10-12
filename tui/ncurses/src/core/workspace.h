/*
 * Lace
 * Core Workspace/Tab Management (platform-independent)
 *
 * This module provides tab management and navigation that operates purely on
 * core state without any UI dependencies.
 *
 * Hierarchy: AppState → Connection → Workspace → Tab
 *
 * Note: Most lifecycle functions are declared in app_state.h.
 * This file contains navigation and pagination operations.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CORE_WORKSPACE_H
#define LACE_CORE_WORKSPACE_H

#include "app_state.h"
#include <stdbool.h>

/* ============================================================================
 * Tab Navigation Operations (UI-agnostic)
 * ============================================================================
 */

/* ============================================================================
 * Tab Pagination State (tracking only, no I/O)
 * ============================================================================
 */

/* ============================================================================
 * Compatibility Aliases (for gradual migration)
 * ============================================================================
 * These will be removed after all code is updated to use Tab-based functions.
 */

/* Old workspace_* functions now operate on Tab */
#define workspace_move_cursor(ws, rd, cd, vr)                                  \
  tab_move_cursor((ws), (rd), (cd), (vr))
#define workspace_page_up(ws, vr) tab_page_up((ws), (vr))
#define workspace_page_down(ws, vr) tab_page_down((ws), (vr))
#define workspace_home(ws) tab_home((ws))
#define workspace_end(ws, vr) tab_end((ws), (vr))
#define workspace_column_first(ws) tab_column_first((ws))
#define workspace_column_last(ws) tab_column_last((ws))
#define workspace_check_data_edge(ws, t) tab_check_data_edge((ws), (t))
#define workspace_has_more_data_forward(ws) tab_has_more_data_forward((ws))
#define workspace_has_more_data_backward(ws) tab_has_more_data_backward((ws))
#define workspace_update_pagination(ws, o, c, t)                               \
  tab_update_pagination((ws), (o), (c), (t))

#endif /* LACE_CORE_WORKSPACE_H */
