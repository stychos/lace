/*
 * Lace
 * Application Configuration
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config.h"
#include "../core/history.h"
#include "../core/ui_types.h"
#include "../platform/platform.h"
#include "../util/json_helpers.h"
#include "../util/str.h"
#include <cJSON.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef LACE_OS_WINDOWS
#include <fcntl.h>
#include <unistd.h>
#endif

/* ============================================================================
 * Action Metadata
 * ============================================================================
 */

typedef struct {
  const char *key;  /* JSON key (e.g., "move_up") */
  const char *name; /* Display name (e.g., "Move up") */
  HotkeyCategory category;
  const char **default_keys;
  size_t num_default_keys;
} ActionMeta;

/* Default key bindings */
/* Navigation */
static const char *def_move_up[] = {"k", "UP"};
static const char *def_move_down[] = {"j", "DOWN"};
static const char *def_move_left[] = {"h", "LEFT"};
static const char *def_move_right[] = {"l", "RIGHT"};
static const char *def_page_up[] = {"PGUP"};
static const char *def_page_down[] = {"PGDN"};
static const char *def_first_row[] = {"g", "a"};
static const char *def_last_row[] = {"G", "z"};
static const char *def_goto_row[] = {"CTRL+G", "F5"};
static const char *def_first_col[] = {"HOME"};
static const char *def_last_col[] = {"END"};

/* Table Viewer */
static const char *def_edit_inline[] = {"ENTER"};
static const char *def_edit_modal[] = {"e", "F4"};
static const char *def_set_null[] = {"n", "CTRL+N"};
static const char *def_set_empty[] = {"d", "CTRL+D"};
static const char *def_delete_row[] = {"x", "DELETE"};
static const char *def_toggle_filters[] = {"f", "/"};
static const char *def_toggle_sidebar[] = {"t", "F9"};
static const char *def_show_schema[] = {"s", "F3"};
static const char *def_refresh[] = {"r"};
static const char *def_cycle_sort[] = {"o"};
static const char *def_cell_copy[] = {"c", "CTRL+K"};
static const char *def_cell_paste[] = {"v", "CTRL+U"};

/* General */
static const char *def_prev_tab[] = {"[", "F7"};
static const char *def_next_tab[] = {"]", "F6"};
static const char *def_close_tab[] = {"-"};
static const char *def_new_tab[] = {"+", "=", "INSERT"};
static const char *def_prev_workspace[] = {"{"};
static const char *def_next_workspace[] = {"}"};
static const char *def_toggle_header[] = {"m"};
static const char *def_toggle_status[] = {"b"};
static const char *def_connect_dialog[] = {"w", "F2"};
static const char *def_help[] = {"?", "F1"};
static const char *def_quit[] = {"q", "CTRL+X", "F10"};
static const char *def_config[] = {"COMMA", "F11"};
static const char *def_toggle_history[] = {"i"};
static const char *def_cut_line[] = {"CTRL+K"};
static const char *def_paste[] = {"CTRL+U"};

/* Query Tab */
static const char *def_open_query[] = {"p"};
static const char *def_execute_query[] = {"CTRL+R"};
static const char *def_execute_all[] = {"CTRL+A"};
static const char *def_execute_transaction[] = {"CTRL+T"};
static const char *def_query_switch_focus[] = {"CTRL+W", "ESCAPE"};

/* Filters Panel */
static const char *def_add_filter[] = {"+", "=", "INSERT"};
static const char *def_remove_filter[] = {"-", "x", "DELETE"};
static const char *def_clear_filters[] = {"X"}; /* Shift+X to clear all */
static const char *def_filters_switch_focus[] = {"CTRL+W", "ESCAPE"};

/* Sidebar */
static const char *def_sidebar_filter[] = {"/", "f"};

/* Connection Dialog */
static const char *def_conn_test[] = {"CTRL+T"};
static const char *def_conn_save[] = {"CTRL+S"};
static const char *def_conn_new[] = {"n"};
static const char *def_conn_new_folder[] = {"N"};
static const char *def_conn_edit[] = {"e"};
static const char *def_conn_delete[] = {"x", "DELETE"};
static const char *def_conn_rename[] = {"r"};

/* Row Selection */
static const char *def_toggle_selection[] = {"SPACE"};
static const char *def_clear_selections[] = {"ESCAPE"};

/* Row Add */
static const char *def_row_add[] = {"+", "=", "INSERT"};
static const char *def_row_save[] = {"F2"};

/* Connection Move */
static const char *def_conn_move[] = {"SPACE"};

/* Modal Editor */
static const char *def_editor_save[] = {"F2"};
static const char *def_editor_null[] = {"CTRL+N"};
static const char *def_editor_empty[] = {"CTRL+D"};
static const char *def_editor_cancel[] = {"ESCAPE"};

/* Config Editor */
static const char *def_config_reset[] = {"r"};
static const char *def_config_reset_all[] = {"R"};

/* History Dialog */
static const char *def_history_copy[] = {"ENTER"};
static const char *def_history_delete[] = {"x", "DELETE"};
static const char *def_history_clear[] = {"c"};
static const char *def_history_close[] = {"ESCAPE", "q"};

#define DEF_KEYS(arr) arr, sizeof(arr) / sizeof(arr[0])

