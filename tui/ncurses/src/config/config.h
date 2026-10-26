/*
 * Lace
 * Application Configuration
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONFIG_CONFIG_H
#define LACE_CONFIG_CONFIG_H

#include "../core/constants.h"
#include <stdbool.h>
#include <stddef.h>

#define CONFIG_FILE "config.json"

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
  HOTKEY_CAT_EDITOR,     /* Modal editor operations */
  HOTKEY_CAT_HISTORY,    /* History dialog operations */
  HOTKEY_CAT_STRUCTURE,  /* The structure editors (the sub-view lists of a table tab) */
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
  HOTKEY_REFRESH,
  HOTKEY_CYCLE_SORT,
  HOTKEY_CELL_COPY,
  HOTKEY_CELL_PASTE,
  HOTKEY_CELL_CUT, /* copy, then set empty */
  HOTKEY_UNDO,     /* revert the view's last write */

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
  HOTKEY_TOGGLE_HISTORY,
  HOTKEY_HELP,
  HOTKEY_QUIT,
  HOTKEY_CONFIG,
  HOTKEY_CUT_LINE,
  HOTKEY_PASTE,

  /* Query Tab (HOTKEY_CAT_QUERY) */
  HOTKEY_OPEN_QUERY,
  HOTKEY_EXECUTE_QUERY,
  HOTKEY_EXECUTE_ALL,
  HOTKEY_EXECUTE_TRANSACTION,
  HOTKEY_QUERY_SWITCH_FOCUS,
  HOTKEY_QUERY_FOCUS_EDITOR,
  HOTKEY_QUERY_UNFOCUS,

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
  HOTKEY_CONN_MOVE,
  HOTKEY_CONN_DUPLICATE,
  HOTKEY_CONN_FILTER,

  /* Row Selection (HOTKEY_CAT_TABLE) */
  HOTKEY_TOGGLE_SELECTION,
  HOTKEY_CLEAR_SELECTIONS,

  /* Row Add (HOTKEY_CAT_TABLE) */
  HOTKEY_ROW_ADD,
  HOTKEY_ROW_SAVE,

  /* Modal Editor (HOTKEY_CAT_EDITOR) */
  HOTKEY_EDITOR_SAVE,
  HOTKEY_EDITOR_NULL,
  HOTKEY_EDITOR_EMPTY,
  HOTKEY_EDITOR_CANCEL,

  /* Config Editor (HOTKEY_CAT_GENERAL) */
  HOTKEY_CONFIG_RESET,
  HOTKEY_CONFIG_RESET_ALL,

  /* History Dialog (HOTKEY_CAT_HISTORY) */
  HOTKEY_HISTORY_COPY,
  HOTKEY_HISTORY_DELETE,
  HOTKEY_HISTORY_CLEAR,
  HOTKEY_HISTORY_CLOSE,


  /* notcurses frontend (HOTKEY_CAT_GENERAL / TABLE / QUERY); masked out of
   * this TUI's dialogs */
  HOTKEY_PALETTE,
  HOTKEY_NEXT_CONN_TAB,
  HOTKEY_PREV_CONN_TAB,
  HOTKEY_CONN_TAB_1,
  HOTKEY_CONN_TAB_2,
  HOTKEY_CONN_TAB_3,
  HOTKEY_CONN_TAB_4,
  HOTKEY_CONN_TAB_5,
  HOTKEY_CONN_TAB_6,
  HOTKEY_CONN_TAB_7,
  HOTKEY_CONN_TAB_8,
  HOTKEY_CONN_TAB_9,
  HOTKEY_VIEW_SWITCHER,
  HOTKEY_TOGGLE_LOG,
  HOTKEY_SWITCH_DATABASE, /* the header's database segment: another database of the server */
  HOTKEY_NEW_WORKSPACE,   /* a new workspace with a connection form (a GUI's new window) */
  HOTKEY_COLUMN_NARROWER,
  HOTKEY_COLUMN_WIDER,
  HOTKEY_SPLIT_UP,
  HOTKEY_SPLIT_DOWN,
  /* The sub-views of a table tab (HOTKEY_CAT_TABLE) and following a foreign key */
  HOTKEY_SUBVIEW_DATA,
  HOTKEY_SUBVIEW_STRUCTURE,
  HOTKEY_SUBVIEW_INDEXES,
  HOTKEY_SUBVIEW_FOREIGN_KEYS,
  HOTKEY_SUBVIEW_INFORMATION,
  HOTKEY_FOLLOW_FK,      /* the referenced table, filtered by the cursor row's key */
  HOTKEY_REFERENCED_BY,  /* a table whose foreign key points at the cursor row */
  /* The connection tab (notcurses for now): a new form, and the form's buttons
   * (HOTKEY_CAT_CONNECT, chords so they work while a field is typed in) */
  HOTKEY_NEW_CONNECTION,
  HOTKEY_CONN_OPEN,
  HOTKEY_CONN_OPEN_BACKGROUND,
  HOTKEY_CONN_OPEN_NEW_WORKSPACE,
  HOTKEY_CONN_OPEN_TO_WORKSPACE,
  HOTKEY_CONN_SAVE_AS_NEW,
  HOTKEY_CONN_CLEAR,
  /* The table's actions (HOTKEY_CAT_TABLE) and the editors of the Structure,
   * Indexes, Foreign keys and Information sub-views (HOTKEY_CAT_STRUCTURE:
   * their keys mean something only in those lists) */
  HOTKEY_TABLE_MENU,        /* rename, truncate, drop, new table/view/function */
  HOTKEY_STRUCT_ADD,        /* a column, an index, a foreign key */
  HOTKEY_STRUCT_EDIT,       /* the row under the cursor (Enter does too) */
  HOTKEY_STRUCT_DROP,
  HOTKEY_STRUCT_RENAME,
  HOTKEY_STRUCT_PRIMARY_KEY,/* Structure: the key editor */
  HOTKEY_STRUCT_MOVE,       /* Structure: reorder a column (MySQL) */
  HOTKEY_STRUCT_CHECK,      /* Indexes: a check constraint */
  HOTKEY_COUNT
} HotkeyAction;

