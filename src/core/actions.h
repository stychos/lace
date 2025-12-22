/*
 * lace - Database Viewer and Manager
 * Core Actions API
 *
 * This module defines the command interface between UI and core logic.
 * All user interactions should be translated into Actions and dispatched
 * through app_dispatch(). This enables:
 *   - Multiple UI frontends (TUI, GUI) sharing the same logic
 *   - Clear separation between input handling and state mutation
 *   - Testable core logic without UI dependencies
 */

#ifndef LACE_CORE_ACTIONS_H
#define LACE_CORE_ACTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations - use struct keyword, not typedef, to avoid conflicts */
struct AppState;
struct TuiState;

/* ============================================================================
 * Change Flags - What was modified by an action
 * ============================================================================
 * Returned by app_dispatch() to tell UI what needs redrawing.
 */
typedef enum {
  CHANGED_NONE = 0,
  CHANGED_CURSOR = 1 << 0,       /* Cursor position changed */
  CHANGED_SCROLL = 1 << 1,       /* Scroll position changed */
  CHANGED_DATA = 1 << 2,         /* Table/query data changed */
  CHANGED_SCHEMA = 1 << 3,       /* Schema information changed */
  CHANGED_FILTERS = 1 << 4,      /* Filter definitions changed */
  CHANGED_STATUS = 1 << 5,       /* Status message changed */
  CHANGED_WORKSPACE = 1 << 6,    /* Current workspace changed */
  CHANGED_WORKSPACES = 1 << 7,   /* Workspace list changed (add/remove) */
  CHANGED_SIDEBAR = 1 << 8,      /* Sidebar state changed */
  CHANGED_CONNECTION = 1 << 9,   /* Connection state changed */
  CHANGED_TABLES = 1 << 10,      /* Table list changed */
  CHANGED_FOCUS = 1 << 11,       /* Focus changed (sidebar/table/filters) */
  CHANGED_EDIT = 1 << 12,        /* Edit mode state changed */
  CHANGED_LAYOUT = 1 << 13,      /* Window layout changed (resize, toggle) */

  /* Convenience combinations */
  CHANGED_VIEW = CHANGED_CURSOR | CHANGED_SCROLL | CHANGED_DATA,
  CHANGED_ALL = 0xFFFFFFFF
} ChangeFlags;

/* ============================================================================
 * Action Types
 * ============================================================================
 */