static const ActionMeta action_meta[HOTKEY_COUNT] = {
    /* Navigation */
    [HOTKEY_MOVE_UP] = {"move_up", "Move up", HOTKEY_CAT_NAVIGATION,
                        DEF_KEYS(def_move_up)},
    [HOTKEY_MOVE_DOWN] = {"move_down", "Move down", HOTKEY_CAT_NAVIGATION,
                          DEF_KEYS(def_move_down)},
    [HOTKEY_MOVE_LEFT] = {"move_left", "Move left", HOTKEY_CAT_NAVIGATION,
                          DEF_KEYS(def_move_left)},
    [HOTKEY_MOVE_RIGHT] = {"move_right", "Move right", HOTKEY_CAT_NAVIGATION,
                           DEF_KEYS(def_move_right)},
    [HOTKEY_PAGE_UP] = {"page_up", "Page up", HOTKEY_CAT_NAVIGATION,
                        DEF_KEYS(def_page_up)},
    [HOTKEY_PAGE_DOWN] = {"page_down", "Page down", HOTKEY_CAT_NAVIGATION,
                          DEF_KEYS(def_page_down)},
    [HOTKEY_FIRST_ROW] = {"first_row", "First row", HOTKEY_CAT_NAVIGATION,
                          DEF_KEYS(def_first_row)},
    [HOTKEY_LAST_ROW] = {"last_row", "Last row", HOTKEY_CAT_NAVIGATION,
                         DEF_KEYS(def_last_row)},
    [HOTKEY_GOTO_ROW] = {"goto_row", "Go to row", HOTKEY_CAT_NAVIGATION,
                         DEF_KEYS(def_goto_row)},
    [HOTKEY_FIRST_COL] = {"first_col", "First column", HOTKEY_CAT_NAVIGATION,
                          DEF_KEYS(def_first_col)},
    [HOTKEY_LAST_COL] = {"last_col", "Last column", HOTKEY_CAT_NAVIGATION,
                         DEF_KEYS(def_last_col)},

    /* Table Viewer */
    [HOTKEY_EDIT_INLINE] = {"edit_inline", "Edit inline", HOTKEY_CAT_TABLE,
                            DEF_KEYS(def_edit_inline)},
    [HOTKEY_EDIT_MODAL] = {"edit_modal", "Edit modal", HOTKEY_CAT_TABLE,
                           DEF_KEYS(def_edit_modal)},
    [HOTKEY_SET_NULL] = {"set_null", "Set NULL", HOTKEY_CAT_TABLE,
                         DEF_KEYS(def_set_null)},
    [HOTKEY_SET_EMPTY] = {"set_empty", "Set empty", HOTKEY_CAT_TABLE,
                          DEF_KEYS(def_set_empty)},
    [HOTKEY_DELETE_ROW] = {"delete_row", "Delete row", HOTKEY_CAT_TABLE,
                           DEF_KEYS(def_delete_row)},
    [HOTKEY_TOGGLE_FILTERS] = {"toggle_filters", "Toggle filters",
                               HOTKEY_CAT_TABLE, DEF_KEYS(def_toggle_filters)},
    [HOTKEY_TOGGLE_SIDEBAR] = {"toggle_sidebar", "Toggle sidebar",
                               HOTKEY_CAT_TABLE, DEF_KEYS(def_toggle_sidebar)},
    [HOTKEY_SHOW_SCHEMA] = {"show_schema", "Show schema", HOTKEY_CAT_TABLE,
                            DEF_KEYS(def_show_schema)},
    [HOTKEY_REFRESH] = {"refresh", "Refresh", HOTKEY_CAT_TABLE,
                        DEF_KEYS(def_refresh)},
    [HOTKEY_CYCLE_SORT] = {"cycle_sort", "Cycle sort", HOTKEY_CAT_TABLE,
                           DEF_KEYS(def_cycle_sort)},
    [HOTKEY_CELL_COPY] = {"cell_copy", "Copy cell", HOTKEY_CAT_TABLE,
                          DEF_KEYS(def_cell_copy)},
    [HOTKEY_CELL_PASTE] = {"cell_paste", "Paste cell", HOTKEY_CAT_TABLE,
                           DEF_KEYS(def_cell_paste)},

    /* General */
    [HOTKEY_PREV_TAB] = {"prev_tab", "Previous tab", HOTKEY_CAT_GENERAL,
                         DEF_KEYS(def_prev_tab)},
    [HOTKEY_NEXT_TAB] = {"next_tab", "Next tab", HOTKEY_CAT_GENERAL,
                         DEF_KEYS(def_next_tab)},
    [HOTKEY_CLOSE_TAB] = {"close_tab", "Close tab", HOTKEY_CAT_GENERAL,
                          DEF_KEYS(def_close_tab)},
    [HOTKEY_NEW_TAB] = {"new_tab", "New tab", HOTKEY_CAT_GENERAL,
                        DEF_KEYS(def_new_tab)},
    [HOTKEY_PREV_WORKSPACE] = {"prev_workspace", "Previous workspace",
                               HOTKEY_CAT_GENERAL,
                               DEF_KEYS(def_prev_workspace)},
    [HOTKEY_NEXT_WORKSPACE] = {"next_workspace", "Next workspace",
                               HOTKEY_CAT_GENERAL,
                               DEF_KEYS(def_next_workspace)},
    [HOTKEY_TOGGLE_HEADER] = {"toggle_header", "Toggle header",
                              HOTKEY_CAT_GENERAL, DEF_KEYS(def_toggle_header)},
    [HOTKEY_TOGGLE_STATUS] = {"toggle_status", "Toggle status bar",
                              HOTKEY_CAT_GENERAL, DEF_KEYS(def_toggle_status)},
    [HOTKEY_CONNECT_DIALOG] = {"connect_dialog", "Connect dialog",
                               HOTKEY_CAT_GENERAL,
                               DEF_KEYS(def_connect_dialog)},
    [HOTKEY_TOGGLE_HISTORY] = {"toggle_history", "Query history",
                               HOTKEY_CAT_GENERAL,
                               DEF_KEYS(def_toggle_history)},
    [HOTKEY_HELP] = {"help", "Hotkeys", HOTKEY_CAT_GENERAL, DEF_KEYS(def_help)},
    [HOTKEY_QUIT] = {"quit", "Quit", HOTKEY_CAT_GENERAL, DEF_KEYS(def_quit)},
    [HOTKEY_CONFIG] = {"config", "Configuration", HOTKEY_CAT_GENERAL,
                       DEF_KEYS(def_config)},
    [HOTKEY_CUT_LINE] = {"cut_line", "Cut line to buffer", HOTKEY_CAT_GENERAL,
                         DEF_KEYS(def_cut_line)},
    [HOTKEY_PASTE] = {"paste", "Paste from buffer", HOTKEY_CAT_GENERAL,
                      DEF_KEYS(def_paste)},

    /* Query Tab */
    [HOTKEY_OPEN_QUERY] = {"open_query", "Open query tab", HOTKEY_CAT_QUERY,
                           DEF_KEYS(def_open_query)},
    [HOTKEY_EXECUTE_QUERY] = {"execute_query", "Execute query",
                              HOTKEY_CAT_QUERY, DEF_KEYS(def_execute_query)},
    [HOTKEY_EXECUTE_ALL] = {"execute_all", "Execute all", HOTKEY_CAT_QUERY,
                            DEF_KEYS(def_execute_all)},
    [HOTKEY_EXECUTE_TRANSACTION] = {"execute_transaction",
                                    "Execute in transaction", HOTKEY_CAT_QUERY,
                                    DEF_KEYS(def_execute_transaction)},
    [HOTKEY_QUERY_SWITCH_FOCUS] = {"query_switch_focus",
                                   "Switch editor/results", HOTKEY_CAT_QUERY,
                                   DEF_KEYS(def_query_switch_focus)},

    /* Filters Panel */
    [HOTKEY_ADD_FILTER] = {"add_filter", "Add filter", HOTKEY_CAT_FILTERS,
                           DEF_KEYS(def_add_filter)},
    [HOTKEY_REMOVE_FILTER] = {"remove_filter", "Remove filter",
                              HOTKEY_CAT_FILTERS, DEF_KEYS(def_remove_filter)},
    [HOTKEY_CLEAR_FILTERS] = {"clear_filters", "Clear filters",
                              HOTKEY_CAT_FILTERS, DEF_KEYS(def_clear_filters)},
    [HOTKEY_FILTERS_SWITCH_FOCUS] = {"filters_switch_focus", "Switch to table",
                                     HOTKEY_CAT_FILTERS,
                                     DEF_KEYS(def_filters_switch_focus)},

    /* Sidebar */
    [HOTKEY_SIDEBAR_FILTER] = {"sidebar_filter", "Filter tables",
                               HOTKEY_CAT_SIDEBAR,
                               DEF_KEYS(def_sidebar_filter)},

    /* Connection Dialog */
    [HOTKEY_CONN_TEST] = {"conn_test", "Test connection", HOTKEY_CAT_CONNECT,
                          DEF_KEYS(def_conn_test)},
    [HOTKEY_CONN_SAVE] = {"conn_save", "Save to list", HOTKEY_CAT_CONNECT,
                          DEF_KEYS(def_conn_save)},
    [HOTKEY_CONN_NEW] = {"conn_new", "New connection", HOTKEY_CAT_CONNECT,
                         DEF_KEYS(def_conn_new)},
    [HOTKEY_CONN_NEW_FOLDER] = {"conn_new_folder", "New folder",
                                HOTKEY_CAT_CONNECT,
                                DEF_KEYS(def_conn_new_folder)},
    [HOTKEY_CONN_EDIT] = {"conn_edit", "Edit", HOTKEY_CAT_CONNECT,
                          DEF_KEYS(def_conn_edit)},
    [HOTKEY_CONN_DELETE] = {"conn_delete", "Delete", HOTKEY_CAT_CONNECT,
                            DEF_KEYS(def_conn_delete)},
    [HOTKEY_CONN_RENAME] = {"conn_rename", "Rename", HOTKEY_CAT_CONNECT,
                            DEF_KEYS(def_conn_rename)},
    [HOTKEY_CONN_MOVE] = {"conn_move", "Move item", HOTKEY_CAT_CONNECT,
                          DEF_KEYS(def_conn_move)},

    /* Row Selection (Table category) */
    [HOTKEY_TOGGLE_SELECTION] = {"toggle_selection", "Toggle selection",
                                 HOTKEY_CAT_TABLE,
                                 DEF_KEYS(def_toggle_selection)},
    [HOTKEY_CLEAR_SELECTIONS] = {"clear_selections", "Clear selections",
                                 HOTKEY_CAT_TABLE,
                                 DEF_KEYS(def_clear_selections)},

    /* Row Add (Table category) */
    [HOTKEY_ROW_ADD] = {"row_add", "Add new row", HOTKEY_CAT_TABLE,
                        DEF_KEYS(def_row_add)},
    [HOTKEY_ROW_SAVE] = {"row_save", "Save new row", HOTKEY_CAT_TABLE,
                         DEF_KEYS(def_row_save)},

    /* Modal Editor */
    [HOTKEY_EDITOR_SAVE] = {"editor_save", "Save", HOTKEY_CAT_EDITOR,
                            DEF_KEYS(def_editor_save)},
    [HOTKEY_EDITOR_NULL] = {"editor_null", "Set NULL", HOTKEY_CAT_EDITOR,
                            DEF_KEYS(def_editor_null)},
    [HOTKEY_EDITOR_EMPTY] = {"editor_empty", "Set empty", HOTKEY_CAT_EDITOR,
                             DEF_KEYS(def_editor_empty)},
    [HOTKEY_EDITOR_CANCEL] = {"editor_cancel", "Cancel", HOTKEY_CAT_EDITOR,
                              DEF_KEYS(def_editor_cancel)},

    /* Config Editor */
    [HOTKEY_CONFIG_RESET] = {"config_reset", "Reset hotkey", HOTKEY_CAT_GENERAL,
                             DEF_KEYS(def_config_reset)},
    [HOTKEY_CONFIG_RESET_ALL] = {"config_reset_all", "Reset all hotkeys",
                                 HOTKEY_CAT_GENERAL,
                                 DEF_KEYS(def_config_reset_all)},

    /* History Dialog */
    [HOTKEY_HISTORY_COPY] = {"history_copy", "Copy to clipboard",
                             HOTKEY_CAT_HISTORY, DEF_KEYS(def_history_copy)},
    [HOTKEY_HISTORY_DELETE] = {"history_delete", "Delete entry",
                               HOTKEY_CAT_HISTORY,
                               DEF_KEYS(def_history_delete)},
    [HOTKEY_HISTORY_CLEAR] = {"history_clear", "Clear all", HOTKEY_CAT_HISTORY,
                              DEF_KEYS(def_history_clear)},
    [HOTKEY_HISTORY_CLOSE] = {"history_close", "Close dialog",
                              HOTKEY_CAT_HISTORY, DEF_KEYS(def_history_close)},
};

