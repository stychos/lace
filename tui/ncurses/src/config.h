/*
 * Lace ncurses frontend
 * Configuration and hotkeys
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FRONTEND_CONFIG_H
#define LACE_FRONTEND_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* ==========================================================================
 * Hotkey Actions
 * ========================================================================== */

typedef enum {
  /* Navigation */
  HOTKEY_MOVE_UP,
  HOTKEY_MOVE_DOWN,
  HOTKEY_MOVE_LEFT,
  HOTKEY_MOVE_RIGHT,
  HOTKEY_PAGE_UP,
  HOTKEY_PAGE_DOWN,
  HOTKEY_FIRST_ROW,
  HOTKEY_LAST_ROW,
  HOTKEY_GOTO_ROW,
  HOTKEY_FIRST_COL,
  HOTKEY_LAST_COL,

  /* Table Viewer */
  HOTKEY_EDIT_INLINE,
  HOTKEY_EDIT_MODAL,
  HOTKEY_SET_NULL,
  HOTKEY_SET_EMPTY,
  HOTKEY_DELETE_ROW,
  HOTKEY_TOGGLE_FILTERS,
  HOTKEY_TOGGLE_SIDEBAR,
  HOTKEY_SHOW_SCHEMA,
  HOTKEY_REFRESH,

  /* General */
  HOTKEY_PREV_TAB,
  HOTKEY_NEXT_TAB,
  HOTKEY_CLOSE_TAB,
  HOTKEY_NEW_TAB,
  HOTKEY_HELP,
  HOTKEY_QUIT,

  /* Query */
  HOTKEY_OPEN_QUERY,
  HOTKEY_EXECUTE_QUERY,
  HOTKEY_EXECUTE_ALL,
  HOTKEY_QUERY_SWITCH_FOCUS,

  /* Filters */
  HOTKEY_ADD_FILTER,
  HOTKEY_REMOVE_FILTER,
  HOTKEY_CLEAR_FILTERS,

  /* Connection */
  HOTKEY_CONNECT,

  HOTKEY_COUNT
} HotkeyAction;

/* ==========================================================================
 * Hotkey Binding
 * ========================================================================== */

#define MAX_KEYS_PER_ACTION 4

typedef struct {
  int keys[MAX_KEYS_PER_ACTION];
  size_t num_keys;
} HotkeyBinding;

/* ==========================================================================
 * Configuration
 * ========================================================================== */

typedef struct {
  /* Hotkey bindings */
  HotkeyBinding hotkeys[HOTKEY_COUNT];

  /* Appearance */
  bool show_header;
  bool show_status;
  int sidebar_width;

  /* Behavior */
  bool confirm_quit;
  bool confirm_delete;

  /* Pagination */
  int page_size;

  /* Config file path */
  char *config_path;
} Config;

/* ==========================================================================
 * Configuration Functions
 * ========================================================================== */

/*
 * Create default configuration.
 */
Config *config_create(void);

/*
 * Free configuration.
 */
void config_free(Config *cfg);

/*
 * Load configuration from file.
 * Returns NULL on error.
 */
Config *config_load(const char *path);

/*
 * Save configuration to file.
 */
bool config_save(Config *cfg);

/*
 * Check if key matches hotkey action.
 */
bool hotkey_matches(Config *cfg, int key, HotkeyAction action);

/*
 * Get display string for hotkey (caller must free).
 */
char *hotkey_get_display(Config *cfg, HotkeyAction action);

/*
 * Get config directory path (caller must free).
 */
char *config_get_dir(void);

#endif /* LACE_FRONTEND_CONFIG_H */