typedef enum {
  ACTION_NONE = 0,           /* No action (sentinel value) */

  /* Navigation */
  ACTION_CURSOR_MOVE,        /* Move cursor by delta */
  ACTION_CURSOR_GOTO,        /* Go to specific row */
  ACTION_PAGE_UP,            /* Page up */
  ACTION_PAGE_DOWN,          /* Page down */
  ACTION_HOME,               /* Go to first row */
  ACTION_END,                /* Go to last row */
  ACTION_COLUMN_FIRST,       /* Go to first column */
  ACTION_COLUMN_LAST,        /* Go to last column */

  /* Cell Editing */
  ACTION_EDIT_START,         /* Start inline editing */
  ACTION_EDIT_START_MODAL,   /* Start modal editing */
  ACTION_EDIT_CONFIRM,       /* Confirm edit */
  ACTION_EDIT_CANCEL,        /* Cancel edit */
  ACTION_EDIT_INPUT,         /* Input character while editing */
  ACTION_EDIT_BACKSPACE,     /* Backspace while editing */
  ACTION_EDIT_DELETE,        /* Delete while editing */
  ACTION_EDIT_CURSOR_LEFT,   /* Move edit cursor left */
  ACTION_EDIT_CURSOR_RIGHT,  /* Move edit cursor right */
  ACTION_EDIT_CURSOR_HOME,   /* Move edit cursor to start */
  ACTION_EDIT_CURSOR_END,    /* Move edit cursor to end */

  /* Cell Operations */
  ACTION_CELL_SET_NULL,      /* Set current cell to NULL */
  ACTION_CELL_SET_EMPTY,     /* Set current cell to empty string */
  ACTION_ROW_DELETE,         /* Delete current row */

  /* Workspace/Tab Management */
  ACTION_WORKSPACE_NEXT,     /* Switch to next workspace */
  ACTION_WORKSPACE_PREV,     /* Switch to previous workspace */
  ACTION_WORKSPACE_SWITCH,   /* Switch to specific workspace */
  ACTION_WORKSPACE_CREATE,   /* Create new workspace for table */
  ACTION_WORKSPACE_CREATE_QUERY, /* Create new query workspace */
  ACTION_WORKSPACE_CLOSE,    /* Close current workspace */

  /* Sidebar */
  ACTION_SIDEBAR_TOGGLE,     /* Toggle sidebar visibility */
  ACTION_SIDEBAR_FOCUS,      /* Focus sidebar */
  ACTION_SIDEBAR_UNFOCUS,    /* Unfocus sidebar (back to table) */
  ACTION_SIDEBAR_MOVE,       /* Move highlight in sidebar */
  ACTION_SIDEBAR_SELECT,     /* Select highlighted table */
  ACTION_SIDEBAR_SELECT_NEW_TAB, /* Select table in new tab */
  ACTION_SIDEBAR_FILTER_START,   /* Start typing filter */
  ACTION_SIDEBAR_FILTER_INPUT,   /* Input to filter */
  ACTION_SIDEBAR_FILTER_CLEAR,   /* Clear filter */
  ACTION_SIDEBAR_FILTER_STOP,    /* Stop filtering */

  /* Table Filters Panel */
  ACTION_FILTERS_TOGGLE,     /* Toggle filters panel */
  ACTION_FILTERS_FOCUS,      /* Focus filters panel */
  ACTION_FILTERS_UNFOCUS,    /* Unfocus filters panel */
  ACTION_FILTERS_MOVE,       /* Move cursor in filters */
  ACTION_FILTERS_ADD,        /* Add new filter */
  ACTION_FILTERS_REMOVE,     /* Remove filter at cursor */
  ACTION_FILTERS_CLEAR,      /* Clear all filters */
  ACTION_FILTERS_EDIT_START, /* Start editing filter field */
  ACTION_FILTERS_EDIT_INPUT, /* Input to filter edit */
  ACTION_FILTERS_EDIT_CONFIRM, /* Confirm filter edit */
  ACTION_FILTERS_EDIT_CANCEL,  /* Cancel filter edit */
  ACTION_FILTERS_APPLY,      /* Apply filters (reload data) */

  /* Query Editor */
  ACTION_QUERY_INPUT,        /* Input character to query */
  ACTION_QUERY_BACKSPACE,    /* Backspace in query */
  ACTION_QUERY_DELETE,       /* Delete in query */
  ACTION_QUERY_NEWLINE,      /* New line in query */
  ACTION_QUERY_CURSOR_MOVE,  /* Move cursor in query */
  ACTION_QUERY_EXECUTE,      /* Execute query at cursor */
  ACTION_QUERY_EXECUTE_ALL,  /* Execute all queries */
  ACTION_QUERY_EXECUTE_TXN,  /* Execute all in transaction */
  ACTION_QUERY_FOCUS_RESULTS, /* Focus query results */
  ACTION_QUERY_FOCUS_EDITOR, /* Focus query editor */

  /* Connection */
  ACTION_CONNECT,            /* Connect to database */
  ACTION_DISCONNECT,         /* Disconnect from database */

  /* Data Loading */
  ACTION_TABLE_LOAD,         /* Load table data */
  ACTION_TABLE_REFRESH,      /* Refresh current table */
  ACTION_DATA_LOAD_MORE,     /* Load more rows (pagination) */
  ACTION_DATA_LOAD_PREV,     /* Load previous rows */

  /* UI Toggles */
  ACTION_TOGGLE_HEADER,      /* Toggle header bar */
  ACTION_TOGGLE_STATUS,      /* Toggle status bar */

  /* Dialogs (UI will handle these, but core tracks state) */
  ACTION_SHOW_SCHEMA,        /* Show schema dialog */
  ACTION_SHOW_GOTO,          /* Show goto dialog */
  ACTION_SHOW_CONNECT,       /* Show connect dialog */
  ACTION_SHOW_HELP,          /* Show help dialog */

  /* Application */
  ACTION_QUIT,               /* Quit application */
  ACTION_QUIT_FORCE,         /* Quit without confirmation */

} ActionType;

