/*
 * lace - Database Viewer and Manager
 * TUI interface
 */

#ifndef LACE_TUI_H
#define LACE_TUI_H

#include <stdbool.h>
#include <ncurses.h>
#include "../db/db.h"

/* Color pairs */
#define COLOR_HEADER     1
#define COLOR_SELECTED   2
#define COLOR_STATUS     3
#define COLOR_ERROR      4
#define COLOR_BORDER     5
#define COLOR_TITLE      6
#define COLOR_NULL       7
#define COLOR_NUMBER     8
#define COLOR_EDIT       9

/* Sidebar width */
#define SIDEBAR_WIDTH 20

/* Maximum number of tabs/workspaces */
#define MAX_WORKSPACES 10

/* Tab bar height */
#define TAB_BAR_HEIGHT 1

/* Workspace - holds per-tab state */
typedef struct {
    size_t table_index;      /* Index into tables array */
    char *table_name;        /* Table name (for display) */

    /* View data */
    ResultSet *data;
    TableSchema *schema;

    /* Cursor position */
    size_t cursor_row;
    size_t cursor_col;
    size_t scroll_row;
    size_t scroll_col;

    /* Pagination */
    size_t total_rows;
    size_t loaded_offset;
    size_t loaded_count;

    /* Column widths */
    int *col_widths;
    size_t num_col_widths;

    /* Is this workspace active/used */
    bool active;
} Workspace;

/* TUI state */
typedef struct {
    /* Windows */
    WINDOW *main_win;
    WINDOW *status_win;
    WINDOW *header_win;
    WINDOW *sidebar_win;
    WINDOW *tab_win;

    /* Database */
    DbConnection *conn;
    char **tables;
    size_t num_tables;

    /* Workspaces/Tabs */
    Workspace workspaces[MAX_WORKSPACES];
    size_t num_workspaces;
    size_t current_workspace;

    /* Convenience pointers to current workspace data */
    size_t current_table;    /* Alias for workspaces[current_workspace].table_index */
    ResultSet *data;         /* Alias for workspaces[current_workspace].data */
    TableSchema *schema;     /* Alias for workspaces[current_workspace].schema */
    size_t cursor_row;
    size_t cursor_col;
    size_t scroll_row;
    size_t scroll_col;
    size_t total_rows;
    size_t loaded_offset;
    size_t loaded_count;
    int *col_widths;
    size_t num_col_widths;

    /* Page size (shared) */
    size_t page_size;

    /* Dimensions */
    int term_rows;
    int term_cols;
    int content_rows;
    int content_cols;

    /* Mode */
    bool editing;
    char *edit_buffer;
    size_t edit_pos;

    /* Sidebar */
    bool sidebar_visible;
    size_t sidebar_highlight;  /* Currently highlighted table in sidebar */
    size_t sidebar_scroll;     /* Scroll offset for sidebar */
    bool sidebar_focused;      /* Whether sidebar has focus */
    bool sidebar_filter_active; /* Whether filter input is active */
    char sidebar_filter[64];   /* Filter string */
    size_t sidebar_filter_len; /* Filter string length */

    /* Status message */
    char *status_msg;
    bool status_is_error;

    /* Running */
    bool running;
} TuiState;

/* Initialize TUI */
bool tui_init(TuiState *state);

/* Cleanup TUI */
void tui_cleanup(TuiState *state);

/* Connect to database */
bool tui_connect(TuiState *state, const char *connstr);

/* Disconnect */
void tui_disconnect(TuiState *state);

/* Main loop */
void tui_run(TuiState *state);

/* Refresh display */
void tui_refresh(TuiState *state);

/* Draw functions */
void tui_draw_header(TuiState *state);
void tui_draw_table(TuiState *state);
void tui_draw_status(TuiState *state);
void tui_draw_sidebar(TuiState *state);
void tui_draw_tabs(TuiState *state);

/* Load data */
bool tui_load_tables(TuiState *state);
bool tui_load_table_data(TuiState *state, const char *table);
bool tui_load_schema(TuiState *state, const char *table);

/* Navigation */
void tui_move_cursor(TuiState *state, int row_delta, int col_delta);
void tui_page_up(TuiState *state);
void tui_page_down(TuiState *state);
void tui_home(TuiState *state);
void tui_end(TuiState *state);

/* Actions */
void tui_next_table(TuiState *state);
void tui_prev_table(TuiState *state);
void tui_show_schema(TuiState *state);
void tui_show_connect_dialog(TuiState *state);
void tui_show_table_selector(TuiState *state);
void tui_show_help(TuiState *state);

/* Status messages */
void tui_set_status(TuiState *state, const char *fmt, ...);
void tui_set_error(TuiState *state, const char *fmt, ...);

/* Utility */
int tui_get_column_width(TuiState *state, size_t col);
void tui_calculate_column_widths(TuiState *state);

#endif /* LACE_TUI_H */
