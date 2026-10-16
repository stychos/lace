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
#include "util/json_helpers.h"
#include "../core/constants.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include <cjson/cJSON.h>
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
  unsigned frontends; /* CONFIG_FE_* mask */
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
static const char *def_query_focus_editor[] = {"ENTER"};
static const char *def_query_unfocus[] = {"ESCAPE"};

/* Filters Panel */
static const char *def_add_filter[] = {"+", "=", "INSERT"};
static const char *def_remove_filter[] = {"-", "x", "DELETE"};
static const char *def_clear_filters[] = {"X"}; /* Shift+X to clear all */

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
static const char *def_conn_duplicate[] = {"d"};
static const char *def_conn_filter[] = {"/"};

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

static const char *def_palette[] = {"CTRL+P"};
static const char *def_close_view[] = {"CTRL+W"};
static const char *def_next_conn_tab[] = {"CTRL+PGDN"};
static const char *def_prev_conn_tab[] = {"CTRL+PGUP"};
static const char *def_conn_tab_1[] = {"ALT+1"};
static const char *def_conn_tab_2[] = {"ALT+2"};
static const char *def_conn_tab_3[] = {"ALT+3"};
static const char *def_conn_tab_4[] = {"ALT+4"};
static const char *def_conn_tab_5[] = {"ALT+5"};
static const char *def_conn_tab_6[] = {"ALT+6"};
static const char *def_conn_tab_7[] = {"ALT+7"};
static const char *def_conn_tab_8[] = {"ALT+8"};
static const char *def_conn_tab_9[] = {"ALT+9"};
static const char *def_view_switcher[] = {"CTRL+E"};
static const char *def_toggle_log[] = {"CTRL+`", "F8"};
static const char *def_column_narrower[] = {"ALT+LEFT"};
static const char *def_column_wider[] = {"ALT+RIGHT"};
static const char *def_split_up[] = {"CTRL+UP"};
static const char *def_split_down[] = {"CTRL+DOWN"};

#define DEF_KEYS(arr) arr, sizeof(arr) / sizeof(arr[0])