/* Category names for display */
static const char *category_names[HOTKEY_CAT_COUNT] = {
    [HOTKEY_CAT_GENERAL] = "General",
    [HOTKEY_CAT_NAVIGATION] = "Navigation",
    [HOTKEY_CAT_TABLE] = "Table Viewer",
    [HOTKEY_CAT_FILTERS] = "Filters Panel",
    [HOTKEY_CAT_SIDEBAR] = "Sidebar",
    [HOTKEY_CAT_QUERY] = "Query Tab",
    [HOTKEY_CAT_CONNECT] = "Connect Dialog",
    [HOTKEY_CAT_EDITOR] = "Modal Editor",
    [HOTKEY_CAT_HISTORY] = "Query History",
};

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

char *config_get_path(void) {
  const char *config_dir = platform_get_config_dir();
  if (!config_dir)
    return NULL;
  return str_printf("%s%s%s", config_dir, LACE_PATH_SEP_STR, CONFIG_FILE);
}

static void hotkey_binding_free(HotkeyBinding *binding) {
  if (!binding)
    return;
  for (size_t i = 0; i < binding->num_keys; i++) {
    free(binding->keys[i]);
  }
  free(binding->keys);
  binding->keys = NULL;
  binding->num_keys = 0;
}

static bool hotkey_binding_copy(HotkeyBinding *dst, const HotkeyBinding *src) {
  if (!src || src->num_keys == 0) {
    dst->keys = NULL;
    dst->num_keys = 0;
    return true;
  }

  dst->keys = calloc(src->num_keys, sizeof(char *));
  if (!dst->keys)
    return false;

  for (size_t i = 0; i < src->num_keys; i++) {
    dst->keys[i] = str_dup(src->keys[i]);
  }
  dst->num_keys = src->num_keys;
  return true;
}

