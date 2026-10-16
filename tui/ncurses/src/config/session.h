/*
 * Lace
 * Session Persistence - Save/Restore workspaces and tabs
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONFIG_SESSION_H
#define LACE_CONFIG_SESSION_H

#include "../core/app_state.h"
#include <stdbool.h>
#include <stddef.h>

/* Forward declaration for TuiState (opaque pointer) */
struct TuiState;

#define SESSION_FILE "session.json"

/* ============================================================================
 * Session Types (for loading before AppState/TuiState exist)
 * ============================================================================
 */

/* Serialized filter */
typedef struct {
  char *column_name; /* Column name (resolved at restore) */
  bool raw;          /* A RAW SQL condition (no column) */
  int op;            /* FilterOperator enum value */
  char *value;       /* Filter value */
  char *value2;      /* BETWEEN upper bound */
} SessionFilter;

/* Serialized sort entry (uses column name, not index) */
typedef struct {
  char *column_name; /* Column name (resolved at restore) */
  int direction;     /* SortDirection enum value */
} SessionSortEntry;

/* Serialized tab UI state (per-tab only; sidebar is global) */
typedef struct {
  bool filters_visible;
  bool filters_focused;
  bool query_focus_results;
} SessionTabUI;

/* Serialized tab */
typedef struct {
  int type;            /* TabType enum value */
  char *connection_id; /* UUID from saved connections */
  char *connstr;       /* Password-less; a connection a GUI opened by URL (no saved id), may be NULL */
  char *table_name;    /* For TABLE tabs */

  /* Cursor/scroll state */
  size_t cursor_row;
  size_t cursor_col;
  size_t scroll_row;
  size_t scroll_col;

  /* Sort state (multi-column) */
  SessionSortEntry *sort_entries; /* Array of sort columns (by name) */
  size_t num_sort_entries;        /* Number of sort columns */

  /* Filters */
  SessionFilter *filters;
  size_t num_filters;

  /* Query tab state */
  char *query_text;
  size_t query_cursor;
  size_t query_scroll_line;
  size_t query_scroll_col;

  /* UI state */
  SessionTabUI ui;
} SessionTab;

/* Serialized workspace */
typedef struct {
  char *name;
  SessionTab *tabs;
  size_t num_tabs;
  size_t current_tab;
} SessionWorkspace;

/* Full session state */
typedef struct {
  /* Global settings */
  bool header_visible;
  bool status_visible;
  bool sidebar_visible;

  /* Workspaces */
  SessionWorkspace *workspaces;
  size_t num_workspaces;
  size_t current_workspace;

  /* Workspaces of the file that could not be read (a newer frontend's
   * operator or tab type), shown after restoring; NULL if none */
  char *warning;
} Session;

/* ============================================================================
 * UI Callbacks (for decoupling from specific UI implementation)
 * ============================================================================
 */

/* Password prompt callback type.
 * Returns: malloc'd password string, or NULL if cancelled.
 * Caller must use str_secure_free() on result.
 * Parameters:
 *   user_data - opaque pointer passed to session_set_password_callback
 *   title - dialog title (e.g., "Password for mydb")
 *   label - prompt label (e.g., "Enter password:")
 *   error_msg - error to display (e.g., "Access denied"), or NULL
 */
typedef char *(*SessionPasswordCallback)(void *user_data, const char *title,
                                         const char *label,
                                         const char *error_msg);

/* Set the password prompt callback. Must be called before session_restore
 * if password prompts are needed. */
void session_set_password_callback(SessionPasswordCallback callback,
                                   void *user_data);

/* ============================================================================
 * Session API
 * ============================================================================
 */

/* Load session from disk (returns NULL if no session or error) */
Session *session_load(char **error);

/* Save current session to disk */
bool session_save(struct TuiState *state, char **error);

/* A stored password-less connection string with typed passwords added (NULL
 * keeps the string's own); SSH tunnel and key kept. str_secure_free() it. */
char *session_connstr_with_password(const char *connstr, const char *password,
                                    const char *ssh_password);

/* Free session structure */
void session_free(Session *session);

/* Restore session into TuiState/AppState */
bool session_restore(struct TuiState *state, Session *session, char **error);

/* One open workspace as JSON (what session.json holds for it), for a saved
 * workspace. Tabs whose connection has neither a saved id nor a connection
 * string cannot be opened again and are left out (*skipped counts them);
 * NULL if none is left. */
struct cJSON *session_workspace_to_json(struct TuiState *state, size_t ws_idx,
                                        size_t *skipped);

/* Open a workspace from session_workspace_to_json() as a new workspace
 * (named name, if given) and make it current. Connections already open are
 * reused; a missing password is asked for. */
bool session_open_workspace_json(struct TuiState *state, struct cJSON *json,
                                 const char *name, char **error);

/* Get session file path */
char *session_get_path(void);

#endif /* LACE_CONFIG_SESSION_H */