/* ============================================================================
 * Configuration Types
 * ============================================================================
 */

/* Daemon spawn mode */
typedef enum {
  SPAWN_MODE_UNIX = 0,   /* Unix socket (default, supports multi-client) */
  SPAWN_MODE_STDIO = 1,  /* Stdio pipes (single client, auto-exit) */
  SPAWN_MODE_TCP = 2,    /* TCP socket (network, multi-client) */
  SPAWN_MODE_DETECT = 3  /* A running daemon (Unix socket, then TCP), else a
                            private stdio one; detected again if lost */
} DaemonSpawnMode;

/* General application settings */
typedef struct {
  bool show_header;
  bool show_status_bar;
  bool restore_session;
  bool restore_cursor_position; /* Restore cursor/scroll on session load */
  bool quit_confirmation;
  bool delete_confirmation;    /* Ask for confirmation before deleting rows */
  bool auto_open_first_table;  /* Open first table instead of connection tab */
  bool keep_connection_tab;    /* The last view's close leaves a connection tab */
  int last_workspace_close;    /* CONFIG_LAST_WS_*: closing the last workspace */
  int history_mode;            /* 0=off, 1=session, 2=persistent */
  int history_max_size;        /* Max history entries per connection */
  int daemon_spawn_mode;       /* 0=unix socket, 1=stdio pipes */
  int daemon_idle_timeout;     /* Seconds before daemon shuts down with no clients (0=disabled) */
  int pagination_strategy;     /* LacePaginationStrategy */
  /* Shared with the other frontends (see the shared config spec) */
  bool approximate_counts;     /* Catalog estimates for big server tables */
  int page_size;               /* Rows per page in the GUIs (100..100000); this TUI pages with TUI_PAGE_SIZE */
  int row_numbers;             /* 0 never, 1 auto, 2 always */
  int color_scheme;            /* 0 system, 1 light, 2 dark */
} GeneralConfig;

#define CONFIG_LAST_WS_QUIT 0 /* "on_last_workspace_close": "quit" */
#define CONFIG_LAST_WS_NEW 1  /* "new": a fresh workspace */

#define CONFIG_ROW_NUMBERS_NEVER 0
#define CONFIG_ROW_NUMBERS_AUTO 1
#define CONFIG_ROW_NUMBERS_ALWAYS 2
#define CONFIG_PAGE_SIZE_MIN 100
#define CONFIG_PAGE_SIZE_MAX 100000
#define CONFIG_PAGE_SIZE_DEFAULT 1000

/* Single hotkey binding (key string like "k", "CTRL+W", "F1") */
typedef struct {
  char **keys; /* Array of key strings */
  size_t num_keys;
} HotkeyBinding;

/* Full configuration */
struct cJSON;
typedef struct {
  GeneralConfig general;
  HotkeyBinding hotkeys[HOTKEY_COUNT];
  struct cJSON *extra; /* Every top-level member of the file this TUI does not
                          own (other frontends' sections); written back as is */
  char **extra_touched; /* Sections taken through config_section(): saved from
                           memory; the others are kept from the file on disk */
  size_t num_touched;
  unsigned frontend; /* CONFIG_FE_* of the frontend running: hotkey conflicts
                        are checked among its actions only */
} Config;

/* ============================================================================
 * Config API
 * ============================================================================
 */

/* Load config from disk (returns defaults if file doesn't exist) */
Config *config_load(char **error);

/* Save config to disk */
bool config_save(const Config *config, char **error);

/* Same, from an explicit path (NULL: the default file) */
Config *config_load_file(const char *path, char **error);
bool config_save_file(const Config *config, const char *path, char **error);