/* ============================================================================
 * Key String Parsing
 * ============================================================================
 */

/* Key name to code lookup table entry */
typedef struct {
  const char *name;
  int code;
} KeyNameEntry;

/* Static lookup table for special key names */
static const KeyNameEntry key_name_table[] = {
    {"UP", UI_KEY_UP},
    {"DOWN", UI_KEY_DOWN},
    {"LEFT", UI_KEY_LEFT},
    {"RIGHT", UI_KEY_RIGHT},
    {"PGUP", UI_KEY_PAGEUP},
    {"PGDN", UI_KEY_PAGEDOWN},
    {"HOME", UI_KEY_HOME},
    {"END", UI_KEY_END},
    {"ENTER", UI_KEY_ENTER},
    {"ESCAPE", UI_KEY_ESCAPE},
    {"DELETE", UI_KEY_DELETE},
    {"INSERT", UI_KEY_INSERT},
    {"BACKSPACE", UI_KEY_BACKSPACE},
    {"TAB", UI_KEY_TAB},
    {"SPACE", ' '},
    {"COMMA", ','},
};
#define KEY_NAME_TABLE_SIZE (sizeof(key_name_table) / sizeof(key_name_table[0]))

/* Lookup key code by name - returns -1 if not found */
static int lookup_key_code(const char *name) {
  for (size_t i = 0; i < KEY_NAME_TABLE_SIZE; i++) {
    if (strcmp(name, key_name_table[i].name) == 0) {
      return key_name_table[i].code;
    }
  }
  return -1;
}

/* Parse a key string like "k", "CTRL+A", "F5", "UP" into key code and mods */
static bool parse_key_string(const char *str, int *key_code, UiKeyMod *mods) {
  if (!str || !str[0])
    return false;

  *mods = UI_MOD_NONE;
  *key_code = 0;

  /* Check for CTRL+ prefix */
  if (strncmp(str, "CTRL+", 5) == 0) {
    *mods |= UI_MOD_CTRL;
    str += 5;
  }

  /* Try lookup table first */
  int code = lookup_key_code(str);
  if (code >= 0) {
    *key_code = code;
    return true;
  }

  /* Function keys F1-F12 */
  if (str[0] == 'F' && str[1] >= '1' && str[1] <= '9') {
    int fnum = atoi(str + 1);
    if (fnum >= 1 && fnum <= 12) {
      *key_code = UI_KEY_F1 + (fnum - 1);
      return true;
    }
    return false;
  }

  /* Single character */
  if (strlen(str) == 1) {
    char c = str[0];
    /* Convert to uppercase for CTRL combinations */
    if ((*mods & UI_MOD_CTRL) && c >= 'a' && c <= 'z') {
      c = (char)(c - ('a' - 'A'));
    }
    *key_code = (unsigned char)c;
    return true;
  }

  return false;
}

