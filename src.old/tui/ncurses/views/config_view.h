/*
 * Lace
 * Configuration Editor dialog
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONFIG_VIEW_H
#define LACE_CONFIG_VIEW_H

#include "../tui.h"

/* Configuration dialog result */
typedef enum {
  CONFIG_RESULT_CANCELLED, /* User cancelled without saving */
  CONFIG_RESULT_SAVED,     /* Configuration was saved */
  CONFIG_RESULT_APPLIED    /* Configuration was applied (runtime change) */
} ConfigResult;

/* Starting tab for config dialog */
typedef enum {
  CONFIG_TAB_GENERAL,
  CONFIG_TAB_HOTKEYS
} ConfigStartTab;

/* Show configuration dialog
 * Returns result indicating what happened.
 * If saved, the config is automatically written to disk. */
ConfigResult config_view_show(TuiState *state);

/* Show configuration dialog starting on a specific tab */
ConfigResult config_view_show_tab(TuiState *state, ConfigStartTab start_tab);

#endif /* LACE_CONFIG_VIEW_H */
