/*
 * Lace
 * Application Configuration
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONFIG_CONFIG_H
#define LACE_CONFIG_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define CONFIG_FILE "config.json"

/* Validation limits */
#define CONFIG_PAGE_SIZE_MIN 10
#define CONFIG_PAGE_SIZE_MAX 10000
#define CONFIG_PAGE_SIZE_DEFAULT 500
#define CONFIG_PREFETCH_PAGES_MIN 1
#define CONFIG_PREFETCH_PAGES_MAX 10
#define CONFIG_PREFETCH_PAGES_DEFAULT 2
#define CONFIG_MAX_RESULT_ROWS_MIN 1000
#define CONFIG_MAX_RESULT_ROWS_MAX (10 * 1024 * 1024)  /* 10M rows */
#define CONFIG_MAX_RESULT_ROWS_DEFAULT (1024 * 1024)   /* 1M rows */

/* ============================================================================
 * Hotkey Categories - for conflict detection and UI grouping
 * ============================================================================
 */

typedef enum {
  HOTKEY_CAT_GENERAL,    /* Global keys (tabs, quit, help, config) */
  HOTKEY_CAT_NAVIGATION, /* Cursor movement */
  HOTKEY_CAT_TABLE,      /* Table viewer operations */
  HOTKEY_CAT_FILTERS,    /* Filter panel operations */
  HOTKEY_CAT_SIDEBAR,    /* Sidebar operations */
  HOTKEY_CAT_QUERY,      /* Query tab operations */
  HOTKEY_CAT_CONNECT,    /* Connection dialog operations */
  HOTKEY_CAT_COUNT
} HotkeyCategory;

/* ============================================================================
 * Hotkey Action Enum
 * ============================================================================
 */

typedef enum {
  /* Navigation (HOTKEY_CAT_NAVIGATION) */
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

  /* Table Viewer (HOTKEY_CAT_TABLE) */
  HOTKEY_EDIT_INLINE,
  HOTKEY_EDIT_MODAL,
  HOTKEY_SET_NULL,
  HOTKEY_SET_EMPTY,
  HOTKEY_DELETE_ROW,
  HOTKEY_TOGGLE_FILTERS,
  HOTKEY_TOGGLE_SIDEBAR,
  HOTKEY_SHOW_SCHEMA,
  HOTKEY_REFRESH,
  HOTKEY_CYCLE_SORT,

  /* General (HOTKEY_CAT_GENERAL) */
  HOTKEY_PREV_TAB,
  HOTKEY_NEXT_TAB,
  HOTKEY_CLOSE_TAB,
  HOTKEY_NEW_TAB,
  HOTKEY_PREV_WORKSPACE,
  HOTKEY_NEXT_WORKSPACE,
  HOTKEY_TOGGLE_HEADER,
  HOTKEY_TOGGLE_STATUS,
  HOTKEY_CONNECT_DIALOG,
  HOTKEY_HELP,
  HOTKEY_QUIT,
  HOTKEY_CONFIG,

  /* Query Tab (HOTKEY_CAT_QUERY) */
  HOTKEY_OPEN_QUERY,
  HOTKEY_EXECUTE_QUERY,
  HOTKEY_EXECUTE_ALL,
  HOTKEY_EXECUTE_TRANSACTION,
  HOTKEY_QUERY_SWITCH_FOCUS,

  /* Filters Panel (HOTKEY_CAT_FILTERS) */
  HOTKEY_ADD_FILTER,
  HOTKEY_REMOVE_FILTER,
  HOTKEY_CLEAR_FILTERS,
  HOTKEY_FILTERS_SWITCH_FOCUS,

  /* Sidebar (HOTKEY_CAT_SIDEBAR) */
  HOTKEY_SIDEBAR_FILTER,

  /* Connection Dialog (HOTKEY_CAT_CONNECT) */
  HOTKEY_CONN_TEST,
  HOTKEY_CONN_SAVE,
  HOTKEY_CONN_NEW,
  HOTKEY_CONN_NEW_FOLDER,
  HOTKEY_CONN_EDIT,
  HOTKEY_CONN_DELETE,
  HOTKEY_CONN_RENAME,

  HOTKEY_COUNT
} HotkeyAction;

/* ============================================================================
 * Configuration Types
 * ============================================================================
 */

/* General application settings */
typedef struct {
  bool show_header;
  bool show_status_bar;
  int page_size;
  int prefetch_pages;
  bool restore_session;
  bool quit_confirmation;
  int max_result_rows;           /* Maximum rows returned by raw SQL queries */
  bool auto_open_first_table;    /* Open first table instead of connection tab */
  bool close_conn_on_last_tab;   /* Close connection when last tab closes */
} GeneralConfig;

/* Single hotkey binding (key string like "k", "CTRL+W", "F1") */
typedef struct {
  char **keys;      /* Array of key strings */
  size_t num_keys;
} HotkeyBinding;

/* Full configuration */
typedef struct {
  GeneralConfig general;
  HotkeyBinding hotkeys[HOTKEY_COUNT];
} Config;

/* ============================================================================
 * Config API
 * ============================================================================
 */

/* Load config from disk (returns defaults if file doesn't exist) */
Config *config_load(char **error);

/* Save config to disk */
bool config_save(const Config *config, char **error);

/* Free config structure */
void config_free(Config *config);

/* Get default configuration */
Config *config_get_defaults(void);

/* Deep copy configuration */
Config *config_copy(const Config *config);

/* Reset a single hotkey to its default */
void config_reset_hotkey(Config *config, HotkeyAction action);

/* Reset all hotkeys to defaults */
void config_reset_all_hotkeys(Config *config);

/* Validate configuration (check for hotkey conflicts) */
bool config_validate(const Config *config, char **error);

/* Get config file path */
char *config_get_path(void);

/* ============================================================================
 * Hotkey API
 * ============================================================================
 */

/* Forward declaration for UiEvent (defined in backend.h) */
typedef struct UiEvent UiEvent;

/* Check if event matches action */
bool hotkey_matches(const Config *config, const UiEvent *event,
                    HotkeyAction action);

/* Get display string for action (e.g., "k, ↑") - caller frees */
char *hotkey_get_display(const Config *config, HotkeyAction action);

/* Get action name for display (e.g., "Move up") */
const char *hotkey_action_name(HotkeyAction action);

/* Get action key for JSON (e.g., "move_up") */
const char *hotkey_action_key(HotkeyAction action);

/* Find action by JSON key, returns ACTION_COUNT if not found */
HotkeyAction hotkey_action_from_key(const char *key);

/* Check for conflicts - returns conflicting action or ACTION_COUNT if none */
HotkeyAction hotkey_find_conflict(const Config *config, HotkeyAction action,
                                  const char *key);

/* Add a key to an action's bindings */
bool hotkey_add_key(Config *config, HotkeyAction action, const char *key);

/* Remove a key from an action's bindings */
bool hotkey_remove_key(Config *config, HotkeyAction action, size_t key_index);

/* Get default keys for an action - caller must free result */
char **hotkey_get_default_keys(HotkeyAction action, size_t *num_keys);

/* Get category for an action */
HotkeyCategory hotkey_get_category(HotkeyAction action);

/* Get category display name */
const char *hotkey_category_name(HotkeyCategory category);

/* Get first action in a category (for iteration) */
HotkeyAction hotkey_category_first(HotkeyCategory category);

/* Get action count in a category */
size_t hotkey_category_count(HotkeyCategory category);

#endif /* LACE_CONFIG_CONFIG_H */