/* ============================================================================
 * Action Structure
 * ============================================================================
 * Actions carry optional parameters depending on type.
 */
typedef struct {
  ActionType type;

  union {
    /* ACTION_CURSOR_MOVE */
    struct {
      int row_delta;
      int col_delta;
    } cursor_move;

    /* ACTION_CURSOR_GOTO */
    struct {
      size_t row;
    } cursor_goto;

    /* ACTION_EDIT_INPUT, ACTION_QUERY_INPUT, ACTION_SIDEBAR_FILTER_INPUT,
       ACTION_FILTERS_EDIT_INPUT */
    struct {
      int ch;              /* Character code */
    } input;

    /* ACTION_WORKSPACE_SWITCH */
    struct {
      size_t index;
    } workspace_switch;

    /* ACTION_WORKSPACE_CREATE */
    struct {
      size_t table_index;
    } workspace_create;

    /* ACTION_SIDEBAR_MOVE, ACTION_FILTERS_MOVE */
    struct {
      int delta;           /* +1 down, -1 up */
    } move;

    /* ACTION_QUERY_CURSOR_MOVE */
    struct {
      int row_delta;
      int col_delta;
    } query_cursor;

    /* ACTION_CONNECT */
    struct {
      const char *connstr;
    } connect;

    /* ACTION_TABLE_LOAD */
    struct {
      const char *table_name;
      size_t table_index;
    } table_load;
  };
} Action;

/* ============================================================================
 * Action Dispatch
 * ============================================================================
 */

/*
 * Dispatch an action to the core.
 *
 * This is the main entry point for UI to communicate with core logic.
 * The function processes the action, updates state, and returns flags
 * indicating what changed (for UI to know what to redraw).
 *
 * Parameters:
 *   state  - The TUI state (will access app state through state->app)
 *   action - The action to perform
 *
 * Returns:
 *   ChangeFlags indicating what was modified
 *
 * Note: Some actions may trigger async operations. The UI should continue
 * its event loop and poll for completion.
 */
ChangeFlags app_dispatch(struct TuiState *state, const Action *action);

/* ============================================================================
 * Action Helpers - Convenient constructors
 * ============================================================================
 */

/* Navigation */
static inline Action action_cursor_move(int row_delta, int col_delta) {
  return (Action){
      .type = ACTION_CURSOR_MOVE,
      .cursor_move = {.row_delta = row_delta, .col_delta = col_delta}};
}

static inline Action action_cursor_goto(size_t row) {
  return (Action){.type = ACTION_CURSOR_GOTO, .cursor_goto = {.row = row}};
}

static inline Action action_page_up(void) {
  return (Action){.type = ACTION_PAGE_UP};
}

static inline Action action_page_down(void) {
  return (Action){.type = ACTION_PAGE_DOWN};
}

static inline Action action_home(void) {
  return (Action){.type = ACTION_HOME};
}

static inline Action action_end(void) {
  return (Action){.type = ACTION_END};
}

static inline Action action_column_first(void) {
  return (Action){.type = ACTION_COLUMN_FIRST};
}

static inline Action action_column_last(void) {
  return (Action){.type = ACTION_COLUMN_LAST};
}

/* Editing */
static inline Action action_edit_start(void) {
  return (Action){.type = ACTION_EDIT_START};
}

static inline Action action_edit_start_modal(void) {
  return (Action){.type = ACTION_EDIT_START_MODAL};
}

static inline Action action_edit_confirm(void) {
  return (Action){.type = ACTION_EDIT_CONFIRM};
}

static inline Action action_edit_cancel(void) {
  return (Action){.type = ACTION_EDIT_CANCEL};
}

static inline Action action_edit_input(int ch) {
  return (Action){.type = ACTION_EDIT_INPUT, .input = {.ch = ch}};
}

static inline Action action_cell_set_null(void) {
  return (Action){.type = ACTION_CELL_SET_NULL};
}

static inline Action action_cell_set_empty(void) {
  return (Action){.type = ACTION_CELL_SET_EMPTY};
}