/* Key code to display name lookup table entry */
typedef struct {
  int code;
  const char *display;
} KeyDisplayEntry;

/* Static lookup table for key display names */
static const KeyDisplayEntry key_display_table[] = {
    {UI_KEY_UP, "\xe2\x86\x91"},         /* ↑ */
    {UI_KEY_DOWN, "\xe2\x86\x93"},       /* ↓ */
    {UI_KEY_LEFT, "\xe2\x86\x90"},       /* ← */
    {UI_KEY_RIGHT, "\xe2\x86\x92"},      /* → */
    {UI_KEY_PAGEUP, "PgUp"},
    {UI_KEY_PAGEDOWN, "PgDn"},
    {UI_KEY_HOME, "Home"},
    {UI_KEY_END, "End"},
    {UI_KEY_ENTER, "Enter"},
    {UI_KEY_ESCAPE, "Esc"},
    {UI_KEY_DELETE, "Del"},
    {UI_KEY_INSERT, "Ins"},
    {UI_KEY_BACKSPACE, "Bksp"},
    {UI_KEY_TAB, "Tab"},
    {UI_KEY_F1, "F1"},
    {UI_KEY_F2, "F2"},
    {UI_KEY_F3, "F3"},
    {UI_KEY_F4, "F4"},
    {UI_KEY_F5, "F5"},
    {UI_KEY_F6, "F6"},
    {UI_KEY_F7, "F7"},
    {UI_KEY_F8, "F8"},
    {UI_KEY_F9, "F9"},
    {UI_KEY_F10, "F10"},
    {UI_KEY_F11, "F11"},
    {UI_KEY_F12, "F12"},
    {' ', "Space"},
    {',', ","},
};
#define KEY_DISPLAY_TABLE_SIZE                                                 \
  (sizeof(key_display_table) / sizeof(key_display_table[0]))

/* Lookup display name by key code - returns NULL if not found */
static const char *lookup_key_display(int code) {
  for (size_t i = 0; i < KEY_DISPLAY_TABLE_SIZE; i++) {
    if (key_display_table[i].code == code) {
      return key_display_table[i].display;
    }
  }
  return NULL;
}

/* Convert key code and mods to display string */
static char *key_to_display(int key_code, UiKeyMod mods) {
  char prefix[16] = "";
  if (mods & UI_MOD_CTRL)
    snprintf(prefix, sizeof(prefix), "Ctrl+");

  /* Try lookup table first */
  const char *name = lookup_key_display(key_code);
  if (name) {
    return str_printf("%s%s", prefix, name);
  }

  /* Regular character */
  return str_printf("%s%c", prefix, key_code);
}

/* ============================================================================
 * Default Configuration
 * ============================================================================
 */

Config *config_get_defaults(void) {
  Config *config = calloc(1, sizeof(Config));
  if (!config)
    return NULL;

  /* General settings */
  config->general.show_header = true;
  config->general.show_status_bar = true;
  config->general.page_size = CONFIG_PAGE_SIZE_DEFAULT;
  config->general.prefetch_pages = CONFIG_PREFETCH_PAGES_DEFAULT;
  config->general.restore_session = true;
  config->general.quit_confirmation = false;
  config->general.delete_confirmation = true; /* Default: ask before delete */
  config->general.max_result_rows = CONFIG_MAX_RESULT_ROWS_DEFAULT;
  config->general.auto_open_first_table = false;
  config->general.close_conn_on_last_tab = false;
  config->general.history_mode =
      HISTORY_MODE_SESSION; /* Default: session only */
  config->general.history_max_size = HISTORY_SIZE_DEFAULT;

  /* Hotkeys - copy from defaults */
  for (int i = 0; i < HOTKEY_COUNT; i++) {
    const ActionMeta *meta = &action_meta[i];
    HotkeyBinding *binding = &config->hotkeys[i];

    if (meta->num_default_keys > 0) {
      binding->keys = calloc(meta->num_default_keys, sizeof(char *));
      if (binding->keys) {
        for (size_t j = 0; j < meta->num_default_keys; j++) {
          binding->keys[j] = str_dup(meta->default_keys[j]);
        }
        binding->num_keys = meta->num_default_keys;
      }
    }
  }

  return config;
}

/* ============================================================================
 * Config Free
 * ============================================================================
 */

void config_free(Config *config) {
  if (!config)
    return;

  for (int i = 0; i < HOTKEY_COUNT; i++) {
    hotkey_binding_free(&config->hotkeys[i]);
  }

  free(config);
}

/* ============================================================================
 * Config Copy
 * ============================================================================
 */

Config *config_copy(const Config *config) {
  if (!config)
    return NULL;

  Config *copy = calloc(1, sizeof(Config));
  if (!copy)
    return NULL;

  copy->general = config->general;

  for (int i = 0; i < HOTKEY_COUNT; i++) {
    if (!hotkey_binding_copy(&copy->hotkeys[i], &config->hotkeys[i])) {
      config_free(copy);
      return NULL;
    }
  }

  return copy;
}

/* ============================================================================
 * JSON Serialization
 * ============================================================================
 */

static cJSON *serialize_hotkeys(const Config *config) {
  cJSON *obj = cJSON_CreateObject();
  if (!obj)
    return NULL;

  for (int i = 0; i < HOTKEY_COUNT; i++) {
    const HotkeyBinding *binding = &config->hotkeys[i];
    const char *key = action_meta[i].key;

    cJSON *arr = cJSON_CreateArray();
    if (arr) {
      for (size_t j = 0; j < binding->num_keys; j++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(binding->keys[j]));
      }
      cJSON_AddItemToObject(obj, key, arr);
    }
  }

  return obj;
}