static const ActionMeta action_meta[HOTKEY_COUNT] = {
    /* Navigation */
    [HOTKEY_MOVE_UP] = {"move_up", "Move up", HOTKEY_CAT_NAVIGATION,
                        DEF_KEYS(def_move_up), CONFIG_FE_ALL},
    [HOTKEY_MOVE_DOWN] = {"move_down", "Move down", HOTKEY_CAT_NAVIGATION,
                          DEF_KEYS(def_move_down), CONFIG_FE_ALL},
    [HOTKEY_MOVE_LEFT] = {"move_left", "Move left", HOTKEY_CAT_NAVIGATION,
                          DEF_KEYS(def_move_left), CONFIG_FE_ALL},
    [HOTKEY_MOVE_RIGHT] = {"move_right", "Move right", HOTKEY_CAT_NAVIGATION,
                           DEF_KEYS(def_move_right), CONFIG_FE_ALL},
    [HOTKEY_PAGE_UP] = {"page_up", "Page up", HOTKEY_CAT_NAVIGATION,
                        DEF_KEYS(def_page_up), CONFIG_FE_ALL},
    [HOTKEY_PAGE_DOWN] = {"page_down", "Page down", HOTKEY_CAT_NAVIGATION,
                          DEF_KEYS(def_page_down), CONFIG_FE_ALL},
    [HOTKEY_FIRST_ROW] = {"first_row", "First row", HOTKEY_CAT_NAVIGATION,
                          DEF_KEYS(def_first_row), CONFIG_FE_ALL},
    [HOTKEY_LAST_ROW] = {"last_row", "Last row", HOTKEY_CAT_NAVIGATION,
                         DEF_KEYS(def_last_row), CONFIG_FE_ALL},
    [HOTKEY_GOTO_ROW] = {"goto_row", "Go to row", HOTKEY_CAT_NAVIGATION,
                         DEF_KEYS(def_goto_row), CONFIG_FE_ALL},
    [HOTKEY_FIRST_COL] = {"first_col", "First column", HOTKEY_CAT_NAVIGATION,
                          DEF_KEYS(def_first_col), CONFIG_FE_ALL},
    [HOTKEY_LAST_COL] = {"last_col", "Last column", HOTKEY_CAT_NAVIGATION,
                         DEF_KEYS(def_last_col), CONFIG_FE_ALL},

    /* Table Viewer */
    [HOTKEY_EDIT_INLINE] = {"edit_inline", "Edit inline", HOTKEY_CAT_TABLE,
                            DEF_KEYS(def_edit_inline), CONFIG_FE_ALL},
    [HOTKEY_EDIT_MODAL] = {"edit_modal", "Edit modal", HOTKEY_CAT_TABLE,
                           DEF_KEYS(def_edit_modal), CONFIG_FE_ALL},
    [HOTKEY_SET_NULL] = {"set_null", "Set NULL", HOTKEY_CAT_TABLE,
                         DEF_KEYS(def_set_null), CONFIG_FE_ALL},
    [HOTKEY_SET_EMPTY] = {"set_empty", "Set empty", HOTKEY_CAT_TABLE,
                          DEF_KEYS(def_set_empty), CONFIG_FE_ALL},
    [HOTKEY_DELETE_ROW] = {"delete_row", "Delete row", HOTKEY_CAT_TABLE,
                           DEF_KEYS(def_delete_row), CONFIG_FE_ALL},
    [HOTKEY_TOGGLE_FILTERS] = {"toggle_filters", "Toggle filters",
                               HOTKEY_CAT_TABLE, DEF_KEYS(def_toggle_filters), CONFIG_FE_ALL},
    [HOTKEY_TOGGLE_SIDEBAR] = {"toggle_sidebar", "Toggle sidebar",
                               HOTKEY_CAT_TABLE, DEF_KEYS(def_toggle_sidebar), CONFIG_FE_ALL},
    [HOTKEY_SHOW_SCHEMA] = {"show_schema", "Show schema", HOTKEY_CAT_TABLE,
                            DEF_KEYS(def_show_schema), CONFIG_FE_ALL},
    [HOTKEY_REFRESH] = {"refresh", "Refresh", HOTKEY_CAT_TABLE,
                        DEF_KEYS(def_refresh), CONFIG_FE_ALL},
    [HOTKEY_CYCLE_SORT] = {"cycle_sort", "Cycle sort", HOTKEY_CAT_TABLE,
                           DEF_KEYS(def_cycle_sort), CONFIG_FE_ALL},
    [HOTKEY_CELL_COPY] = {"cell_copy", "Copy cell", HOTKEY_CAT_TABLE,
                          DEF_KEYS(def_cell_copy), CONFIG_FE_ALL},
    [HOTKEY_CELL_PASTE] = {"cell_paste", "Paste cell", HOTKEY_CAT_TABLE,
                           DEF_KEYS(def_cell_paste), CONFIG_FE_ALL},

    /* General */
    [HOTKEY_PREV_TAB] = {"prev_tab", "Previous tab", HOTKEY_CAT_GENERAL,
                         DEF_KEYS(def_prev_tab), CONFIG_FE_ALL},
    [HOTKEY_NEXT_TAB] = {"next_tab", "Next tab", HOTKEY_CAT_GENERAL,
                         DEF_KEYS(def_next_tab), CONFIG_FE_ALL},
    [HOTKEY_CLOSE_TAB] = {"close_tab", "Close tab", HOTKEY_CAT_GENERAL,
                          DEF_KEYS(def_close_tab), CONFIG_FE_ALL},
    [HOTKEY_NEW_TAB] = {"new_tab", "New tab", HOTKEY_CAT_GENERAL,
                        DEF_KEYS(def_new_tab), CONFIG_FE_ALL},
    [HOTKEY_PREV_WORKSPACE] = {"prev_workspace", "Previous workspace",
                               HOTKEY_CAT_GENERAL,
                               DEF_KEYS(def_prev_workspace), CONFIG_FE_ALL},
    [HOTKEY_NEXT_WORKSPACE] = {"next_workspace", "Next workspace",
                               HOTKEY_CAT_GENERAL,
                               DEF_KEYS(def_next_workspace), CONFIG_FE_ALL},
    [HOTKEY_TOGGLE_HEADER] = {"toggle_header", "Toggle header",
                              HOTKEY_CAT_GENERAL, DEF_KEYS(def_toggle_header), CONFIG_FE_NCURSES},
    [HOTKEY_TOGGLE_STATUS] = {"toggle_status", "Toggle status bar",
                              HOTKEY_CAT_GENERAL, DEF_KEYS(def_toggle_status), CONFIG_FE_NCURSES},
    [HOTKEY_CONNECT_DIALOG] = {"connect_dialog", "Workspaces",
                               HOTKEY_CAT_GENERAL,
                               DEF_KEYS(def_connect_dialog), CONFIG_FE_ALL},
    [HOTKEY_TOGGLE_HISTORY] = {"toggle_history", "Query history",
                               HOTKEY_CAT_GENERAL,
                               DEF_KEYS(def_toggle_history), CONFIG_FE_ALL},
    [HOTKEY_HELP] = {"help", "Hotkeys", HOTKEY_CAT_GENERAL, DEF_KEYS(def_help), CONFIG_FE_ALL},
    [HOTKEY_QUIT] = {"quit", "Quit", HOTKEY_CAT_GENERAL, DEF_KEYS(def_quit), CONFIG_FE_ALL},
    [HOTKEY_CONFIG] = {"config", "Configuration", HOTKEY_CAT_GENERAL,
                       DEF_KEYS(def_config), CONFIG_FE_ALL},
    [HOTKEY_CUT_LINE] = {"cut_line", "Cut line to buffer", HOTKEY_CAT_GENERAL,
                         DEF_KEYS(def_cut_line), CONFIG_FE_ALL},
    [HOTKEY_PASTE] = {"paste", "Paste from buffer", HOTKEY_CAT_GENERAL,
                      DEF_KEYS(def_paste), CONFIG_FE_ALL},

    /* Query Tab */
    [HOTKEY_OPEN_QUERY] = {"open_query", "Open query tab", HOTKEY_CAT_QUERY,
                           DEF_KEYS(def_open_query), CONFIG_FE_ALL},
    [HOTKEY_EXECUTE_QUERY] = {"execute_query", "Execute query",
                              HOTKEY_CAT_QUERY, DEF_KEYS(def_execute_query), CONFIG_FE_ALL},
    [HOTKEY_EXECUTE_ALL] = {"execute_all", "Execute all", HOTKEY_CAT_QUERY,
                            DEF_KEYS(def_execute_all), CONFIG_FE_ALL},
    [HOTKEY_EXECUTE_TRANSACTION] = {"execute_transaction",
                                    "Execute in transaction", HOTKEY_CAT_QUERY,
                                    DEF_KEYS(def_execute_transaction), CONFIG_FE_ALL},
    [HOTKEY_QUERY_SWITCH_FOCUS] = {"query_switch_focus",
                                   "Switch editor/results", HOTKEY_CAT_QUERY,
                                   NULL, 0, CONFIG_FE_NCURSES}, /* Unbound by default */
    [HOTKEY_QUERY_FOCUS_EDITOR] = {"query_focus_editor", "Focus query editor",
                                   HOTKEY_CAT_QUERY,
                                   DEF_KEYS(def_query_focus_editor), CONFIG_FE_NCURSES},
    [HOTKEY_QUERY_UNFOCUS] = {"query_unfocus", "Leave editor/results",
                              HOTKEY_CAT_QUERY, DEF_KEYS(def_query_unfocus), CONFIG_FE_NCURSES},

    /* Filters Panel */
    [HOTKEY_ADD_FILTER] = {"add_filter", "Add filter", HOTKEY_CAT_FILTERS,
                           DEF_KEYS(def_add_filter), CONFIG_FE_ALL},
    [HOTKEY_REMOVE_FILTER] = {"remove_filter", "Remove filter",
                              HOTKEY_CAT_FILTERS, DEF_KEYS(def_remove_filter), CONFIG_FE_ALL},
    [HOTKEY_CLEAR_FILTERS] = {"clear_filters", "Clear filters",
                              HOTKEY_CAT_FILTERS, DEF_KEYS(def_clear_filters), CONFIG_FE_ALL},
    [HOTKEY_FILTERS_SWITCH_FOCUS] = {"filters_switch_focus", "Switch to table",
                                     HOTKEY_CAT_FILTERS,
                                     NULL, 0 /* Unbound: Tab moves between panels */, CONFIG_FE_NCURSES},

    /* Sidebar */
    [HOTKEY_SIDEBAR_FILTER] = {"sidebar_filter", "Filter tables",
                               HOTKEY_CAT_SIDEBAR,
                               DEF_KEYS(def_sidebar_filter), CONFIG_FE_ALL},

    /* Connection Dialog */
    [HOTKEY_CONN_TEST] = {"conn_test", "Test connection", HOTKEY_CAT_CONNECT,
                          DEF_KEYS(def_conn_test), CONFIG_FE_NCURSES},
    [HOTKEY_CONN_SAVE] = {"conn_save", "Save to list", HOTKEY_CAT_CONNECT,
                          DEF_KEYS(def_conn_save), CONFIG_FE_NCURSES},
    [HOTKEY_CONN_NEW] = {"conn_new", "New connection", HOTKEY_CAT_CONNECT,
                         DEF_KEYS(def_conn_new), CONFIG_FE_NCURSES},
    [HOTKEY_CONN_NEW_FOLDER] = {"conn_new_folder", "New folder",
                                HOTKEY_CAT_CONNECT,
                                DEF_KEYS(def_conn_new_folder), CONFIG_FE_NCURSES},
    [HOTKEY_CONN_EDIT] = {"conn_edit", "Edit", HOTKEY_CAT_CONNECT,
                          DEF_KEYS(def_conn_edit), CONFIG_FE_NCURSES},
    [HOTKEY_CONN_DELETE] = {"conn_delete", "Delete", HOTKEY_CAT_CONNECT,
                            DEF_KEYS(def_conn_delete), CONFIG_FE_NCURSES},
    [HOTKEY_CONN_RENAME] = {"conn_rename", "Rename", HOTKEY_CAT_CONNECT,
                            DEF_KEYS(def_conn_rename), CONFIG_FE_NCURSES},
    [HOTKEY_CONN_MOVE] = {"conn_move", "Move item", HOTKEY_CAT_CONNECT,
                          DEF_KEYS(def_conn_move), CONFIG_FE_NCURSES},
    [HOTKEY_CONN_DUPLICATE] = {"conn_duplicate", "Duplicate connection",
                               HOTKEY_CAT_CONNECT, DEF_KEYS(def_conn_duplicate), CONFIG_FE_NCURSES},
    [HOTKEY_CONN_FILTER] = {"conn_filter", "Filter connections",
                            HOTKEY_CAT_CONNECT, DEF_KEYS(def_conn_filter), CONFIG_FE_NCURSES},

    /* Row Selection (Table category) */
    [HOTKEY_TOGGLE_SELECTION] = {"toggle_selection", "Toggle selection",
                                 HOTKEY_CAT_TABLE,
                                 DEF_KEYS(def_toggle_selection), CONFIG_FE_ALL},
    [HOTKEY_CLEAR_SELECTIONS] = {"clear_selections", "Clear selections",
                                 HOTKEY_CAT_TABLE,
                                 DEF_KEYS(def_clear_selections), CONFIG_FE_ALL},

    /* Row Add (Table category) */
    [HOTKEY_ROW_ADD] = {"row_add", "Add new row", HOTKEY_CAT_TABLE,
                        DEF_KEYS(def_row_add), CONFIG_FE_ALL},
    [HOTKEY_ROW_SAVE] = {"row_save", "Save new row", HOTKEY_CAT_TABLE,
                         DEF_KEYS(def_row_save), CONFIG_FE_ALL},

    /* Modal Editor */
    [HOTKEY_EDITOR_SAVE] = {"editor_save", "Save", HOTKEY_CAT_EDITOR,
                            DEF_KEYS(def_editor_save), CONFIG_FE_ALL},
    [HOTKEY_EDITOR_NULL] = {"editor_null", "Set NULL", HOTKEY_CAT_EDITOR,
                            DEF_KEYS(def_editor_null), CONFIG_FE_ALL},
    [HOTKEY_EDITOR_EMPTY] = {"editor_empty", "Set empty", HOTKEY_CAT_EDITOR,
                             DEF_KEYS(def_editor_empty), CONFIG_FE_ALL},
    [HOTKEY_EDITOR_CANCEL] = {"editor_cancel", "Cancel", HOTKEY_CAT_EDITOR,
                              DEF_KEYS(def_editor_cancel), CONFIG_FE_ALL},

    /* Config Editor */
    [HOTKEY_CONFIG_RESET] = {"config_reset", "Reset hotkey", HOTKEY_CAT_GENERAL,
                             DEF_KEYS(def_config_reset), CONFIG_FE_NCURSES},
    [HOTKEY_CONFIG_RESET_ALL] = {"config_reset_all", "Reset all hotkeys",
                                 HOTKEY_CAT_GENERAL,
                                 DEF_KEYS(def_config_reset_all), CONFIG_FE_NCURSES},

    /* History Dialog */
    [HOTKEY_HISTORY_COPY] = {"history_copy", "Copy to clipboard",
                             HOTKEY_CAT_HISTORY, DEF_KEYS(def_history_copy), CONFIG_FE_NCURSES},
    [HOTKEY_HISTORY_DELETE] = {"history_delete", "Delete entry",
                               HOTKEY_CAT_HISTORY,
                               DEF_KEYS(def_history_delete), CONFIG_FE_NCURSES},
    [HOTKEY_HISTORY_CLEAR] = {"history_clear", "Clear all", HOTKEY_CAT_HISTORY,
                              DEF_KEYS(def_history_clear), CONFIG_FE_NCURSES},
    [HOTKEY_HISTORY_CLOSE] = {"history_close", "Close dialog",
                              HOTKEY_CAT_HISTORY, DEF_KEYS(def_history_close), CONFIG_FE_NCURSES},
    [HOTKEY_PALETTE] = {"palette", "Command palette", HOTKEY_CAT_GENERAL, DEF_KEYS(def_palette), CONFIG_FE_NOTCURSES},
    [HOTKEY_CLOSE_VIEW] = {"close_view", "Close view or tab", HOTKEY_CAT_GENERAL, DEF_KEYS(def_close_view), CONFIG_FE_NOTCURSES},
    [HOTKEY_NEXT_CONN_TAB] = {"next_conn_tab", "Next connection tab", HOTKEY_CAT_GENERAL, DEF_KEYS(def_next_conn_tab), CONFIG_FE_NOTCURSES},
    [HOTKEY_PREV_CONN_TAB] = {"prev_conn_tab", "Previous connection tab", HOTKEY_CAT_GENERAL, DEF_KEYS(def_prev_conn_tab), CONFIG_FE_NOTCURSES},
    [HOTKEY_CONN_TAB_1] = {"conn_tab_1", "Connection tab 1", HOTKEY_CAT_GENERAL, DEF_KEYS(def_conn_tab_1), CONFIG_FE_NOTCURSES},
    [HOTKEY_CONN_TAB_2] = {"conn_tab_2", "Connection tab 2", HOTKEY_CAT_GENERAL, DEF_KEYS(def_conn_tab_2), CONFIG_FE_NOTCURSES},
    [HOTKEY_CONN_TAB_3] = {"conn_tab_3", "Connection tab 3", HOTKEY_CAT_GENERAL, DEF_KEYS(def_conn_tab_3), CONFIG_FE_NOTCURSES},
    [HOTKEY_CONN_TAB_4] = {"conn_tab_4", "Connection tab 4", HOTKEY_CAT_GENERAL, DEF_KEYS(def_conn_tab_4), CONFIG_FE_NOTCURSES},
    [HOTKEY_CONN_TAB_5] = {"conn_tab_5", "Connection tab 5", HOTKEY_CAT_GENERAL, DEF_KEYS(def_conn_tab_5), CONFIG_FE_NOTCURSES},
    [HOTKEY_CONN_TAB_6] = {"conn_tab_6", "Connection tab 6", HOTKEY_CAT_GENERAL, DEF_KEYS(def_conn_tab_6), CONFIG_FE_NOTCURSES},
    [HOTKEY_CONN_TAB_7] = {"conn_tab_7", "Connection tab 7", HOTKEY_CAT_GENERAL, DEF_KEYS(def_conn_tab_7), CONFIG_FE_NOTCURSES},
    [HOTKEY_CONN_TAB_8] = {"conn_tab_8", "Connection tab 8", HOTKEY_CAT_GENERAL, DEF_KEYS(def_conn_tab_8), CONFIG_FE_NOTCURSES},
    [HOTKEY_CONN_TAB_9] = {"conn_tab_9", "Connection tab 9", HOTKEY_CAT_GENERAL, DEF_KEYS(def_conn_tab_9), CONFIG_FE_NOTCURSES},
    [HOTKEY_VIEW_SWITCHER] = {"view_switcher", "Switch view", HOTKEY_CAT_GENERAL, DEF_KEYS(def_view_switcher), CONFIG_FE_NOTCURSES},
    [HOTKEY_TOGGLE_LOG] = {"toggle_log", "Toggle log", HOTKEY_CAT_GENERAL, DEF_KEYS(def_toggle_log), CONFIG_FE_NOTCURSES},
    [HOTKEY_COLUMN_NARROWER] = {"column_narrower", "Narrow column", HOTKEY_CAT_TABLE, DEF_KEYS(def_column_narrower), CONFIG_FE_NOTCURSES},
    [HOTKEY_COLUMN_WIDER] = {"column_wider", "Widen column", HOTKEY_CAT_TABLE, DEF_KEYS(def_column_wider), CONFIG_FE_NOTCURSES},
    [HOTKEY_SPLIT_UP] = {"split_up", "Editor smaller", HOTKEY_CAT_QUERY, DEF_KEYS(def_split_up), CONFIG_FE_NOTCURSES},
    [HOTKEY_SPLIT_DOWN] = {"split_down", "Editor larger", HOTKEY_CAT_QUERY, DEF_KEYS(def_split_down), CONFIG_FE_NOTCURSES},
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

  dst->keys = safe_calloc(src->num_keys, sizeof(char *));

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
    {"SHIFT+TAB", UI_KEY_BTAB},
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

  /* Modifier prefixes in any order ("SHIFT+TAB" is a table name) */
  for (;;) {
    if (strncmp(str, "CTRL+", 5) == 0) {
      *mods |= UI_MOD_CTRL;
      str += 5;
    } else if (strncmp(str, "ALT+", 4) == 0) {
      *mods |= UI_MOD_ALT;
      str += 4;
    } else if (strncmp(str, "SHIFT+", 6) == 0 && strcmp(str, "SHIFT+TAB") != 0) {
      *mods |= UI_MOD_SHIFT;
      str += 6;
    } else {
      break;
    }
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
    if ((*mods & (UI_MOD_CTRL | UI_MOD_SHIFT)) && c >= 'a' && c <= 'z') {
      c = (char)(c - ('a' - 'A'));
    }
    /* Shift alone on a character is its case; with Ctrl or Alt it stays a
     * modifier (Ctrl+Shift+P is not Ctrl+P) */
    if ((*mods & UI_MOD_SHIFT) && !(*mods & (UI_MOD_CTRL | UI_MOD_ALT)) &&
        ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
      *mods &= ~UI_MOD_SHIFT;
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
    {UI_KEY_BTAB, "S-Tab"},
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
  char prefix[24] = "";
  if (mods & UI_MOD_CTRL)
    strcat(prefix, "Ctrl+");
  if (mods & UI_MOD_ALT)
    strcat(prefix, "Alt+");
  if (mods & UI_MOD_SHIFT)
    strcat(prefix, "Shift+");

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
  Config *config = safe_calloc(1, sizeof(Config));

  /* General settings */
  config->general.show_header = true;
  config->general.show_status_bar = true;
  config->general.restore_session = true;
  config->general.restore_cursor_position = true;
  config->general.quit_confirmation = false;
  config->general.delete_confirmation = true; /* Default: ask before delete */
  config->general.auto_open_first_table = false;
  config->general.close_conn_on_last_tab = false;
  config->general.history_mode =
      HISTORY_MODE_SESSION; /* Default: session only */
  config->general.history_max_size = HISTORY_SIZE_DEFAULT;
  config->general.daemon_spawn_mode = SPAWN_MODE_UNIX; /* Default: Unix socket */
  config->general.daemon_idle_timeout = CONFIG_IDLE_TIMEOUT_DEFAULT;
  config->general.pagination_strategy = LACE_PAGINATION_KEYSET;
  config->general.approximate_counts = true;
  config->general.page_size = CONFIG_PAGE_SIZE_DEFAULT;
  config->general.row_numbers = CONFIG_ROW_NUMBERS_AUTO;
  config->general.color_scheme = 0;
  config->extra = cJSON_CreateObject();
  config->frontend = CONFIG_FE_NCURSES; /* The notcurses frontend sets its own */

  /* Hotkeys - copy from defaults */
  for (int i = 0; i < HOTKEY_COUNT; i++) {
    const ActionMeta *meta = &action_meta[i];
    HotkeyBinding *binding = &config->hotkeys[i];

    if (meta->num_default_keys > 0) {
      binding->keys = safe_calloc(meta->num_default_keys, sizeof(char *));
      for (size_t j = 0; j < meta->num_default_keys; j++) {
        binding->keys[j] = str_dup(meta->default_keys[j]);
      }
      binding->num_keys = meta->num_default_keys;
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

  cJSON_Delete(config->extra);
  for (size_t i = 0; i < config->num_touched; i++)
    free(config->extra_touched[i]);
  free(config->extra_touched);
  free(config);
}

cJSON *config_section(Config *config, const char *name) {
  if (!config || !name || !name[0])
    return NULL;
  if (!config->extra)
    config->extra = cJSON_CreateObject();
  bool known = false;
  for (size_t i = 0; i < config->num_touched; i++)
    if (strcmp(config->extra_touched[i], name) == 0)
      known = true;
  if (!known) {
    char *dup = str_dup(name);
    if (dup) {
      config->extra_touched = safe_realloc(
          config->extra_touched, (config->num_touched + 1) * sizeof(char *));
      config->extra_touched[config->num_touched++] = dup;
    }
  }
  cJSON *s = cJSON_GetObjectItem(config->extra, name);
  if (!cJSON_IsObject(s)) {
    if (s)
      cJSON_DeleteItemFromObject(config->extra, name);
    s = cJSON_AddObjectToObject(config->extra, name);
  }
  return s;
}

/* ============================================================================
 * Config Copy
 * ============================================================================
 */

Config *config_copy(const Config *config) {
  if (!config)
    return NULL;

  Config *copy = safe_calloc(1, sizeof(Config));

  copy->general = config->general;
  copy->frontend = config->frontend;
  copy->extra = config->extra ? cJSON_Duplicate(config->extra, true)
                              : cJSON_CreateObject();
  if (config->num_touched) {
    copy->extra_touched = safe_calloc(config->num_touched, sizeof(char *));
    for (size_t i = 0; i < config->num_touched; i++)
      copy->extra_touched[i] = str_dup(config->extra_touched[i]);
    copy->num_touched = config->num_touched;
  }

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
      binding->keys = safe_calloc((size_t)count, sizeof(char *));
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

  return true;
}

/* ============================================================================
 * Config Load
 * ============================================================================
 */

Config *config_load(char **error) { return config_load_file(NULL, error); }

Config *config_load_file(const char *path_in, char **error) {
  char *path = path_in ? str_dup(path_in) : config_get_path();
  if (!path) {
    /* No config directory, return defaults */
    return config_get_defaults();
  }

  if (!platform_file_exists(path)) {
    /* No config file, return defaults */
    free(path);
    return config_get_defaults();
  }

  char *content = platform_read_file(path, 1024 * 1024); /* Max 1MB */
  if (!content || !content[0]) {
    if (content)
      err_setf(error, "Empty config file");
    else
      err_setf(error, "Failed to read %s: %s", path, strerror(errno));
    free(content);
    free(path);
    return config_get_defaults();
  }
  free(path);

  cJSON *json = cJSON_Parse(content);

  if (!json) {
    const char *at = cJSON_GetErrorPtr();
    if (at)
      err_setf(error, "JSON parse error near byte %ld", (long)(at - content));
    else
      err_setf(error, "JSON parse error");
    str_secure_free(content);
    return config_get_defaults();
  }
  if (!cJSON_IsObject(json)) {
    cJSON_Delete(json);
    str_secure_free(content);
    err_setf(error, "Config file is not a JSON object");
    return config_get_defaults();
  }
  str_secure_free(content);

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
    config->general.restore_cursor_position =
        json_get_bool(general, "restore_cursor_position",
                      config->general.restore_cursor_position);
    config->general.quit_confirmation =
        json_get_bool(general, "quit_confirmation", config->general.quit_confirmation);
    config->general.delete_confirmation =
        json_get_bool(general, "delete_confirmation", config->general.delete_confirmation);
    config->general.auto_open_first_table =
        json_get_bool(general, "auto_open_first_table", config->general.auto_open_first_table);
    config->general.close_conn_on_last_tab =
        json_get_bool(general, "close_conn_on_last_tab", config->general.close_conn_on_last_tab);

    /* Integer settings with range validation */
    int val = json_get_int(general, "history_mode", config->general.history_mode);
    if (val >= HISTORY_MODE_OFF && val <= HISTORY_MODE_PERSISTENT)
      config->general.history_mode = val;

    val = json_get_int(general, "history_max_size", config->general.history_max_size);
    if (val >= HISTORY_SIZE_MIN && val <= HISTORY_SIZE_MAX)
      config->general.history_max_size = val;

    val = json_get_int(general, "daemon_spawn_mode", config->general.daemon_spawn_mode);
    if (val >= SPAWN_MODE_UNIX && val <= SPAWN_MODE_DETECT)
      config->general.daemon_spawn_mode = val;

    val = json_get_int(general, "daemon_idle_timeout", config->general.daemon_idle_timeout);
    if (val >= CONFIG_IDLE_TIMEOUT_MIN && val <= CONFIG_IDLE_TIMEOUT_MAX)
      config->general.daemon_idle_timeout = val;

    val = json_get_int(general, "pagination_strategy", config->general.pagination_strategy);
    /* 2 was "smart" keyset, now the same as keyset */
    if (val == LACE_PAGINATION_OFFSET || val == LACE_PAGINATION_KEYSET || val == 2)
      config->general.pagination_strategy =
          val == LACE_PAGINATION_OFFSET ? LACE_PAGINATION_OFFSET
                                        : LACE_PAGINATION_KEYSET;

    config->general.approximate_counts =
        json_get_bool(general, "approximate_counts", config->general.approximate_counts);
    val = json_get_int(general, "page_size", config->general.page_size);
    if (val >= CONFIG_PAGE_SIZE_MIN && val <= CONFIG_PAGE_SIZE_MAX)
      config->general.page_size = val;
    val = json_get_int(general, "row_numbers", config->general.row_numbers);
    if (val >= CONFIG_ROW_NUMBERS_NEVER && val <= CONFIG_ROW_NUMBERS_ALWAYS)
      config->general.row_numbers = val;
    val = json_get_int(general, "color_scheme", config->general.color_scheme);
    if (val >= 0 && val <= 2)
      config->general.color_scheme = val;
  }

  /* Parse hotkeys */
  cJSON *hotkeys = json_get_object(json, "hotkeys");
  if (hotkeys) {
    parse_hotkeys(hotkeys, config);
  }

  /* Every other top-level member belongs to another frontend: keep it */
  cJSON_DeleteItemFromObject(json, "general");
  cJSON_DeleteItemFromObject(json, "hotkeys");
  cJSON_Delete(config->extra);
  config->extra = json; /* adopted */
  return config;
}

/* ============================================================================
 * Config Save
 * ============================================================================
 */

bool config_save(const Config *config, char **error) {
  return config_save_file(config, NULL, error);
}

bool config_save_file(const Config *config, const char *path_in, char **error) {
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

  if (!path_in) {
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
  }

  char *path = path_in ? str_dup(path_in) : config_get_path();
  if (!path) {
    err_setf(error, "Failed to get config path");
    return false;
  }

  /* Start from the file on disk so sections another frontend changed since we
   * loaded survive; only sections touched through config_section() are
   * written from memory. A file that exists but does not parse is never
   * overwritten. */
  cJSON *json = NULL;
  if (platform_file_exists(path)) {
    char *cur = platform_read_file(path, 1024 * 1024);
    if (!cur) {
      err_setf(error, "Failed to read %s: %s", path, strerror(errno));
      free(path);
      return false;
    }
    if (cur[0]) {
      json = cJSON_Parse(cur);
      if (!json) {
        const char *at = cJSON_GetErrorPtr();
        err_setf(error, "%s: JSON parse error near byte %ld", path,
                 at ? (long)(at - cur) : 0L);
      } else if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        json = NULL;
        err_setf(error, "%s: not a JSON object", path);
      }
      if (!json) {
        str_secure_free(cur);
        free(path);
        return false;
      }
    }
    str_secure_free(cur);
  }
  if (!json) {
    json = config->extra ? cJSON_Duplicate(config->extra, true)
                         : cJSON_CreateObject();
  } else {
    for (size_t i = 0; i < config->num_touched; i++) {
      const char *name = config->extra_touched[i];
      cJSON *sec = config->extra ? cJSON_GetObjectItem(config->extra, name) : NULL;
      cJSON_DeleteItemFromObject(json, name);
      if (sec)
        cJSON_AddItemToObject(json, name, cJSON_Duplicate(sec, true));
    }
  }
  cJSON_DeleteItemFromObject(json, "general");
  cJSON_DeleteItemFromObject(json, "hotkeys");

  /* General settings */
  cJSON *general = cJSON_CreateObject();
  JSON_ADD_BOOL(general, "show_header", config->general.show_header);
  JSON_ADD_BOOL(general, "show_status_bar", config->general.show_status_bar);
  JSON_ADD_BOOL(general, "restore_session", config->general.restore_session);
  JSON_ADD_BOOL(general, "restore_cursor_position",
                config->general.restore_cursor_position);
  JSON_ADD_BOOL(general, "quit_confirmation", config->general.quit_confirmation);
  JSON_ADD_BOOL(general, "delete_confirmation", config->general.delete_confirmation);
  JSON_ADD_BOOL(general, "auto_open_first_table", config->general.auto_open_first_table);
  JSON_ADD_BOOL(general, "close_conn_on_last_tab", config->general.close_conn_on_last_tab);
  JSON_ADD_INT(general, "history_mode", config->general.history_mode);
  JSON_ADD_INT(general, "history_max_size", config->general.history_max_size);
  JSON_ADD_INT(general, "daemon_spawn_mode", config->general.daemon_spawn_mode);
  JSON_ADD_INT(general, "daemon_idle_timeout", config->general.daemon_idle_timeout);
  JSON_ADD_INT(general, "pagination_strategy", config->general.pagination_strategy);
  JSON_ADD_BOOL(general, "approximate_counts", config->general.approximate_counts);
  JSON_ADD_INT(general, "page_size", config->general.page_size);
  JSON_ADD_INT(general, "row_numbers", config->general.row_numbers);
  JSON_ADD_INT(general, "color_scheme", config->general.color_scheme);
  cJSON_AddItemToObject(json, "general", general);

  /* Hotkeys */
  cJSON *hotkeys = serialize_hotkeys(config);
  if (hotkeys) {
    cJSON_AddItemToObject(json, "hotkeys", hotkeys);
  }

  char *content = cJSON_Print(json);
  cJSON_Delete(json);

  if (!content) {
    free(path);
    err_setf(error, "Failed to serialize JSON");
    return false;
  }

  bool ok = platform_write_file_private(path, content, strlen(content));
  int e = errno;
  free(content);
  if (!ok) {
    err_setf(error, "Failed to write %s: %s", path, strerror(e));
    free(path);
    return false;
  }
  free(path);

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
    binding->keys = safe_calloc(meta->num_default_keys, sizeof(char *));
    for (size_t j = 0; j < meta->num_default_keys; j++) {
      binding->keys[j] = str_dup(meta->default_keys[j]);
    }
    binding->num_keys = meta->num_default_keys;
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

  /* The shared general ranges (load clamps them; this catches callers) */
  const GeneralConfig *g = &config->general;
  if (g->page_size < CONFIG_PAGE_SIZE_MIN || g->page_size > CONFIG_PAGE_SIZE_MAX) {
    err_setf(error, "Page size %d is out of range (%d..%d)", g->page_size,
             CONFIG_PAGE_SIZE_MIN, CONFIG_PAGE_SIZE_MAX);
    return false;
  }
  if (g->row_numbers < CONFIG_ROW_NUMBERS_NEVER ||
      g->row_numbers > CONFIG_ROW_NUMBERS_ALWAYS) {
    err_setf(error, "Row numbers %d is out of range (0..2)", g->row_numbers);
    return false;
  }
  if (g->color_scheme < 0 || g->color_scheme > 2) {
    err_setf(error, "Color scheme %d is out of range (0..2)", g->color_scheme);
    return false;
  }

  /* Check for hotkey conflicts among this frontend's actions */
  for (int i = 0; i < HOTKEY_COUNT; i++) {
    if (!(action_meta[i].frontends & config->frontend))
      continue;
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
      UiKeyMod mask = UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_SHIFT;
      if ((mods & mask) == (event->key.mods & mask))
        return true;
    }
  }

  return false;
}

unsigned hotkey_action_frontends(HotkeyAction action) {
  if (action < 0 || action >= HOTKEY_COUNT)
    return 0;
  return action_meta[action].frontends;
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

  /* An action the running frontend does not have conflicts with nothing */
  if (!(action_meta[action].frontends & config->frontend))
    return HOTKEY_COUNT;

  /* Only check for conflicts within the same category */
  HotkeyCategory category = action_meta[action].category;

  for (HotkeyAction i = 0; i < HOTKEY_COUNT; i++) {
    if (i == action)
      continue; /* Skip self */

    /* Skip actions the running frontend does not have */
    if (!(action_meta[i].frontends & config->frontend))
      continue;

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
  binding->keys =
      safe_realloc(binding->keys, (binding->num_keys + 1) * sizeof(char *));
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