static inline Action action_row_delete(void) {
  return (Action){.type = ACTION_ROW_DELETE};
}

/* Workspaces */
static inline Action action_workspace_next(void) {
  return (Action){.type = ACTION_WORKSPACE_NEXT};
}

static inline Action action_workspace_prev(void) {
  return (Action){.type = ACTION_WORKSPACE_PREV};
}

static inline Action action_workspace_switch(size_t index) {
  return (Action){.type = ACTION_WORKSPACE_SWITCH,
                  .workspace_switch = {.index = index}};
}

static inline Action action_workspace_create(size_t table_index) {
  return (Action){.type = ACTION_WORKSPACE_CREATE,
                  .workspace_create = {.table_index = table_index}};
}

static inline Action action_workspace_create_query(void) {
  return (Action){.type = ACTION_WORKSPACE_CREATE_QUERY};
}

static inline Action action_workspace_close(void) {
  return (Action){.type = ACTION_WORKSPACE_CLOSE};
}

/* Sidebar */
static inline Action action_sidebar_toggle(void) {
  return (Action){.type = ACTION_SIDEBAR_TOGGLE};
}

static inline Action action_sidebar_focus(void) {
  return (Action){.type = ACTION_SIDEBAR_FOCUS};
}

static inline Action action_sidebar_unfocus(void) {
  return (Action){.type = ACTION_SIDEBAR_UNFOCUS};
}

static inline Action action_sidebar_move(int delta) {
  return (Action){.type = ACTION_SIDEBAR_MOVE, .move = {.delta = delta}};
}

static inline Action action_sidebar_select(void) {
  return (Action){.type = ACTION_SIDEBAR_SELECT};
}

static inline Action action_sidebar_select_new_tab(void) {
  return (Action){.type = ACTION_SIDEBAR_SELECT_NEW_TAB};
}

/* Filters */
static inline Action action_filters_toggle(void) {
  return (Action){.type = ACTION_FILTERS_TOGGLE};
}

static inline Action action_filters_focus(void) {
  return (Action){.type = ACTION_FILTERS_FOCUS};
}

static inline Action action_filters_unfocus(void) {
  return (Action){.type = ACTION_FILTERS_UNFOCUS};
}

static inline Action action_filters_add(void) {
  return (Action){.type = ACTION_FILTERS_ADD};
}

static inline Action action_filters_remove(void) {
  return (Action){.type = ACTION_FILTERS_REMOVE};
}

static inline Action action_filters_clear(void) {
  return (Action){.type = ACTION_FILTERS_CLEAR};
}

/* Query */
static inline Action action_query_input(int ch) {
  return (Action){.type = ACTION_QUERY_INPUT, .input = {.ch = ch}};
}

static inline Action action_query_execute(void) {
  return (Action){.type = ACTION_QUERY_EXECUTE};
}

static inline Action action_query_execute_all(void) {
  return (Action){.type = ACTION_QUERY_EXECUTE_ALL};
}

/* Connection */
static inline Action action_connect(const char *connstr) {
  return (Action){.type = ACTION_CONNECT, .connect = {.connstr = connstr}};
}

static inline Action action_disconnect(void) {
  return (Action){.type = ACTION_DISCONNECT};
}

/* UI */
static inline Action action_toggle_header(void) {
  return (Action){.type = ACTION_TOGGLE_HEADER};
}

static inline Action action_toggle_status(void) {
  return (Action){.type = ACTION_TOGGLE_STATUS};
}

/* Dialogs */
static inline Action action_show_schema(void) {
  return (Action){.type = ACTION_SHOW_SCHEMA};
}

static inline Action action_show_goto(void) {
  return (Action){.type = ACTION_SHOW_GOTO};
}

static inline Action action_show_connect(void) {
  return (Action){.type = ACTION_SHOW_CONNECT};
}

static inline Action action_show_help(void) {
  return (Action){.type = ACTION_SHOW_HELP};
}

/* Application */
static inline Action action_quit(void) {
  return (Action){.type = ACTION_QUIT};
}

static inline Action action_quit_force(void) {
  return (Action){.type = ACTION_QUIT_FORCE};
}

#endif /* LACE_CORE_ACTIONS_H */