static bool parse_hotkeys(cJSON *json, Config *config) {
  if (!cJSON_IsObject(json))
    return false;

  cJSON *item;
  cJSON_ArrayForEach(item, json) {
    const char *key = item->string;
    HotkeyAction action = hotkey_action_from_key(key);

    if (action == HOTKEY_COUNT)
      continue; /* Unknown action, skip */

    if (!cJSON_IsArray(item))
      continue;

    HotkeyBinding *binding = &config->hotkeys[action];
    hotkey_binding_free(binding);

    int count = json_array_size(item);
    if (count > 0) {
      binding->keys = calloc((size_t)count, sizeof(char *));
      if (binding->keys) {
        for (int i = 0; i < count; i++) {
          if (binding->num_keys >= (size_t)count)
            break; /* Safety check: don't exceed allocated capacity */
          cJSON *key_item = json_get_array_item(item, i);
          if (key_item && cJSON_IsString(key_item)) {
            binding->keys[binding->num_keys] = str_dup(key_item->valuestring);
            binding->num_keys++;
          }
        }
      }
    }
  }

  return true;
}

/* ============================================================================
 * Config Load
 * ============================================================================
 */

Config *config_load(char **error) {
  char *path = config_get_path();
  if (!path) {
    /* No config directory, return defaults */
    return config_get_defaults();
  }

  if (!platform_file_exists(path)) {
    /* No config file, return defaults */
    free(path);
    return config_get_defaults();
  }

  FILE *f = fopen(path, "r");
  if (!f) {
    err_setf(error, "Failed to open %s: %s", path, strerror(errno));
    free(path);
    return config_get_defaults();
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0 || size > 1024 * 1024) { /* Max 1MB */
    fclose(f);
    err_setf(error, "Invalid config file size");
    free(path);
    return config_get_defaults();
  }

  char *content = malloc((size_t)size + 1);
  if (!content) {
    fclose(f);
    err_setf(error, "Out of memory");
    free(path);
    return config_get_defaults();
  }

  size_t read_bytes = fread(content, 1, (size_t)size, f);
  fclose(f);
  content[read_bytes] = '\0';
  free(path);

  cJSON *json = cJSON_Parse(content);
  free(content);

  if (!json) {
    err_setf(error, "JSON parse error");
    return config_get_defaults();
  }

  /* Start with defaults */
  Config *config = config_get_defaults();
  if (!config) {
    cJSON_Delete(json);
    err_setf(error, "Out of memory");
    return NULL;
  }

  /* Parse general settings */
  cJSON *general = json_get_object(json, "general");
  if (general) {
    /* Boolean settings - use defaults as fallback */
    config->general.show_header =
        json_get_bool(general, "show_header", config->general.show_header);
    config->general.show_status_bar =
        json_get_bool(general, "show_status_bar", config->general.show_status_bar);
    config->general.restore_session =
        json_get_bool(general, "restore_session", config->general.restore_session);
    config->general.quit_confirmation =
        json_get_bool(general, "quit_confirmation", config->general.quit_confirmation);
    config->general.delete_confirmation =
        json_get_bool(general, "delete_confirmation", config->general.delete_confirmation);
    config->general.auto_open_first_table =
        json_get_bool(general, "auto_open_first_table", config->general.auto_open_first_table);
    config->general.close_conn_on_last_tab =
        json_get_bool(general, "close_conn_on_last_tab", config->general.close_conn_on_last_tab);

    /* Integer settings with range validation */
    int val = json_get_int(general, "page_size", config->general.page_size);
    if (val >= CONFIG_PAGE_SIZE_MIN && val <= CONFIG_PAGE_SIZE_MAX)
      config->general.page_size = val;

    val = json_get_int(general, "prefetch_pages", config->general.prefetch_pages);
    if (val >= CONFIG_PREFETCH_PAGES_MIN && val <= CONFIG_PREFETCH_PAGES_MAX)
      config->general.prefetch_pages = val;

    val = json_get_int(general, "max_result_rows", config->general.max_result_rows);
    if (val >= CONFIG_MAX_RESULT_ROWS_MIN && val <= CONFIG_MAX_RESULT_ROWS_MAX)
      config->general.max_result_rows = val;

    val = json_get_int(general, "history_mode", config->general.history_mode);
    if (val >= HISTORY_MODE_OFF && val <= HISTORY_MODE_PERSISTENT)
      config->general.history_mode = val;

    val = json_get_int(general, "history_max_size", config->general.history_max_size);
    if (val >= HISTORY_SIZE_MIN && val <= HISTORY_SIZE_MAX)
      config->general.history_max_size = val;
  }

  /* Parse hotkeys */
  cJSON *hotkeys = json_get_object(json, "hotkeys");
  if (hotkeys) {
    parse_hotkeys(hotkeys, config);
  }

  cJSON_Delete(json);
  return config;
}

/* ============================================================================
 * Config Save
 * ============================================================================
 */