/* A frontend's own section in extra (owned object, created on demand) */
struct cJSON *config_section(Config *config, const char *name);

/* Free config structure */
void config_free(Config *config);

/* Get default configuration */
Config *config_get_defaults(void);

/* Deep copy configuration */
Config *config_copy(const Config *config);

/* Reset a single hotkey to its default */
void config_reset_hotkey(Config *config, HotkeyAction action);

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
/* The config string ("CTRL+A", "F5", "SHIFT+TAB", "SPACE", "x") for a key event, as
 * hotkey_add_key() takes it and hotkey_matches() matches the event again;
 * NULL for a key that cannot be bound (a non-ASCII character). Freed by the caller. */
char *hotkey_key_string(const UiEvent *event);
/* The config string ("CTRL+A", "F5", "SHIFT+TAB", "SPACE", "x") for a key event, as
 * hotkey_add_key() takes it and hotkey_matches() matches the event again;
 * NULL for a key that cannot be bound (a non-ASCII character). Freed by the caller. */
char *hotkey_key_string(const UiEvent *event);

/* Get action name for display (e.g., "Move up") */
const char *hotkey_action_name(HotkeyAction action);

/* Find action by JSON key, returns ACTION_COUNT if not found */
HotkeyAction hotkey_action_from_key(const char *key);

/* Where an action's keys are live. Two actions conflict when a key of one
 * is a key of the other and they share a context in the running frontend;
 * the same key in different places (x deletes a row in the grid, drops a
 * column in the Structure list, removes a filter in the panel) is fine. */
typedef enum {
  HOTKEY_CTX_GRID = 1u << 0,      /* a table tab's Data grid */
  HOTKEY_CTX_LIST = 1u << 1,      /* a table tab's Structure, Indexes, Foreign keys or Information list */
  HOTKEY_CTX_RESULTS = 1u << 2,   /* a query tab's results */
  HOTKEY_CTX_EDITOR = 1u << 3,    /* a query tab's SQL editor (only chords are live) */
  HOTKEY_CTX_UNFOCUSED = 1u << 4, /* a query tab with neither pane focused (ncurses) */
  HOTKEY_CTX_FILTERS = 1u << 5,   /* the filters panel */
  HOTKEY_CTX_SIDEBAR = 1u << 6,   /* the sidebar */
  HOTKEY_CTX_CONNECT = 1u << 7,   /* the Workspaces window, the connection form */
  HOTKEY_CTX_CELL = 1u << 8,      /* a cell editor (inline, modal, the add-row form's fields) */
  HOTKEY_CTX_ADD_ROW = 1u << 9,   /* the add-row form */
  HOTKEY_CTX_HISTORY = 1u << 10,  /* the history dialog */
  HOTKEY_CTX_CONFIG = 1u << 11    /* the hotkeys editor */
} HotkeyContext;
/* The main screen with a connection open */
#define HOTKEY_CTX_MAIN (HOTKEY_CTX_GRID | HOTKEY_CTX_LIST | HOTKEY_CTX_RESULTS | HOTKEY_CTX_UNFOCUSED | \
                         HOTKEY_CTX_FILTERS | HOTKEY_CTX_SIDEBAR)
unsigned hotkey_action_contexts(HotkeyAction action);

/* Check for conflicts: the first other action of the running frontend that
 * is live in one of the action's contexts and bound to key; HOTKEY_COUNT
 * when none */
HotkeyAction hotkey_find_conflict(const Config *config, HotkeyAction action,
                                  const char *key);
/* Every conflict among the running frontend's bindings, each pair once */
typedef struct { HotkeyAction a, b; const char *key; } HotkeyConflict;
size_t config_hotkey_conflicts(const Config *config, HotkeyConflict *out, size_t max);
/* "f: Toggle filters and Foreign keys; a: First row and Data" (allocated),
 * NULL when there is none; for the status line at startup */
char *config_hotkey_conflicts_text(const Config *config);

/* Add a key to an action's bindings */
bool hotkey_add_key(Config *config, HotkeyAction action, const char *key);

/* Remove a key from an action's bindings */
bool hotkey_remove_key(Config *config, HotkeyAction action, size_t key_index);

/* Get category for an action */
HotkeyCategory hotkey_get_category(HotkeyAction action);

/* Which frontends an action exists in (a configuration dialog shows only
 * its own; bindings of the others are kept in the file untouched) */
#define CONFIG_FE_NCURSES 1u
#define CONFIG_FE_NOTCURSES 2u
#define CONFIG_FE_ALL (CONFIG_FE_NCURSES | CONFIG_FE_NOTCURSES)
unsigned hotkey_action_frontends(HotkeyAction action);

/* Get category display name */
const char *hotkey_category_name(HotkeyCategory category);

#endif /* LACE_CONFIG_CONFIG_H */