bool config_save(const Config *config, char **error) {
  if (!config) {
    err_setf(error, "Invalid config");
    return false;
  }

  /* Validate before saving */
  char *validate_err = NULL;
  if (!config_validate(config, &validate_err)) {
    if (error)
      *error = validate_err;
    else
      free(validate_err);
    return false;
  }

  const char *config_dir = platform_get_config_dir();
  if (!config_dir) {
    err_setf(error, "Failed to get config directory");
    return false;
  }

  if (!platform_dir_exists(config_dir)) {
    if (!platform_mkdir(config_dir)) {
      err_setf(error, "Failed to create config directory");
      return false;
    }
  }

  cJSON *json = cJSON_CreateObject();
  if (!json) {
    err_setf(error, "Out of memory");
    return false;
  }

  /* General settings */
  cJSON *general = cJSON_CreateObject();
  JSON_ADD_BOOL(general, "show_header", config->general.show_header);
  JSON_ADD_BOOL(general, "show_status_bar", config->general.show_status_bar);
  JSON_ADD_INT(general, "page_size", config->general.page_size);
  JSON_ADD_INT(general, "prefetch_pages", config->general.prefetch_pages);
  JSON_ADD_BOOL(general, "restore_session", config->general.restore_session);
  JSON_ADD_BOOL(general, "quit_confirmation", config->general.quit_confirmation);
  JSON_ADD_BOOL(general, "delete_confirmation", config->general.delete_confirmation);
  JSON_ADD_INT(general, "max_result_rows", config->general.max_result_rows);
  JSON_ADD_BOOL(general, "auto_open_first_table", config->general.auto_open_first_table);
  JSON_ADD_BOOL(general, "close_conn_on_last_tab", config->general.close_conn_on_last_tab);
  JSON_ADD_INT(general, "history_mode", config->general.history_mode);
  JSON_ADD_INT(general, "history_max_size", config->general.history_max_size);
  cJSON_AddItemToObject(json, "general", general);

  /* Hotkeys */
  cJSON *hotkeys = serialize_hotkeys(config);
  if (hotkeys) {
    cJSON_AddItemToObject(json, "hotkeys", hotkeys);
  }

  char *path = config_get_path();
  if (!path) {
    cJSON_Delete(json);
    err_setf(error, "Failed to get config path");
    return false;
  }

  char *content = cJSON_Print(json);
  cJSON_Delete(json);

  if (!content) {
    free(path);
    err_setf(error, "Failed to serialize JSON");
    return false;
  }

  /* Open file with secure permissions */
  FILE *f = NULL;
#ifndef LACE_OS_WINDOWS
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    free(content);
    err_setf(error, "Failed to open %s: %s", path, strerror(errno));
    free(path);
    return false;
  }
  f = fdopen(fd, "w");
  if (!f) {
    close(fd);
    free(content);
    err_setf(error, "Failed to open %s: %s", path, strerror(errno));
    free(path);
    return false;
  }
#else
  f = fopen(path, "w");
  if (!f) {
    free(content);
    err_setf(error, "Failed to open %s: %s", path, strerror(errno));
    free(path);
    return false;
  }
#endif

  size_t len = strlen(content);
  size_t written = fwrite(content, 1, len, f);
  fclose(f);
  free(content);
  free(path);

  if (written != len) {
    err_setf(error, "Failed to write all data");
    return false;
  }

  return true;
}

/* ============================================================================
 * Hotkey Reset
 * ============================================================================
 */

void config_reset_hotkey(Config *config, HotkeyAction action) {
  if (!config || action < 0 || action >= HOTKEY_COUNT)
    return;

  HotkeyBinding *binding = &config->hotkeys[action];
  hotkey_binding_free(binding);

  const ActionMeta *meta = &action_meta[action];
  if (meta->num_default_keys > 0) {
    binding->keys = calloc(meta->num_default_keys, sizeof(char *));
    if (binding->keys) {
      size_t copied = 0;
      for (size_t j = 0; j < meta->num_default_keys; j++) {
        binding->keys[j] = str_dup(meta->default_keys[j]);
        if (binding->keys[j]) {
          copied++;
        }
        /* On str_dup failure, key stays NULL (calloc zeroed) */
      }
      binding->num_keys = copied;
    }
  }
}

void config_reset_all_hotkeys(Config *config) {
  if (!config)
    return;
  for (int i = 0; i < HOTKEY_COUNT; i++) {
    config_reset_hotkey(config, (HotkeyAction)i);
  }
}

/* ============================================================================
 * Validation
 * ============================================================================
 */

bool config_validate(const Config *config, char **error) {
  if (!config) {
    err_setf(error, "Invalid config");
    return false;
  }

  /* Check for hotkey conflicts */
  for (int i = 0; i < HOTKEY_COUNT; i++) {
    const HotkeyBinding *binding = &config->hotkeys[i];
    for (size_t j = 0; j < binding->num_keys; j++) {
      HotkeyAction conflict =
          hotkey_find_conflict(config, (HotkeyAction)i, binding->keys[j]);
      if (conflict != HOTKEY_COUNT) {
        err_setf(error, "Conflict: '%s' is bound to both '%s' and '%s'",
                  binding->keys[j], action_meta[i].name,
                  action_meta[conflict].name);
        return false;
      }
    }
  }

  return true;
}

/* ============================================================================
 * Hotkey API
 * ============================================================================
 */

const char *hotkey_action_name(HotkeyAction action) {
  if (action < 0 || action >= HOTKEY_COUNT)
    return "Unknown";
  return action_meta[action].name;
}

const char *hotkey_action_key(HotkeyAction action) {
  if (action < 0 || action >= HOTKEY_COUNT)
    return NULL;
  return action_meta[action].key;
}

HotkeyAction hotkey_action_from_key(const char *key) {
  if (!key)
    return HOTKEY_COUNT;
  for (int i = 0; i < HOTKEY_COUNT; i++) {
    if (strcmp(action_meta[i].key, key) == 0) {
      return (HotkeyAction)i;
    }
  }
  return HOTKEY_COUNT;
}

bool hotkey_matches(const Config *config, const UiEvent *event,
                    HotkeyAction action) {
  if (!config || !event || action < 0 || action >= HOTKEY_COUNT)
    return false;

  if (event->type != UI_EVENT_KEY)
    return false;

  const HotkeyBinding *binding = &config->hotkeys[action];

  for (size_t i = 0; i < binding->num_keys; i++) {
    int key_code;
    UiKeyMod mods;
    if (!parse_key_string(binding->keys[i], &key_code, &mods))
      continue;

    /* Check if event matches */
    bool key_match = false;

    if (key_code >= UI_KEY_UP) {
      /* Special key */
      key_match = event->key.is_special && event->key.key == key_code;
    } else {
      /* Regular character */
      key_match = !event->key.is_special && event->key.key == key_code;
    }

    if (key_match) {
      /* Check modifiers */
      bool ctrl_match =
          ((mods & UI_MOD_CTRL) != 0) == ((event->key.mods & UI_MOD_CTRL) != 0);
      if (ctrl_match) {
        return true;
      }
    }
  }

  return false;
}

char *hotkey_get_display(const Config *config, HotkeyAction action) {
  if (!config || action < 0 || action >= HOTKEY_COUNT)
    return str_dup("");

  const HotkeyBinding *binding = &config->hotkeys[action];
  if (binding->num_keys == 0)
    return str_dup("");

  /* Build comma-separated display string */
  char *result = NULL;
  for (size_t i = 0; i < binding->num_keys; i++) {
    int key_code;
    UiKeyMod mods;
    if (!parse_key_string(binding->keys[i], &key_code, &mods))
      continue;

    char *key_str = key_to_display(key_code, mods);
    if (!key_str)
      continue;

    if (result) {
      char *new_result = str_printf("%s, %s", result, key_str);
      free(result);
      free(key_str);
      result = new_result;
    } else {
      result = key_str;
    }
  }

  return result ? result : str_dup("");
}

HotkeyAction hotkey_find_conflict(const Config *config, HotkeyAction action,
                                  const char *key) {
  if (!config || !key || action < 0 || action >= HOTKEY_COUNT)
    return HOTKEY_COUNT;

  int key_code;
  UiKeyMod mods;
  if (!parse_key_string(key, &key_code, &mods))
    return HOTKEY_COUNT;

  /* Only check for conflicts within the same category */
  HotkeyCategory category = action_meta[action].category;

  for (HotkeyAction i = 0; i < HOTKEY_COUNT; i++) {
    if (i == action)
      continue; /* Skip self */

    /* Skip if different category - same key allowed in different contexts */
    if (action_meta[i].category != category)
      continue;

    const HotkeyBinding *binding = &config->hotkeys[i];
    for (size_t j = 0; j < binding->num_keys; j++) {
      int other_code;
      UiKeyMod other_mods;
      if (!parse_key_string(binding->keys[j], &other_code, &other_mods))
        continue;

      if (key_code == other_code && mods == other_mods) {
        return i;
      }
    }
  }

  return HOTKEY_COUNT;
}

bool hotkey_add_key(Config *config, HotkeyAction action, const char *key) {
  if (!config || !key || action < 0 || action >= HOTKEY_COUNT)
    return false;

  HotkeyBinding *binding = &config->hotkeys[action];

  /* Validate key string */
  int key_code;
  UiKeyMod mods;
  if (!parse_key_string(key, &key_code, &mods))
    return false;

  /* Check if already exists */
  for (size_t i = 0; i < binding->num_keys; i++) {
    if (binding->keys[i] && strcmp(binding->keys[i], key) == 0)
      return true; /* Already exists */
  }

  /* Add new key - check for overflow before realloc */
  if (binding->num_keys > SIZE_MAX / sizeof(char *) - 1)
    return false;
  char **new_keys =
      realloc(binding->keys, (binding->num_keys + 1) * sizeof(char *));
  if (!new_keys)
    return false;

  binding->keys = new_keys;
  binding->keys[binding->num_keys] = str_dup(key);
  binding->num_keys++;
  return true;
}

bool hotkey_remove_key(Config *config, HotkeyAction action, size_t key_index) {
  if (!config || action < 0 || action >= HOTKEY_COUNT)
    return false;

  HotkeyBinding *binding = &config->hotkeys[action];
  if (key_index >= binding->num_keys)
    return false;

  free(binding->keys[key_index]);

  /* Shift remaining keys */
  for (size_t i = key_index; i < binding->num_keys - 1; i++) {
    binding->keys[i] = binding->keys[i + 1];
  }

  binding->num_keys--;
  return true;
}

char **hotkey_get_default_keys(HotkeyAction action, size_t *num_keys) {
  if (action < 0 || action >= HOTKEY_COUNT || !num_keys)
    return NULL;

  const ActionMeta *meta = &action_meta[action];
  *num_keys = meta->num_default_keys;

  if (meta->num_default_keys == 0)
    return NULL;

  char **keys = calloc(meta->num_default_keys, sizeof(char *));
  if (!keys)
    return NULL;

  for (size_t i = 0; i < meta->num_default_keys; i++) {
    keys[i] = str_dup(meta->default_keys[i]);
  }

  return keys;
}

/* ============================================================================
 * Category API
 * ============================================================================
 */

HotkeyCategory hotkey_get_category(HotkeyAction action) {
  if (action < 0 || action >= HOTKEY_COUNT)
    return HOTKEY_CAT_GENERAL;
  return action_meta[action].category;
}

const char *hotkey_category_name(HotkeyCategory category) {
  if (category < 0 || category >= HOTKEY_CAT_COUNT)
    return "Unknown";
  return category_names[category];
}

HotkeyAction hotkey_category_first(HotkeyCategory category) {
  for (HotkeyAction i = 0; i < HOTKEY_COUNT; i++) {
    if (action_meta[i].category == category)
      return i;
  }
  return HOTKEY_COUNT;
}

size_t hotkey_category_count(HotkeyCategory category) {
  size_t count = 0;
  for (HotkeyAction i = 0; i < HOTKEY_COUNT; i++) {
    if (action_meta[i].category == category)
      count++;
  }
  return count;
}
