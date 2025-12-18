/*
 * lace - Database Viewer and Manager
 * TUI implementation
 */

#include "tui.h"
#include "views/connect_view.h"
#include "views/editor_view.h"
#include "../util/str.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <locale.h>
#include <ctype.h>
#include <menu.h>
#include <termios.h>
#include <unistd.h>

#define MIN_COL_WIDTH 4
#define MAX_COL_WIDTH 40
#define DEFAULT_COL_WIDTH 15
#define PAGE_SIZE 500
#define LOAD_THRESHOLD 50      /* Load more when within this many rows of edge */
#define MAX_LOADED_PAGES 5     /* Maximum pages to keep in memory */
#define TRIM_DISTANCE_PAGES 2  /* Trim data farther than this from cursor */

/* Forward declarations */
static void tui_confirm_edit(TuiState *state);
static void tui_trim_loaded_data(TuiState *state);
static size_t get_filtered_table_index(TuiState *state, size_t filtered_idx);

/*
 * Translate keyboard input from non-Latin layouts to Latin equivalents
 * based on physical key position. This allows hotkeys to work regardless
 * of the active keyboard layout (e.g., Russian, Ukrainian, etc.)
 */
static int translate_key(int ch) {
    /* Russian layout (ЙЦУКЕН) -> QWERTY mapping */
    switch (ch) {
        /* Lowercase Cyrillic */
        case 0x0439: return 'q';  /* й */
        case 0x0446: return 'w';  /* ц */
        case 0x0443: return 'e';  /* у */
        case 0x043A: return 'r';  /* к */
        case 0x0435: return 't';  /* е */
        case 0x043D: return 'y';  /* н */
        case 0x0433: return 'u';  /* г */
        case 0x0448: return 'i';  /* ш */
        case 0x0449: return 'o';  /* щ */
        case 0x0437: return 'p';  /* з */
        case 0x0445: return '[';  /* х */
        case 0x044A: return ']';  /* ъ */
        case 0x0444: return 'a';  /* ф */
        case 0x044B: return 's';  /* ы */
        case 0x0432: return 'd';  /* в */
        case 0x0430: return 'f';  /* а */
        case 0x043F: return 'g';  /* п */
        case 0x0440: return 'h';  /* р */
        case 0x043E: return 'j';  /* о */
        case 0x043B: return 'k';  /* л */
        case 0x0434: return 'l';  /* д */
        case 0x044F: return 'z';  /* я */
        case 0x0447: return 'x';  /* ч */
        case 0x0441: return 'c';  /* с */
        case 0x043C: return 'v';  /* м */
        case 0x0438: return 'b';  /* и */
        case 0x0442: return 'n';  /* т */
        case 0x044C: return 'm';  /* ь */
        case 0x0436: return ';';  /* ж */
        case 0x044D: return '\''; /* э */
        case 0x0451: return '`';  /* ё */

        /* Uppercase Cyrillic */
        case 0x0419: return 'Q';  /* Й */
        case 0x0426: return 'W';  /* Ц */
        case 0x0423: return 'E';  /* У */
        case 0x041A: return 'R';  /* К */
        case 0x0415: return 'T';  /* Е */
        case 0x041D: return 'Y';  /* Н */
        case 0x0413: return 'U';  /* Г */
        case 0x0428: return 'I';  /* Ш */
        case 0x0429: return 'O';  /* Щ */
        case 0x0417: return 'P';  /* З */
        case 0x0425: return '{';  /* Х */
        case 0x042A: return '}';  /* Ъ */
        case 0x0424: return 'A';  /* Ф */
        case 0x042B: return 'S';  /* Ы */
        case 0x0412: return 'D';  /* В */
        case 0x0410: return 'F';  /* А */
        case 0x041F: return 'G';  /* П */
        case 0x0420: return 'H';  /* Р */
        case 0x041E: return 'J';  /* О */
        case 0x041B: return 'K';  /* Л */
        case 0x0414: return 'L';  /* Д */
        case 0x042F: return 'Z';  /* Я */
        case 0x0427: return 'X';  /* Ч */
        case 0x0421: return 'C';  /* С */
        case 0x041C: return 'V';  /* М */
        case 0x0418: return 'B';  /* И */
        case 0x0422: return 'N';  /* Т */
        case 0x042C: return 'M';  /* Ь */
        case 0x0416: return ':';  /* Ж */
        case 0x042D: return '"';  /* Э */
        case 0x0401: return '~';  /* Ё */

        default: return ch;
    }
}

/*
 * Sanitize string for single-line cell display.
 * Replaces newlines, tabs, and control characters with safe alternatives.
 * Returns a newly allocated string that must be freed.
 */
static char *sanitize_for_display(const char *str) {
    if (!str) return NULL;

    size_t len = strlen(str);
    char *result = malloc(len + 1);
    if (!result) return NULL;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c == '\n' || c == '\r') {
            result[i] = ' ';  /* Replace newlines with space */
        } else if (c == '\t') {
            result[i] = ' ';  /* Replace tabs with space */
        } else if (c < 32 && c != 0) {
            result[i] = '?';  /* Replace other control chars */
        } else {
            result[i] = str[i];
        }
    }
    result[len] = '\0';
    return result;
}

/* Case-insensitive substring search */
static const char *str_istr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return haystack;

    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

bool tui_init(TuiState *state) {
    if (!state) return false;

    memset(state, 0, sizeof(TuiState));

    /* Set locale for UTF-8 support */
    setlocale(LC_ALL, "");

    /* Initialize ncurses */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);  /* Hide cursor */

    /* Define Ctrl+Home and Ctrl+End key sequences */
    define_key("\033[1;5H", KEY_F(61));   /* Ctrl+Home - xterm */
    define_key("\033[7^", KEY_F(61));     /* Ctrl+Home - rxvt */
    define_key("\033[1;5F", KEY_F(62));   /* Ctrl+End - xterm */
    define_key("\033[8^", KEY_F(62));     /* Ctrl+End - rxvt */

    /* Enable mouse support */
    mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED, NULL);
    mouseinterval(300);  /* Double-click interval in ms */

    /* Disable XON/XOFF flow control so Ctrl+S works */
    struct termios term;
    if (tcgetattr(STDIN_FILENO, &term) == 0) {
        term.c_iflag &= ~(IXON | IXOFF);
        tcsetattr(STDIN_FILENO, TCSANOW, &term);
    }

    /* Initialize colors if available */
    if (has_colors()) {
        start_color();
        use_default_colors();

        init_pair(COLOR_HEADER, COLOR_BLACK, COLOR_CYAN);
        init_pair(COLOR_SELECTED, COLOR_BLACK, COLOR_WHITE);
        init_pair(COLOR_STATUS, COLOR_CYAN, -1);
        init_pair(COLOR_ERROR, COLOR_WHITE, COLOR_RED);
        init_pair(COLOR_BORDER, COLOR_CYAN, -1);
        init_pair(COLOR_TITLE, COLOR_YELLOW, -1);
        init_pair(COLOR_NULL, COLOR_MAGENTA, -1);
        init_pair(COLOR_NUMBER, COLOR_GREEN, -1);
        init_pair(COLOR_EDIT, COLOR_BLACK, COLOR_YELLOW);
    }

    /* Get terminal size */
    getmaxyx(stdscr, state->term_rows, state->term_cols);

    /* Sidebar hidden by default (shown when connected) */
    state->sidebar_visible = false;

    /* Initialize sidebar scroll animation */
    state->sidebar_name_scroll = 0;
    state->sidebar_name_scroll_dir = 1;
    state->sidebar_name_scroll_delay = 0;
    state->sidebar_last_highlight = (size_t)-1;

    /* Initialize workspaces */
    state->num_workspaces = 0;
    state->current_workspace = 0;
    for (size_t i = 0; i < MAX_WORKSPACES; i++) {
        memset(&state->workspaces[i], 0, sizeof(Workspace));
    }

    /* Create windows (without sidebar initially) */
    /* Layout: header (1) + tab bar (1) + main area + status (1) */
    state->header_win = newwin(1, state->term_cols, 0, 0);
    state->tab_win = newwin(1, state->term_cols, 1, 0);
    state->sidebar_win = NULL;
    state->main_win = newwin(state->term_rows - 3, state->term_cols, 2, 0);
    state->status_win = newwin(1, state->term_cols, state->term_rows - 1, 0);

    if (!state->header_win || !state->tab_win || !state->main_win || !state->status_win) {
        tui_cleanup(state);
        return false;
    }

    /* Calculate content area */
    state->content_rows = state->term_rows - 4;  /* Header + table header + status + border */
    state->content_cols = state->term_cols - 2;  /* Borders */

    scrollok(state->main_win, FALSE);
    keypad(state->main_win, TRUE);
    if (state->sidebar_win) {
        keypad(state->sidebar_win, TRUE);
    }

    state->running = true;

    return true;
}

void tui_cleanup(TuiState *state) {
    if (!state) return;

    if (state->header_win) delwin(state->header_win);
    if (state->tab_win) delwin(state->tab_win);
    if (state->main_win) delwin(state->main_win);
    if (state->status_win) delwin(state->status_win);
    if (state->sidebar_win) delwin(state->sidebar_win);

    endwin();

    /* tui_disconnect handles workspace cleanup */
    tui_disconnect(state);

    free(state->status_msg);
    free(state->edit_buffer);
}

/* Recreate windows when sidebar is toggled */
static void tui_recreate_windows(TuiState *state) {
    if (!state) return;

    /* Delete existing windows */
    if (state->main_win) delwin(state->main_win);
    if (state->sidebar_win) delwin(state->sidebar_win);
    if (state->tab_win) delwin(state->tab_win);

    int main_start_x = 0;
    int main_width = state->term_cols;
    int main_start_y = 2;  /* After header and tab bar */
    int main_height = state->term_rows - 3;  /* Total - header - tab bar - status */

    /* Recreate tab window */
    state->tab_win = newwin(1, state->term_cols, 1, 0);

    if (state->sidebar_visible) {
        /* Create sidebar window (starts after tab bar) */
        state->sidebar_win = newwin(main_height, SIDEBAR_WIDTH, main_start_y, 0);
        if (state->sidebar_win) {
            keypad(state->sidebar_win, TRUE);
            wtimeout(state->sidebar_win, 80);  /* For scroll animation */
        }
        main_start_x = SIDEBAR_WIDTH;
        main_width = state->term_cols - SIDEBAR_WIDTH;
    } else {
        state->sidebar_win = NULL;
    }

    /* Create main window */
    state->main_win = newwin(main_height, main_width, main_start_y, main_start_x);
    if (state->main_win) {
        scrollok(state->main_win, FALSE);
        keypad(state->main_win, TRUE);
    }

    /* Update content dimensions */
    state->content_cols = main_width - 2;
}

/* Save current TUI state to workspace */
static void workspace_save(TuiState *state) {
    if (!state || state->num_workspaces == 0) return;

    Workspace *ws = &state->workspaces[state->current_workspace];

    /* Save cursor and scroll positions */
    ws->cursor_row = state->cursor_row;
    ws->cursor_col = state->cursor_col;
    ws->scroll_row = state->scroll_row;
    ws->scroll_col = state->scroll_col;

    /* Save pagination state */
    ws->total_rows = state->total_rows;
    ws->loaded_offset = state->loaded_offset;
    ws->loaded_count = state->loaded_count;

    /* Data and schema pointers are already stored in workspace */
    ws->data = state->data;
    ws->schema = state->schema;

    /* Save column widths */
    ws->col_widths = state->col_widths;
    ws->num_col_widths = state->num_col_widths;
}

/* Restore TUI state from workspace */
static void workspace_restore(TuiState *state) {
    if (!state || state->num_workspaces == 0) return;

    Workspace *ws = &state->workspaces[state->current_workspace];

    /* Restore cursor and scroll positions */
    state->cursor_row = ws->cursor_row;
    state->cursor_col = ws->cursor_col;
    state->scroll_row = ws->scroll_row;
    state->scroll_col = ws->scroll_col;

    /* Restore pagination state */
    state->total_rows = ws->total_rows;
    state->loaded_offset = ws->loaded_offset;
    state->loaded_count = ws->loaded_count;

    /* Restore data and schema */
    state->data = ws->data;
    state->schema = ws->schema;
    state->current_table = ws->table_index;

    /* Restore column widths */
    state->col_widths = ws->col_widths;
    state->num_col_widths = ws->num_col_widths;
}

/* Switch to a different workspace */
static void workspace_switch(TuiState *state, size_t index) {
    if (!state || index >= state->num_workspaces) return;
    if (index == state->current_workspace) return;

    /* Save current workspace state */
    workspace_save(state);

    /* Switch to new workspace */
    state->current_workspace = index;

    /* Restore new workspace state */
    workspace_restore(state);

    /* Clear status message */
    free(state->status_msg);
    state->status_msg = NULL;
    state->status_is_error = false;
}

/* Create a new workspace for a table */
static bool workspace_create(TuiState *state, size_t table_index) {
    if (!state || table_index >= state->num_tables) return false;
    if (state->num_workspaces >= MAX_WORKSPACES) {
        tui_set_error(state, "Maximum %d tabs reached", MAX_WORKSPACES);
        return false;
    }

    /* Save current workspace first */
    if (state->num_workspaces > 0) {
        workspace_save(state);
    }

    /* Create new workspace */
    size_t new_idx = state->num_workspaces;
    Workspace *ws = &state->workspaces[new_idx];
    memset(ws, 0, sizeof(Workspace));

    ws->active = true;
    ws->table_index = table_index;
    ws->table_name = str_dup(state->tables[table_index]);

    state->num_workspaces++;
    state->current_workspace = new_idx;

    /* Clear TUI state for new workspace */
    state->data = NULL;
    state->schema = NULL;
    state->col_widths = NULL;
    state->num_col_widths = 0;
    state->cursor_row = 0;
    state->cursor_col = 0;
    state->scroll_row = 0;
    state->scroll_col = 0;

    /* Load the table data */
    if (!tui_load_table_data(state, state->tables[table_index])) {
        /* Failed - remove the workspace */
        ws->active = false;
        free(ws->table_name);
        ws->table_name = NULL;
        state->num_workspaces--;

        /* Restore previous workspace */
        if (state->num_workspaces > 0) {
            state->current_workspace = state->num_workspaces - 1;
            workspace_restore(state);
        }
        return false;
    }

    /* Save the loaded data to workspace */
    ws->data = state->data;
    ws->schema = state->schema;
    ws->col_widths = state->col_widths;
    ws->num_col_widths = state->num_col_widths;
    ws->total_rows = state->total_rows;
    ws->loaded_offset = state->loaded_offset;
    ws->loaded_count = state->loaded_count;

    state->current_table = table_index;

    return true;
}

/* Close current workspace */
static void workspace_close(TuiState *state) {
    if (!state || state->num_workspaces == 0) return;

    Workspace *ws = &state->workspaces[state->current_workspace];

    /* Free workspace data */
    free(ws->table_name);
    db_result_free(ws->data);
    db_schema_free(ws->schema);
    free(ws->col_widths);
    memset(ws, 0, sizeof(Workspace));

    /* Shift remaining workspaces down */
    for (size_t i = state->current_workspace; i < state->num_workspaces - 1; i++) {
        state->workspaces[i] = state->workspaces[i + 1];
    }
    memset(&state->workspaces[state->num_workspaces - 1], 0, sizeof(Workspace));

    state->num_workspaces--;

    if (state->num_workspaces == 0) {
        /* Last tab closed - clear state and focus sidebar */
        state->current_workspace = 0;
        state->data = NULL;
        state->schema = NULL;
        state->col_widths = NULL;
        state->num_col_widths = 0;
        state->cursor_row = 0;
        state->cursor_col = 0;
        state->scroll_row = 0;
        state->scroll_col = 0;
        state->total_rows = 0;
        state->loaded_offset = 0;
        state->loaded_count = 0;
        state->sidebar_focused = true;
        state->sidebar_highlight = 0;
    } else {
        /* Adjust current workspace index */
        if (state->current_workspace >= state->num_workspaces) {
            state->current_workspace = state->num_workspaces - 1;
        }

        /* Restore the now-current workspace */
        workspace_restore(state);
    }
}

/* Draw tab bar */
void tui_draw_tabs(TuiState *state) {
    if (!state || !state->tab_win) return;

    werase(state->tab_win);
    wbkgd(state->tab_win, COLOR_PAIR(COLOR_BORDER));

    int x = 0;

    for (size_t i = 0; i < state->num_workspaces; i++) {
        Workspace *ws = &state->workspaces[i];
        if (!ws->active) continue;

        const char *name = ws->table_name ? ws->table_name : "?";
        int tab_width = (int)strlen(name) + 4;  /* " name  " with padding */

        if (x + tab_width > state->term_cols) break;

        if (i == state->current_workspace) {
            /* Current tab - highlighted */
            wattron(state->tab_win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
            mvwprintw(state->tab_win, 0, x, " %s ", name);
            wattroff(state->tab_win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            /* Inactive tab */
            mvwprintw(state->tab_win, 0, x, " %s ", name);
        }

        x += tab_width;

        /* Tab separator */
        if (i < state->num_workspaces - 1 && x < state->term_cols) {
            mvwaddch(state->tab_win, 0, x - 1, ACS_VLINE);
        }
    }

    /* Show hint for new tab if space and sidebar visible */
    if (state->num_workspaces < MAX_WORKSPACES && state->sidebar_focused) {
        const char *hint = "[+] New tab";
        int hint_len = (int)strlen(hint);
        if (state->term_cols - x > hint_len + 2) {
            wattron(state->tab_win, A_DIM);
            mvwprintw(state->tab_win, 0, state->term_cols - hint_len - 1, "%s", hint);
            wattroff(state->tab_win, A_DIM);
        }
    }

    wrefresh(state->tab_win);
}

bool tui_connect(TuiState *state, const char *connstr) {
    if (!state || !connstr) return false;

    char *err = NULL;
    state->conn = db_connect(connstr, &err);

    if (!state->conn) {
        tui_set_error(state, "Connection failed: %s", err ? err : "Unknown error");
        free(err);
        return false;
    }

    /* Show sidebar on successful connection */
    if (!state->sidebar_visible) {
        state->sidebar_visible = true;
        tui_recreate_windows(state);
        tui_calculate_column_widths(state);
    }

    tui_set_status(state, "Connected to %s", state->conn->database);
    return tui_load_tables(state);
}

void tui_disconnect(TuiState *state) {
    if (!state) return;

    /* Clean up all workspaces first (they own data/schema/col_widths) */
    for (size_t i = 0; i < MAX_WORKSPACES; i++) {
        Workspace *ws = &state->workspaces[i];
        if (ws->active) {
            free(ws->table_name);
            db_result_free(ws->data);
            db_schema_free(ws->schema);
            free(ws->col_widths);
            memset(ws, 0, sizeof(Workspace));
        }
    }
    state->num_workspaces = 0;
    state->current_workspace = 0;

    /* Clear convenience pointers (data was freed with workspaces) */
    state->data = NULL;
    state->schema = NULL;
    state->col_widths = NULL;
    state->num_col_widths = 0;

    if (state->tables) {
        for (size_t i = 0; i < state->num_tables; i++) {
            free(state->tables[i]);
        }
        free(state->tables);
        state->tables = NULL;
        state->num_tables = 0;
    }

    if (state->conn) {
        db_disconnect(state->conn);
        state->conn = NULL;
    }
}

bool tui_load_tables(TuiState *state) {
    if (!state || !state->conn) return false;

    /* Free old tables */
    if (state->tables) {
        for (size_t i = 0; i < state->num_tables; i++) {
            free(state->tables[i]);
        }
        free(state->tables);
    }

    char *err = NULL;
    state->tables = db_list_tables(state->conn, &state->num_tables, &err);

    if (!state->tables) {
        tui_set_error(state, "Failed to list tables: %s", err ? err : "Unknown error");
        free(err);
        return false;
    }

    state->current_table = 0;
    state->sidebar_highlight = 0;

    if (state->num_tables == 0) {
        tui_set_status(state, "No tables found");
        state->sidebar_focused = true;
    } else {
        /* Auto-load first table */
        workspace_create(state, 0);
        state->sidebar_focused = false;
    }

    return true;
}

bool tui_load_table_data(TuiState *state, const char *table) {
    if (!state || !state->conn || !table) return false;

    /* Free old data */
    if (state->data) {
        db_result_free(state->data);
        state->data = NULL;
    }

    if (state->schema) {
        db_schema_free(state->schema);
        state->schema = NULL;
    }

    /* Load schema */
    char *err = NULL;
    state->schema = db_get_table_schema(state->conn, table, &err);
    if (err) {
        tui_set_error(state, "Schema: %s", err);
        free(err);
        /* Continue anyway - we can still show data */
    }

    /* Get total row count */
    int64_t count = db_count_rows(state->conn, table, &err);
    if (count < 0) {
        /* Fallback if COUNT fails */
        count = 0;
        free(err);
        err = NULL;
    }
    state->total_rows = (size_t)count;
    state->page_size = PAGE_SIZE;
    state->loaded_offset = 0;

    /* Load first page of data */
    state->data = db_query_page(state->conn, table, 0, PAGE_SIZE, NULL, false, &err);
    if (!state->data) {
        tui_set_error(state, "Query failed: %s", err ? err : "Unknown error");
        free(err);
        return false;
    }

    state->loaded_count = state->data->num_rows;

    /* Apply schema column names to result set */
    if (state->schema && state->data) {
        size_t min_cols = state->schema->num_columns;
        if (state->data->num_columns < min_cols) {
            min_cols = state->data->num_columns;
        }
        for (size_t i = 0; i < min_cols; i++) {
            if (state->schema->columns[i].name) {
                free(state->data->columns[i].name);
                state->data->columns[i].name = str_dup(state->schema->columns[i].name);
                state->data->columns[i].type = state->schema->columns[i].type;
            }
        }
    }

    /* Reset cursor */
    state->cursor_row = 0;
    state->cursor_col = 0;
    state->scroll_row = 0;
    state->scroll_col = 0;

    /* Calculate column widths */
    tui_calculate_column_widths(state);

    /* Clear any previous status message so column info is shown */
    free(state->status_msg);
    state->status_msg = NULL;
    state->status_is_error = false;
    return true;
}

/* Load more rows at the end of current data */
static bool tui_load_more_rows(TuiState *state) {
    if (!state || !state->conn || !state->data || !state->tables) return false;
    if (state->current_table >= state->num_tables) return false;

    const char *table = state->tables[state->current_table];
    size_t new_offset = state->loaded_offset + state->loaded_count;

    /* Check if there are more rows to load */
    if (new_offset >= state->total_rows) return false;

    char *err = NULL;
    ResultSet *more = db_query_page(state->conn, table, new_offset, PAGE_SIZE, NULL, false, &err);
    if (!more || more->num_rows == 0) {
        if (more) db_result_free(more);
        free(err);
        return false;
    }

    /* Extend existing rows array */
    size_t old_count = state->data->num_rows;
    size_t new_count = old_count + more->num_rows;
    Row *new_rows = realloc(state->data->rows, new_count * sizeof(Row));
    if (!new_rows) {
        db_result_free(more);
        return false;
    }

    state->data->rows = new_rows;

    /* Copy new rows */
    for (size_t i = 0; i < more->num_rows; i++) {
        state->data->rows[old_count + i] = more->rows[i];
        /* Clear source so free doesn't deallocate the cells we moved */
        more->rows[i].cells = NULL;
        more->rows[i].num_cells = 0;
    }

    state->data->num_rows = new_count;
    state->loaded_count = new_count;

    db_result_free(more);

    /* Trim old data to keep memory bounded */
    tui_trim_loaded_data(state);

    tui_set_status(state, "Loaded %zu/%zu rows", state->loaded_count, state->total_rows);
    return true;
}

/* Load rows starting at a specific offset (replaces current data) */
static bool tui_load_rows_at(TuiState *state, size_t offset) {
    if (!state || !state->conn || !state->tables) return false;
    if (state->current_table >= state->num_tables) return false;

    const char *table = state->tables[state->current_table];

    /* Clamp offset */
    if (offset >= state->total_rows) {
        offset = state->total_rows > PAGE_SIZE ? state->total_rows - PAGE_SIZE : 0;
    }

    char *err = NULL;
    ResultSet *data = db_query_page(state->conn, table, offset, PAGE_SIZE, NULL, false, &err);
    if (!data) {
        tui_set_error(state, "Query failed: %s", err ? err : "Unknown error");
        free(err);
        return false;
    }

    /* Free old data but keep schema */
    if (state->data) {
        db_result_free(state->data);
    }
    state->data = data;
    state->loaded_offset = offset;
    state->loaded_count = data->num_rows;

    /* Apply schema column names */
    if (state->schema && state->data) {
        size_t min_cols = state->schema->num_columns;
        if (state->data->num_columns < min_cols) {
            min_cols = state->data->num_columns;
        }
        for (size_t i = 0; i < min_cols; i++) {
            if (state->schema->columns[i].name) {
                free(state->data->columns[i].name);
                state->data->columns[i].name = str_dup(state->schema->columns[i].name);
                state->data->columns[i].type = state->schema->columns[i].type;
            }
        }
    }

    tui_calculate_column_widths(state);
    tui_set_status(state, "Rows %zu-%zu of %zu", offset + 1,
                   offset + state->loaded_count, state->total_rows);
    return true;
}

/* Trim loaded data to keep memory bounded - keeps data within TRIM_DISTANCE_PAGES of cursor */
static void tui_trim_loaded_data(TuiState *state) {
    if (!state || !state->data || state->data->num_rows == 0) return;

    size_t max_rows = MAX_LOADED_PAGES * PAGE_SIZE;
    if (state->loaded_count <= max_rows) return;

    /* Calculate cursor's page within loaded data */
    size_t cursor_page = state->cursor_row / PAGE_SIZE;
    size_t total_pages = (state->loaded_count + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Determine pages to keep: TRIM_DISTANCE_PAGES on each side of cursor */
    size_t keep_start_page = 0;
    size_t keep_end_page = total_pages;

    if (cursor_page > TRIM_DISTANCE_PAGES) {
        keep_start_page = cursor_page - TRIM_DISTANCE_PAGES;
    }
    if (cursor_page + TRIM_DISTANCE_PAGES + 1 < total_pages) {
        keep_end_page = cursor_page + TRIM_DISTANCE_PAGES + 1;
    }

    /* Ensure we don't exceed MAX_LOADED_PAGES */
    size_t pages_to_keep = keep_end_page - keep_start_page;
    if (pages_to_keep > MAX_LOADED_PAGES) {
        /* Trim from the end that's farther from cursor */
        size_t excess = pages_to_keep - MAX_LOADED_PAGES;
        size_t pages_before_cursor = cursor_page - keep_start_page;
        size_t pages_after_cursor = keep_end_page - cursor_page - 1;

        if (pages_before_cursor > pages_after_cursor) {
            keep_start_page += excess;
        } else {
            keep_end_page -= excess;
        }
    }

    /* Convert pages to row indices */
    size_t trim_start = keep_start_page * PAGE_SIZE;
    size_t trim_end = keep_end_page * PAGE_SIZE;
    if (trim_end > state->loaded_count) trim_end = state->loaded_count;

    /* Check if we actually need to trim */
    if (trim_start == 0 && trim_end >= state->loaded_count) return;

    /* Free rows before trim_start */
    for (size_t i = 0; i < trim_start; i++) {
        Row *row = &state->data->rows[i];
        for (size_t j = 0; j < row->num_cells; j++) {
            db_value_free(&row->cells[j]);
        }
        free(row->cells);
    }

    /* Free rows after trim_end */
    for (size_t i = trim_end; i < state->loaded_count; i++) {
        Row *row = &state->data->rows[i];
        for (size_t j = 0; j < row->num_cells; j++) {
            db_value_free(&row->cells[j]);
        }
        free(row->cells);
    }

    /* Move remaining rows to beginning of array */
    size_t new_count = trim_end - trim_start;
    if (trim_start > 0) {
        memmove(state->data->rows, state->data->rows + trim_start,
                new_count * sizeof(Row));
    }

    /* Resize array (optional, realloc to shrink) */
    Row *new_rows = realloc(state->data->rows, new_count * sizeof(Row));
    if (new_rows) {
        state->data->rows = new_rows;
    }
    state->data->num_rows = new_count;

    /* Adjust cursor and scroll positions */
    if (state->cursor_row >= trim_start) {
        state->cursor_row -= trim_start;
    } else {
        state->cursor_row = 0;
    }

    if (state->scroll_row >= trim_start) {
        state->scroll_row -= trim_start;
    } else {
        state->scroll_row = 0;
    }

    /* Update tracking */
    state->loaded_offset += trim_start;
    state->loaded_count = new_count;
}

/* Load previous rows (prepend to current data) */
static bool tui_load_prev_rows(TuiState *state) {
    if (!state || !state->conn || !state->data || !state->tables) return false;
    if (state->current_table >= state->num_tables) return false;
    if (state->loaded_offset == 0) return false;  /* Already at beginning */

    const char *table = state->tables[state->current_table];

    /* Calculate how many rows to load before current offset */
    size_t load_count = PAGE_SIZE;
    size_t new_offset = 0;
    if (state->loaded_offset > load_count) {
        new_offset = state->loaded_offset - load_count;
    } else {
        load_count = state->loaded_offset;
        new_offset = 0;
    }

    char *err = NULL;
    ResultSet *more = db_query_page(state->conn, table, new_offset, load_count, NULL, false, &err);
    if (!more || more->num_rows == 0) {
        if (more) db_result_free(more);
        free(err);
        return false;
    }

    /* Prepend rows to existing data */
    size_t old_count = state->data->num_rows;
    size_t new_count = old_count + more->num_rows;
    Row *new_rows = malloc(new_count * sizeof(Row));
    if (!new_rows) {
        db_result_free(more);
        return false;
    }

    /* Copy new rows first (prepend) */
    for (size_t i = 0; i < more->num_rows; i++) {
        new_rows[i] = more->rows[i];
        /* Clear source so free doesn't deallocate the cells we moved */
        more->rows[i].cells = NULL;
        more->rows[i].num_cells = 0;
    }

    /* Then copy old rows */
    for (size_t i = 0; i < old_count; i++) {
        new_rows[more->num_rows + i] = state->data->rows[i];
    }

    /* Free old array (but not the cells which we moved) */
    free(state->data->rows);
    state->data->rows = new_rows;
    state->data->num_rows = new_count;

    /* Adjust cursor position (it's now offset by the prepended rows) */
    state->cursor_row += more->num_rows;
    state->scroll_row += more->num_rows;

    /* Update tracking */
    state->loaded_offset = new_offset;
    state->loaded_count = new_count;

    db_result_free(more);

    /* Trim old data to keep memory bounded */
    tui_trim_loaded_data(state);

    tui_set_status(state, "Loaded %zu/%zu rows", state->loaded_count, state->total_rows);
    return true;
}

/* Check if we need to load more rows based on cursor position */
static void tui_check_load_more(TuiState *state) {
    if (!state || !state->data) return;

    /* If cursor is within LOAD_THRESHOLD of the END, load more at end */
    size_t rows_from_end = state->data->num_rows > state->cursor_row
                           ? state->data->num_rows - state->cursor_row : 0;

    if (rows_from_end < LOAD_THRESHOLD) {
        /* Check if there are more rows to load at end */
        size_t loaded_end = state->loaded_offset + state->loaded_count;
        if (loaded_end < state->total_rows) {
            tui_load_more_rows(state);
        }
    }

    /* If cursor is within LOAD_THRESHOLD of the BEGINNING, load previous rows */
    if (state->cursor_row < LOAD_THRESHOLD && state->loaded_offset > 0) {
        tui_load_prev_rows(state);
    }
}

/* Show "Go to row" dialog */
static void tui_show_goto_dialog(TuiState *state) {
    if (!state || !state->data) return;

    int term_rows, term_cols;
    getmaxyx(stdscr, term_rows, term_cols);

    int height = 7;
    int width = 50;
    int starty = (term_rows - height) / 2;
    int startx = (term_cols - width) / 2;

    WINDOW *win = newwin(height, width, starty, startx);
    if (!win) return;

    keypad(win, TRUE);
    curs_set(1);

    char input[32] = {0};
    size_t input_len = 0;

    bool running = true;
    while (running) {
        werase(win);
        box(win, 0, 0);

        wattron(win, A_BOLD);
        mvwprintw(win, 0, (width - 14) / 2, " Go to Row ");
        wattroff(win, A_BOLD);

        mvwprintw(win, 2, 2, "Enter row number (1-%zu):", state->total_rows);

        /* Draw input field */
        wattron(win, A_REVERSE);
        mvwhline(win, 3, 2, ' ', width - 4);
        mvwprintw(win, 3, 3, "%s", input);
        wattroff(win, A_REVERSE);

        mvwprintw(win, 5, 2, "[Enter] Go  [Esc] Cancel");

        wmove(win, 3, 3 + (int)input_len);
        wrefresh(win);

        int ch = wgetch(win);

        switch (ch) {
            case 27:  /* Escape */
                running = false;
                break;

            case '\n':
            case KEY_ENTER:
                if (input_len > 0) {
                    size_t row_num = (size_t)strtoll(input, NULL, 10);
                    if (row_num > 0 && row_num <= state->total_rows) {
                        /* Load data at that offset and position cursor */
                        size_t target_row = row_num - 1;  /* 0-indexed */

                        /* Check if target is in currently loaded range */
                        if (target_row >= state->loaded_offset &&
                            target_row < state->loaded_offset + state->loaded_count) {
                            /* Already loaded, just move cursor */
                            state->cursor_row = target_row - state->loaded_offset;
                        } else {
                            /* Need to load new data */
                            size_t load_offset = target_row > PAGE_SIZE / 2
                                                 ? target_row - PAGE_SIZE / 2 : 0;
                            tui_load_rows_at(state, load_offset);
                            state->cursor_row = target_row - state->loaded_offset;
                        }

                        /* Adjust scroll */
                        if (state->cursor_row < state->scroll_row) {
                            state->scroll_row = state->cursor_row;
                        } else if (state->cursor_row >= state->scroll_row + (size_t)state->content_rows) {
                            state->scroll_row = state->cursor_row - state->content_rows + 1;
                        }
                    } else {
                        /* Invalid row number - flash or beep */
                        flash();
                        continue;
                    }
                }
                running = false;
                break;

            case KEY_BACKSPACE:
            case 127:
            case 8:
                if (input_len > 0) {
                    input[--input_len] = '\0';
                }
                break;

            default:
                if (ch >= '0' && ch <= '9' && input_len < sizeof(input) - 1) {
                    input[input_len++] = (char)ch;
                    input[input_len] = '\0';
                }
                break;
        }
    }

    curs_set(0);
    delwin(win);

    touchwin(stdscr);
    tui_refresh(state);
}

void tui_calculate_column_widths(TuiState *state) {
    if (!state || !state->data) return;

    free(state->col_widths);
    state->num_col_widths = state->data->num_columns;
    state->col_widths = calloc(state->num_col_widths, sizeof(int));

    if (!state->col_widths) return;

    /* Start with column name widths */
    for (size_t i = 0; i < state->data->num_columns; i++) {
        const char *name = state->data->columns[i].name;
        int len = name ? (int)strlen(name) : 0;
        state->col_widths[i] = len < MIN_COL_WIDTH ? MIN_COL_WIDTH : len;
    }

    /* Check data widths */
    for (size_t row = 0; row < state->data->num_rows && row < 100; row++) {
        for (size_t col = 0; col < state->data->num_columns; col++) {
            char *str = db_value_to_string(&state->data->rows[row].cells[col]);
            if (str) {
                int len = (int)strlen(str);
                if (len > state->col_widths[col]) {
                    state->col_widths[col] = len;
                }
                free(str);
            }
        }
    }

    /* Apply max width */
    for (size_t i = 0; i < state->num_col_widths; i++) {
        if (state->col_widths[i] > MAX_COL_WIDTH) {
            state->col_widths[i] = MAX_COL_WIDTH;
        }
    }
}

void tui_draw_header(TuiState *state) {
    if (!state || !state->header_win) return;

    werase(state->header_win);
    wbkgd(state->header_win, COLOR_PAIR(COLOR_HEADER));

    /* Draw title */
    mvwprintw(state->header_win, 0, 1, " lace ");

    if (state->conn && state->conn->database) {
        mvwprintw(state->header_win, 0, 8, "| %s ", state->conn->database);
    }

    /* Help hint */
    const char *help = "q:Quit t:Sidebar /:GoTo []:Tabs -:Close ?:Help";
    mvwprintw(state->header_win, 0, state->term_cols - (int)strlen(help) - 1, "%s", help);

    wrefresh(state->header_win);
}

void tui_draw_table(TuiState *state) {
    if (!state || !state->main_win) return;

    werase(state->main_win);

    /* Get actual window dimensions */
    int win_rows, win_cols;
    getmaxyx(state->main_win, win_rows, win_cols);

    if (!state->data || state->data->num_columns == 0) {
        mvwprintw(state->main_win, win_rows / 2,
                  (win_cols - 7) / 2, "No data");
        wrefresh(state->main_win);
        return;
    }

    int y = 0;
    int x = 0;

    /* Draw column headers */
    wattron(state->main_win, A_BOLD | COLOR_PAIR(COLOR_BORDER));
    mvwhline(state->main_win, y, 0, ACS_HLINE, win_cols);
    y++;

    wattron(state->main_win, A_BOLD);
    x = 1;
    for (size_t col = state->scroll_col; col < state->data->num_columns; col++) {
        int width = tui_get_column_width(state, col);
        if (x + width + 3 > win_cols) break;

        /* Only show column header selection when sidebar is not focused */
        if (col == state->cursor_col && !state->sidebar_focused) {
            wattron(state->main_win, A_REVERSE);
        }

        const char *name = state->data->columns[col].name;
        mvwprintw(state->main_win, y, x, "%-*.*s", width, width, name ? name : "");

        if (col == state->cursor_col && !state->sidebar_focused) {
            wattroff(state->main_win, A_REVERSE);
        }

        x += width + 1;
        mvwaddch(state->main_win, y, x - 1, ACS_VLINE);
    }
    wattroff(state->main_win, A_BOLD);
    y++;

    wattron(state->main_win, COLOR_PAIR(COLOR_BORDER));
    mvwhline(state->main_win, y, 0, ACS_HLINE, win_cols);
    wattroff(state->main_win, COLOR_PAIR(COLOR_BORDER));
    y++;

    /* Draw data rows */
    for (size_t row = state->scroll_row;
         row < state->data->num_rows && y < win_rows;
         row++) {

        x = 1;
        /* Only show row selection when sidebar is not focused */
        bool is_selected_row = (row == state->cursor_row) && !state->sidebar_focused;

        if (is_selected_row) {
            wattron(state->main_win, A_BOLD);
        }

        for (size_t col = state->scroll_col; col < state->data->num_columns; col++) {
            int width = tui_get_column_width(state, col);
            if (x + width + 3 > win_cols) break;

            /* Only show selection when sidebar is not focused */
            bool is_selected = is_selected_row && (col == state->cursor_col) && !state->sidebar_focused;
            bool is_editing = is_selected && state->editing;

            if (is_editing) {
                /* Draw edit field with distinctive background */
                wattron(state->main_win, COLOR_PAIR(COLOR_EDIT));
                mvwhline(state->main_win, y, x, ' ', width);

                /* Draw the edit buffer */
                const char *buf = state->edit_buffer ? state->edit_buffer : "";
                size_t buf_len = strlen(buf);

                /* Calculate scroll for long text */
                size_t scroll = 0;
                if (state->edit_pos >= (size_t)(width - 1)) {
                    scroll = state->edit_pos - width + 2;
                }

                /* Draw visible portion of text */
                size_t draw_len = buf_len > scroll ? buf_len - scroll : 0;
                if (draw_len > (size_t)width) draw_len = width;
                if (draw_len > 0) {
                    mvwaddnstr(state->main_win, y, x, buf + scroll, (int)draw_len);
                }

                wattroff(state->main_win, COLOR_PAIR(COLOR_EDIT));

                /* Draw cursor character with reverse video for visibility */
                int cursor_x = x + (int)(state->edit_pos - scroll);
                if (cursor_x >= x && cursor_x < x + width) {
                    char cursor_char = (state->edit_pos < buf_len) ? buf[state->edit_pos] : ' ';
                    wattron(state->main_win, A_REVERSE | A_BOLD);
                    mvwaddch(state->main_win, y, cursor_x, cursor_char);
                    wattroff(state->main_win, A_REVERSE | A_BOLD);
                    wmove(state->main_win, y, cursor_x);
                }
            } else if (is_selected) {
                wattron(state->main_win, COLOR_PAIR(COLOR_SELECTED));

                DbValue *val = &state->data->rows[row].cells[col];
                if (val->is_null) {
                    mvwprintw(state->main_win, y, x, "%-*s", width, "NULL");
                } else {
                    char *str = db_value_to_string(val);
                    if (str) {
                        char *safe = sanitize_for_display(str);
                        mvwprintw(state->main_win, y, x, "%-*.*s", width, width, safe ? safe : str);
                        free(safe);
                        free(str);
                    }
                }

                wattroff(state->main_win, COLOR_PAIR(COLOR_SELECTED));
            } else {
                DbValue *val = &state->data->rows[row].cells[col];
                if (val->is_null) {
                    wattron(state->main_win, COLOR_PAIR(COLOR_NULL));
                    mvwprintw(state->main_win, y, x, "%-*s", width, "NULL");
                    wattroff(state->main_win, COLOR_PAIR(COLOR_NULL));
                } else {
                    char *str = db_value_to_string(val);
                    if (str) {
                        char *safe = sanitize_for_display(str);
                        if (val->type == DB_TYPE_INT || val->type == DB_TYPE_FLOAT) {
                            wattron(state->main_win, COLOR_PAIR(COLOR_NUMBER));
                        }
                        mvwprintw(state->main_win, y, x, "%-*.*s", width, width, safe ? safe : str);
                        if (val->type == DB_TYPE_INT || val->type == DB_TYPE_FLOAT) {
                            wattroff(state->main_win, COLOR_PAIR(COLOR_NUMBER));
                        }
                        free(safe);
                        free(str);
                    }
                }
            }

            x += width + 1;
            mvwaddch(state->main_win, y, x - 1, ACS_VLINE);
        }

        if (is_selected_row) {
            wattroff(state->main_win, A_BOLD);
        }

        y++;
    }

    wrefresh(state->main_win);
}

void tui_draw_status(TuiState *state) {
    if (!state || !state->status_win) return;

    werase(state->status_win);

    if (state->status_is_error) {
        wbkgd(state->status_win, COLOR_PAIR(COLOR_ERROR));
    } else {
        wbkgd(state->status_win, COLOR_PAIR(COLOR_STATUS));
    }

    /* Left: show table name when sidebar focused, otherwise column info */
    if (state->sidebar_focused && state->tables && state->num_tables > 0) {
        /* Show highlighted table name */
        size_t actual_idx = get_filtered_table_index(state, state->sidebar_highlight);
        if (actual_idx < state->num_tables && state->tables[actual_idx]) {
            const char *name = state->tables[actual_idx];
            mvwprintw(state->status_win, 0, 1, "%s", name);
        }
    } else if (state->schema && state->cursor_col < state->schema->num_columns) {
        ColumnDef *col = &state->schema->columns[state->cursor_col];

        /* Build column info string */
        char info[256];
        int pos = 0;

        /* Column name */
        pos += snprintf(info + pos, sizeof(info) - pos, "%s", col->name ? col->name : "?");

        /* Type */
        if (col->type_name) {
            pos += snprintf(info + pos, sizeof(info) - pos, " : %s", col->type_name);
        }

        /* Flags */
        if (col->primary_key) {
            pos += snprintf(info + pos, sizeof(info) - pos, " [PK]");
        }
        if (!col->nullable) {
            pos += snprintf(info + pos, sizeof(info) - pos, " NOT NULL");
        }
        if (col->default_val) {
            pos += snprintf(info + pos, sizeof(info) - pos, " DEFAULT %s", col->default_val);
        }

        mvwprintw(state->status_win, 0, 1, "%s", info);
    }

    /* Center: status/error message */
    if (state->status_msg) {
        int msg_len = (int)strlen(state->status_msg);
        int center_x = (state->term_cols - msg_len) / 2;
        if (center_x < 1) center_x = 1;
        mvwprintw(state->status_win, 0, center_x, "%s", state->status_msg);
    }

    /* Right: row position */
    if (state->data) {
        char pos[64];
        size_t actual_row = state->loaded_offset + state->cursor_row + 1;
        size_t total = state->total_rows > 0 ? state->total_rows : state->data->num_rows;
        snprintf(pos, sizeof(pos), "Row %zu/%zu", actual_row, total);
        mvwprintw(state->status_win, 0, state->term_cols - (int)strlen(pos) - 1, "%s", pos);
    }

    wrefresh(state->status_win);
}

void tui_draw_sidebar(TuiState *state) {
    if (!state || !state->sidebar_win || !state->sidebar_visible) return;

    werase(state->sidebar_win);

    /* Draw border */
    wattron(state->sidebar_win, COLOR_PAIR(COLOR_BORDER));
    box(state->sidebar_win, 0, 0);
    wattroff(state->sidebar_win, COLOR_PAIR(COLOR_BORDER));

    /* Title */
    wattron(state->sidebar_win, A_BOLD);
    mvwprintw(state->sidebar_win, 0, 2, " Tables ");
    wattroff(state->sidebar_win, A_BOLD);

    int y = 1;
    int max_name_len = SIDEBAR_WIDTH - 4;

    /* Draw filter input */
    if (state->sidebar_filter_active) {
        wattron(state->sidebar_win, A_REVERSE);
    }
    mvwprintw(state->sidebar_win, y, 1, "/%-*.*s",
              max_name_len, max_name_len,
              state->sidebar_filter_len > 0 ? state->sidebar_filter : "");
    if (state->sidebar_filter_active) {
        wattroff(state->sidebar_win, A_REVERSE);
        /* Show cursor position */
        curs_set(1);
        wmove(state->sidebar_win, y, 2 + (int)state->sidebar_filter_len);
    } else {
        curs_set(0);
    }
    y++;

    /* Separator */
    mvwhline(state->sidebar_win, y, 1, ACS_HLINE, SIDEBAR_WIDTH - 2);
    y++;

    if (!state->tables || state->num_tables == 0) {
        mvwprintw(state->sidebar_win, y, 2, "(no tables)");
        wrefresh(state->sidebar_win);
        return;
    }

    int list_height = state->term_rows - 6;  /* Content area height minus filter */

    /* Count filtered tables */
    size_t filtered_count = 0;
    for (size_t i = 0; i < state->num_tables; i++) {
        if (state->tables[i] &&
            (state->sidebar_filter_len == 0 ||
             str_istr(state->tables[i], state->sidebar_filter))) {
            filtered_count++;
        }
    }

    if (filtered_count == 0) {
        mvwprintw(state->sidebar_win, y, 2, "(no matches)");
        wrefresh(state->sidebar_win);
        return;
    }

    /* Adjust scroll if highlight is out of view */
    if (state->sidebar_highlight < state->sidebar_scroll) {
        state->sidebar_scroll = state->sidebar_highlight;
    } else if (state->sidebar_highlight >= state->sidebar_scroll + (size_t)list_height) {
        state->sidebar_scroll = state->sidebar_highlight - list_height + 1;
    }

    /* Draw filtered tables */
    size_t filtered_idx = 0;
    for (size_t i = 0; i < state->num_tables && y < state->term_rows - 3; i++) {
        const char *name = state->tables[i];
        if (!name) continue;

        /* Apply filter */
        if (state->sidebar_filter_len > 0 && !str_istr(name, state->sidebar_filter)) {
            continue;
        }

        /* Skip items before scroll offset */
        if (filtered_idx < state->sidebar_scroll) {
            filtered_idx++;
            continue;
        }

        bool is_highlighted = (filtered_idx == state->sidebar_highlight);
        bool is_current = (i == state->current_table);

        if (is_highlighted && state->sidebar_focused && !state->sidebar_filter_active) {
            wattron(state->sidebar_win, A_REVERSE);
        }

        /* Truncate name if too long */
        char display_name[SIDEBAR_WIDTH];
        int name_len = (int)strlen(name);
        if (name_len > max_name_len) {
            if (is_highlighted && state->sidebar_focused && !state->sidebar_filter_active) {
                /* Apply scroll animation for highlighted item */
                size_t scroll = state->sidebar_name_scroll;
                if (scroll > (size_t)(name_len - max_name_len)) {
                    scroll = name_len - max_name_len;
                }
                snprintf(display_name, sizeof(display_name), "%.*s", max_name_len, name + scroll);
            } else {
                snprintf(display_name, sizeof(display_name), "%.*s..", max_name_len - 2, name);
            }
        } else {
            snprintf(display_name, sizeof(display_name), "%s", name);
        }

        if (is_current) {
            wattron(state->sidebar_win, A_BOLD);
        }

        mvwprintw(state->sidebar_win, y, 2, "%-*s", max_name_len, display_name);

        if (is_current) {
            wattroff(state->sidebar_win, A_BOLD);
        }
        if (is_highlighted && state->sidebar_focused && !state->sidebar_filter_active) {
            wattroff(state->sidebar_win, A_REVERSE);
        }

        y++;
        filtered_idx++;
    }

    wrefresh(state->sidebar_win);
}

void tui_refresh(TuiState *state) {
    tui_draw_header(state);
    tui_draw_tabs(state);
    tui_draw_sidebar(state);
    tui_draw_table(state);
    tui_draw_status(state);

    /* Ensure cursor is only visible when filter is active */
    if (state->sidebar_filter_active && state->sidebar_focused) {
        curs_set(1);
        if (state->sidebar_win) {
            wmove(state->sidebar_win, 1, 2 + (int)state->sidebar_filter_len);
            wrefresh(state->sidebar_win);
        }
    } else {
        curs_set(0);
    }
}

void tui_move_cursor(TuiState *state, int row_delta, int col_delta) {
    if (!state || !state->data) return;

    /* Update row */
    if (row_delta < 0 && state->cursor_row > 0) {
        state->cursor_row--;
    } else if (row_delta > 0 && state->cursor_row < state->data->num_rows - 1) {
        state->cursor_row++;
    }

    /* Update column */
    if (col_delta < 0 && state->cursor_col > 0) {
        state->cursor_col--;
    } else if (col_delta > 0 && state->cursor_col < state->data->num_columns - 1) {
        state->cursor_col++;
    }

    /* Adjust scroll */
    /* Visible rows = main window height - 3 header rows (border + headers + separator) */
    int visible_rows = state->term_rows - 6;
    if (visible_rows < 1) visible_rows = 1;

    if (state->cursor_row < state->scroll_row) {
        state->scroll_row = state->cursor_row;
    } else if (state->cursor_row >= state->scroll_row + (size_t)visible_rows) {
        state->scroll_row = state->cursor_row - visible_rows + 1;
    }

    /* Calculate visible columns */
    int x = 1;
    size_t first_visible_col = state->scroll_col;
    size_t last_visible_col = state->scroll_col;

    for (size_t col = state->scroll_col; col < state->data->num_columns; col++) {
        int width = tui_get_column_width(state, col);
        if (x + width + 3 > state->term_cols) break;
        x += width + 1;
        last_visible_col = col;
    }

    if (state->cursor_col < first_visible_col) {
        state->scroll_col = state->cursor_col;
    } else if (state->cursor_col > last_visible_col) {
        /* Scroll right */
        state->scroll_col = state->cursor_col;
        /* Adjust to show as many columns as possible */
        x = 1;
        while (state->scroll_col > 0) {
            int width = tui_get_column_width(state, state->scroll_col);
            if (x + width + 3 > state->term_cols) break;
            x += width + 1;
            if (state->scroll_col == state->cursor_col) break;
            state->scroll_col--;
        }
    }

    /* Check if we need to load more rows */
    tui_check_load_more(state);
}

void tui_page_up(TuiState *state) {
    if (!state || !state->data) return;

    int page_size = state->term_rows - 6;
    if (page_size < 1) page_size = 1;

    if (state->cursor_row > (size_t)page_size) {
        state->cursor_row -= page_size;
    } else {
        state->cursor_row = 0;
    }

    if (state->scroll_row > (size_t)page_size) {
        state->scroll_row -= page_size;
    } else {
        state->scroll_row = 0;
    }

    /* Ensure cursor remains visible after scroll adjustment */
    if (state->cursor_row < state->scroll_row) {
        state->scroll_row = state->cursor_row;
    } else if (state->cursor_row >= state->scroll_row + (size_t)page_size) {
        state->scroll_row = state->cursor_row - page_size + 1;
    }

    /* Check if we need to load previous rows */
    tui_check_load_more(state);
}

void tui_page_down(TuiState *state) {
    if (!state || !state->data) return;

    int page_size = state->term_rows - 6;
    if (page_size < 1) page_size = 1;

    state->cursor_row += page_size;
    if (state->cursor_row >= state->data->num_rows) {
        state->cursor_row = state->data->num_rows - 1;
    }

    state->scroll_row += page_size;
    size_t max_scroll = state->data->num_rows > (size_t)page_size ?
                        state->data->num_rows - page_size : 0;
    if (state->scroll_row > max_scroll) {
        state->scroll_row = max_scroll;
    }

    /* Ensure cursor remains visible after scroll adjustment */
    if (state->cursor_row < state->scroll_row) {
        state->scroll_row = state->cursor_row;
    } else if (state->cursor_row >= state->scroll_row + (size_t)page_size) {
        state->scroll_row = state->cursor_row - page_size + 1;
    }

    /* Check if we need to load more rows */
    tui_check_load_more(state);
}

void tui_home(TuiState *state) {
    if (!state) return;

    /* If we're not at the beginning, load the first page */
    if (state->loaded_offset > 0) {
        tui_load_rows_at(state, 0);
    }

    state->cursor_row = 0;
    state->cursor_col = 0;
    state->scroll_row = 0;
    state->scroll_col = 0;
}

void tui_end(TuiState *state) {
    if (!state || !state->data) return;

    /* If we haven't loaded all rows, load the last page */
    size_t loaded_end = state->loaded_offset + state->loaded_count;
    if (loaded_end < state->total_rows) {
        /* Load last page of data */
        size_t last_page_offset = state->total_rows > PAGE_SIZE
                                  ? state->total_rows - PAGE_SIZE : 0;
        tui_load_rows_at(state, last_page_offset);
    }

    state->cursor_row = state->data->num_rows > 0 ? state->data->num_rows - 1 : 0;
    state->cursor_col = state->data->num_columns > 0 ? state->data->num_columns - 1 : 0;

    int visible_rows = state->term_rows - 6;
    if (visible_rows < 1) visible_rows = 1;

    state->scroll_row = state->data->num_rows > (size_t)visible_rows ?
                        state->data->num_rows - visible_rows : 0;
}

void tui_next_table(TuiState *state) {
    if (!state || !state->tables || state->num_tables == 0) return;

    state->current_table++;
    if (state->current_table >= state->num_tables) {
        state->current_table = 0;
    }

    tui_load_table_data(state, state->tables[state->current_table]);
}

void tui_prev_table(TuiState *state) {
    if (!state || !state->tables || state->num_tables == 0) return;

    if (state->current_table == 0) {
        state->current_table = state->num_tables - 1;
    } else {
        state->current_table--;
    }

    tui_load_table_data(state, state->tables[state->current_table]);
}

void tui_show_schema(TuiState *state) {
    if (!state || !state->schema) {
        tui_set_error(state, "No schema available");
        return;
    }

    /* Create a popup window */
    int height = state->term_rows - 4;
    int width = state->term_cols - 10;
    int starty = 2;
    int startx = 5;

    WINDOW *schema_win = newwin(height, width, starty, startx);
    if (!schema_win) return;

    keypad(schema_win, TRUE);

    int scroll_offset = 0;
    int max_scroll = 0;
    bool running = true;

    while (running) {
        werase(schema_win);
        box(schema_win, 0, 0);
        wattron(schema_win, A_BOLD);
        mvwprintw(schema_win, 0, 2, " Schema: %s ", state->schema->name);
        wattroff(schema_win, A_BOLD);

        int y = 2;
        int content_height = height - 4;
        int line = 0;

        /* Calculate total lines needed */
        int total_lines = 2 + (int)state->schema->num_columns;  /* Columns header + data */
        if (state->schema->num_indexes > 0) {
            total_lines += 2 + (int)state->schema->num_indexes;  /* Indexes section */
        }
        if (state->schema->num_foreign_keys > 0) {
            total_lines += 2 + (int)state->schema->num_foreign_keys;  /* FKs section */
        }

        max_scroll = total_lines - content_height;
        if (max_scroll < 0) max_scroll = 0;

        /* Draw content with scrolling */
        #define DRAW_LINE(fmt, ...) do { \
            if (line >= scroll_offset && y < height - 2) { \
                mvwprintw(schema_win, y++, 2, fmt, ##__VA_ARGS__); \
            } \
            line++; \
        } while(0)

        /* Columns section */
        wattron(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
        DRAW_LINE("Columns (%zu):", state->schema->num_columns);
        wattroff(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));

        if (line >= scroll_offset && y < height - 2) {
            wattron(schema_win, A_BOLD);
            mvwprintw(schema_win, y++, 4, "%-20s %-15s %-8s %-8s %-8s",
                      "Name", "Type", "Nullable", "PK", "AI");
            wattroff(schema_win, A_BOLD);
        }
        line++;

        for (size_t i = 0; i < state->schema->num_columns; i++) {
            ColumnDef *col = &state->schema->columns[i];
            if (line >= scroll_offset && y < height - 2) {
                mvwprintw(schema_win, y++, 4, "%-20s %-15s %-8s %-8s %-8s",
                          col->name ? col->name : "",
                          col->type_name ? col->type_name : db_value_type_name(col->type),
                          col->nullable ? "YES" : "NO",
                          col->primary_key ? "YES" : "",
                          col->auto_increment ? "YES" : "");
            }
            line++;
        }

        /* Indexes section */
        if (state->schema->num_indexes > 0) {
            line++;  /* blank line */
            if (line >= scroll_offset && y < height - 2) y++;

            wattron(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
            DRAW_LINE("Indexes (%zu):", state->schema->num_indexes);
            wattroff(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));

            for (size_t i = 0; i < state->schema->num_indexes; i++) {
                IndexDef *idx = &state->schema->indexes[i];
                if (line >= scroll_offset && y < height - 2) {
                    /* Build column list string */
                    char cols[256] = "";
                    for (size_t j = 0; j < idx->num_columns && strlen(cols) < 200; j++) {
                        if (j > 0) strcat(cols, ", ");
                        strncat(cols, idx->columns[j], 200 - strlen(cols));
                    }
                    mvwprintw(schema_win, y++, 4, "%s%-20s %s(%s)",
                              idx->unique ? "[U] " : "    ",
                              idx->name ? idx->name : "",
                              idx->type ? idx->type : "",
                              cols);
                }
                line++;
            }
        }

        /* Foreign Keys section */
        if (state->schema->num_foreign_keys > 0) {
            line++;  /* blank line */
            if (line >= scroll_offset && y < height - 2) y++;

            wattron(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
            DRAW_LINE("Foreign Keys (%zu):", state->schema->num_foreign_keys);
            wattroff(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));

            for (size_t i = 0; i < state->schema->num_foreign_keys; i++) {
                ForeignKeyDef *fk = &state->schema->foreign_keys[i];
                if (line >= scroll_offset && y < height - 2) {
                    /* Build column lists */
                    char src_cols[128] = "";
                    for (size_t j = 0; j < fk->num_columns && strlen(src_cols) < 100; j++) {
                        if (j > 0) strcat(src_cols, ", ");
                        strncat(src_cols, fk->columns[j], 100 - strlen(src_cols));
                    }
                    char ref_cols[128] = "";
                    for (size_t j = 0; j < fk->num_ref_columns && strlen(ref_cols) < 100; j++) {
                        if (j > 0) strcat(ref_cols, ", ");
                        strncat(ref_cols, fk->ref_columns[j], 100 - strlen(ref_cols));
                    }
                    mvwprintw(schema_win, y++, 4, "(%s) -> %s(%s)",
                              src_cols,
                              fk->ref_table ? fk->ref_table : "?",
                              ref_cols);
                }
                line++;
            }
        }

        #undef DRAW_LINE

        /* Footer */
        if (max_scroll > 0) {
            mvwprintw(schema_win, height - 2, 2,
                      "[Up/Down] Scroll  [q/Esc] Close  (%d/%d)",
                      scroll_offset + 1, max_scroll + 1);
        } else {
            mvwprintw(schema_win, height - 2, 2, "[q/Esc] Close");
        }

        wrefresh(schema_win);

        int ch = wgetch(schema_win);
        switch (ch) {
            case 'q':
            case 'Q':
            case 27:  /* Escape */
                running = false;
                break;
            case KEY_UP:
            case 'k':
                if (scroll_offset > 0) scroll_offset--;
                break;
            case KEY_DOWN:
            case 'j':
                if (scroll_offset < max_scroll) scroll_offset++;
                break;
            case KEY_PPAGE:
                scroll_offset -= content_height / 2;
                if (scroll_offset < 0) scroll_offset = 0;
                break;
            case KEY_NPAGE:
                scroll_offset += content_height / 2;
                if (scroll_offset > max_scroll) scroll_offset = max_scroll;
                break;
        }
    }

    delwin(schema_win);
    touchwin(stdscr);
    tui_refresh(state);
}

void tui_set_status(TuiState *state, const char *fmt, ...) {
    if (!state) return;

    free(state->status_msg);

    va_list args;
    va_start(args, fmt);
    state->status_msg = str_vprintf(fmt, args);
    va_end(args);

    state->status_is_error = false;
}

void tui_set_error(TuiState *state, const char *fmt, ...) {
    if (!state) return;

    free(state->status_msg);

    va_list args;
    va_start(args, fmt);
    state->status_msg = str_vprintf(fmt, args);
    va_end(args);

    state->status_is_error = true;
}

int tui_get_column_width(TuiState *state, size_t col) {
    if (!state || !state->col_widths || col >= state->num_col_widths) {
        return DEFAULT_COL_WIDTH;
    }
    return state->col_widths[col];
}

/* Count filtered tables and get filtered index */
static size_t count_filtered_tables(TuiState *state) {
    if (state->sidebar_filter_len == 0) return state->num_tables;

    size_t count = 0;
    for (size_t i = 0; i < state->num_tables; i++) {
        if (state->tables[i] && str_istr(state->tables[i], state->sidebar_filter)) {
            count++;
        }
    }
    return count;
}

/* Get the actual table index from filtered index */
static size_t get_filtered_table_index(TuiState *state, size_t filtered_idx) {
    if (state->sidebar_filter_len == 0) return filtered_idx;

    size_t count = 0;
    for (size_t i = 0; i < state->num_tables; i++) {
        if (state->tables[i] && str_istr(state->tables[i], state->sidebar_filter)) {
            if (count == filtered_idx) return i;
            count++;
        }
    }
    return 0;
}

/* Get the filtered index from actual table index (returns 0 if not in filtered list) */
static size_t get_sidebar_highlight_for_table(TuiState *state, size_t table_idx) {
    if (state->sidebar_filter_len == 0) return table_idx;

    size_t count = 0;
    for (size_t i = 0; i < state->num_tables; i++) {
        if (state->tables[i] && str_istr(state->tables[i], state->sidebar_filter)) {
            if (i == table_idx) return count;
            count++;
        }
    }
    return 0;  /* Table not in filtered list, default to first */
}

/* Find primary key column index (returns -1 if not found) */
static int find_pk_column(TuiState *state) {
    if (!state || !state->schema) return -1;

    for (size_t i = 0; i < state->schema->num_columns; i++) {
        if (state->schema->columns[i].primary_key) {
            return (int)i;
        }
    }
    return -1;
}

/* Start editing the current cell - uses modal for truncated content */
/* Start editing with modal editor (always) */
static void tui_start_modal_edit(TuiState *state) {
    if (!state || !state->data || state->editing) return;
    if (state->cursor_row >= state->data->num_rows) return;
    if (state->cursor_col >= state->data->num_columns) return;

    /* Get current cell value */
    DbValue *val = &state->data->rows[state->cursor_row].cells[state->cursor_col];

    /* Convert value to string */
    char *content = NULL;
    if (val->is_null) {
        content = str_dup("");
    } else {
        content = db_value_to_string(val);
        if (!content) content = str_dup("");
    }

    /* Always use modal editor */
    const char *col_name = state->data->columns[state->cursor_col].name;
    char *title = str_printf("Edit: %s", col_name);

    EditorResult result = editor_view_show(state, title ? title : "Edit Cell", content, false);
    free(title);

    if (result.saved) {
        /* Update the cell with new content (or NULL) */
        free(state->edit_buffer);
        state->edit_buffer = result.set_null ? NULL : result.content;
        state->editing = true;  /* Required for tui_confirm_edit */
        tui_confirm_edit(state);
    } else {
        free(result.content);
    }

    free(content);
}

static void tui_start_edit(TuiState *state) {
    if (!state || !state->data || state->editing) return;
    if (state->cursor_row >= state->data->num_rows) return;
    if (state->cursor_col >= state->data->num_columns) return;

    /* Get current cell value */
    DbValue *val = &state->data->rows[state->cursor_row].cells[state->cursor_col];

    /* Convert value to string */
    char *content = NULL;
    if (val->is_null) {
        content = str_dup("");
    } else {
        content = db_value_to_string(val);
        if (!content) content = str_dup("");
    }

    /* Check if content is truncated (exceeds column width) */
    int col_width = tui_get_column_width(state, state->cursor_col);
    size_t content_len = content ? strlen(content) : 0;
    bool is_truncated = content_len > (size_t)col_width;

    /* Also check if content has newlines (always use modal for multi-line) */
    bool has_newlines = content && strchr(content, '\n') != NULL;

    if (is_truncated || has_newlines) {
        /* Use modal editor for truncated or multi-line content */
        const char *col_name = state->data->columns[state->cursor_col].name;
        char *title = str_printf("Edit: %s", col_name);

        EditorResult result = editor_view_show(state, title ? title : "Edit Cell", content, false);
        free(title);

        if (result.saved) {
            /* Update the cell with new content (or NULL) */
            free(state->edit_buffer);
            state->edit_buffer = result.set_null ? NULL : result.content;
            state->editing = true;  /* Required for tui_confirm_edit */
            tui_confirm_edit(state);
        } else {
            free(result.content);
        }

        free(content);
    } else {
        /* Use inline editing for short content */
        free(state->edit_buffer);
        state->edit_buffer = content;
        state->edit_pos = state->edit_buffer ? strlen(state->edit_buffer) : 0;
        state->editing = true;
        curs_set(1);  /* Show cursor */
    }
}

/* Cancel editing */
static void tui_cancel_edit(TuiState *state) {
    if (!state) return;

    free(state->edit_buffer);
    state->edit_buffer = NULL;
    state->edit_pos = 0;
    state->editing = false;
    curs_set(0);  /* Hide cursor */
}

/* Set cell value directly (without edit mode) - set_null true for NULL, false for empty string */
static void tui_set_cell_direct(TuiState *state, bool set_null) {
    if (!state || !state->data || !state->conn) return;
    if (state->cursor_row >= state->data->num_rows) return;
    if (state->cursor_col >= state->data->num_columns) return;

    /* Find primary key column */
    int pk_col = find_pk_column(state);
    if (pk_col < 0) {
        tui_set_error(state, "Cannot update: no primary key found");
        return;
    }

    /* Get the current table name */
    const char *table = state->tables[state->current_table];

    /* Get primary key column name and value */
    const char *pk_col_name = state->data->columns[pk_col].name;
    DbValue *pk_val = &state->data->rows[state->cursor_row].cells[pk_col];

    /* Get the column being edited */
    const char *col_name = state->data->columns[state->cursor_col].name;

    /* Create new value */
    DbValue new_val;
    if (set_null) {
        new_val = db_value_null();
    } else {
        new_val = db_value_text("");
    }

    /* Attempt to update */
    char *err = NULL;
    bool success = db_update_cell(state->conn, table, pk_col_name, pk_val,
                                   col_name, &new_val, &err);

    if (success) {
        /* Update the local data */
        DbValue *cell = &state->data->rows[state->cursor_row].cells[state->cursor_col];
        db_value_free(cell);
        *cell = new_val;
        tui_set_status(state, set_null ? "Cell set to NULL" : "Cell set to empty");
    } else {
        db_value_free(&new_val);
        tui_set_error(state, "Update failed: %s", err ? err : "unknown error");
        free(err);
    }
}

/* Delete the current row */
static void tui_delete_row(TuiState *state) {
    if (!state || !state->data || !state->conn) return;
    if (state->cursor_row >= state->data->num_rows) return;

    /* Find primary key column */
    int pk_col = find_pk_column(state);
    if (pk_col < 0) {
        tui_set_error(state, "Cannot delete: no primary key found");
        return;
    }

    /* Get the current table name */
    const char *table = state->tables[state->current_table];

    /* Get primary key column name and value */
    const char *pk_col_name = state->data->columns[pk_col].name;
    DbValue *pk_val = &state->data->rows[state->cursor_row].cells[pk_col];

    /* Highlight the row being deleted with danger background */
    int win_rows, win_cols;
    getmaxyx(state->main_win, win_rows, win_cols);
    (void)win_rows;  /* unused */

    /* Calculate screen Y position: 3 header rows + (cursor_row - scroll_row) */
    int row_y = 3 + (int)(state->cursor_row - state->scroll_row);

    /* Draw the entire row with danger background */
    wattron(state->main_win, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
    int x = 1;
    for (size_t col = state->scroll_col; col < state->data->num_columns; col++) {
        int col_width = tui_get_column_width(state, col);
        if (x + col_width + 3 > win_cols) break;

        DbValue *val = &state->data->rows[state->cursor_row].cells[col];
        if (val->is_null) {
            mvwprintw(state->main_win, row_y, x, "%-*s", col_width, "NULL");
        } else {
            char *str = db_value_to_string(val);
            if (str) {
                char *safe = sanitize_for_display(str);
                mvwprintw(state->main_win, row_y, x, "%-*.*s", col_width, col_width, safe ? safe : str);
                free(safe);
                free(str);
            }
        }
        x += col_width + 1;
        mvwaddch(state->main_win, row_y, x - 1, ACS_VLINE);
    }
    wattroff(state->main_win, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
    wrefresh(state->main_win);

    /* Show confirmation dialog */
    int height = 7;
    int width = 50;
    int starty = (state->term_rows - height) / 2;
    int startx = (state->term_cols - width) / 2;

    WINDOW *confirm_win = newwin(height, width, starty, startx);
    if (!confirm_win) return;

    box(confirm_win, 0, 0);
    wattron(confirm_win, A_BOLD | COLOR_PAIR(COLOR_ERROR));
    mvwprintw(confirm_win, 0, (width - 18) / 2, " Delete Row ");
    wattroff(confirm_win, A_BOLD | COLOR_PAIR(COLOR_ERROR));

    mvwprintw(confirm_win, 2, 2, "Are you sure you want to delete this row?");
    mvwprintw(confirm_win, 4, 2, "[Enter/y] Delete    [n/Esc] Cancel");

    wrefresh(confirm_win);

    int ch = wgetch(confirm_win);
    delwin(confirm_win);
    touchwin(stdscr);
    tui_refresh(state);

    if (ch != 'y' && ch != 'Y' && ch != '\n' && ch != KEY_ENTER) {
        tui_set_status(state, "Delete cancelled");
        return;
    }

    /* Calculate absolute row position before delete */
    size_t abs_row = state->loaded_offset + state->cursor_row;
    size_t saved_col = state->cursor_col;
    size_t saved_scroll_col = state->scroll_col;
    /* Save visual offset (how far cursor is from top of visible area) */
    size_t visual_offset = state->cursor_row >= state->scroll_row
                         ? state->cursor_row - state->scroll_row : 0;

    /* Perform the delete */
    char *err = NULL;
    bool success = db_delete_row(state->conn, table, pk_col_name, pk_val, &err);

    if (success) {
        tui_set_status(state, "Row deleted");

        /* Update total row count */
        if (state->total_rows > 0) {
            state->total_rows--;
        }

        /* Adjust target row if we deleted the last row */
        if (abs_row >= state->total_rows && state->total_rows > 0) {
            abs_row = state->total_rows - 1;
        }

        /* Calculate which page to load */
        size_t target_offset = (abs_row / PAGE_SIZE) * PAGE_SIZE;

        /* Reload data at the appropriate offset */
        tui_load_rows_at(state, target_offset);

        /* Set cursor position relative to loaded data */
        if (state->data && state->data->num_rows > 0) {
            state->cursor_row = abs_row - state->loaded_offset;
            if (state->cursor_row >= state->data->num_rows) {
                state->cursor_row = state->data->num_rows - 1;
            }
            state->cursor_col = saved_col;
            state->scroll_col = saved_scroll_col;

            /* Restore scroll position to maintain visual offset */
            if (state->cursor_row >= visual_offset) {
                state->scroll_row = state->cursor_row - visual_offset;
            } else {
                state->scroll_row = 0;
            }
        }
    } else {
        tui_set_error(state, "Delete failed: %s", err ? err : "unknown error");
        free(err);
    }
}

/* Confirm editing and update database */
static void tui_confirm_edit(TuiState *state) {
    if (!state || !state->editing || !state->data || !state->conn) {
        tui_cancel_edit(state);
        return;
    }

    /* Find primary key column */
    int pk_col = find_pk_column(state);
    if (pk_col < 0) {
        tui_set_error(state, "Cannot update: no primary key found");
        tui_cancel_edit(state);
        return;
    }

    /* Get the current table name */
    const char *table = state->tables[state->current_table];

    /* Get primary key column name and value */
    const char *pk_col_name = state->data->columns[pk_col].name;
    DbValue *pk_val = &state->data->rows[state->cursor_row].cells[pk_col];

    /* Get the column being edited */
    const char *col_name = state->data->columns[state->cursor_col].name;

    /* Create new value from edit buffer */
    DbValue new_val;
    if (state->edit_buffer == NULL || state->edit_buffer[0] == '\0') {
        /* Empty string = NULL */
        new_val = db_value_null();
    } else {
        new_val = db_value_text(state->edit_buffer);
    }

    /* Attempt to update */
    char *err = NULL;
    bool success = db_update_cell(state->conn, table, pk_col_name, pk_val,
                                   col_name, &new_val, &err);

    if (success) {
        /* Update the local data */
        DbValue *cell = &state->data->rows[state->cursor_row].cells[state->cursor_col];
        db_value_free(cell);
        *cell = new_val;
        tui_set_status(state, "Cell updated");
    } else {
        db_value_free(&new_val);
        tui_set_error(state, "Update failed: %s", err ? err : "unknown error");
        free(err);
    }

    tui_cancel_edit(state);
}

/* Handle edit mode input */
static bool handle_edit_input(TuiState *state, int ch) {
    if (!state->editing) return false;

    size_t len = state->edit_buffer ? strlen(state->edit_buffer) : 0;

    switch (ch) {
        case 27:  /* Escape - cancel */
            tui_cancel_edit(state);
            return true;

        case '\n':
        case KEY_ENTER:
            tui_confirm_edit(state);
            return true;

        case KEY_LEFT:
            if (state->edit_pos > 0) {
                state->edit_pos--;
            }
            return true;

        case KEY_RIGHT:
            if (state->edit_pos < len) {
                state->edit_pos++;
            }
            return true;

        case KEY_HOME:
        case 1:  /* Ctrl+A */
            state->edit_pos = 0;
            return true;

        case KEY_END:
        case 5:  /* Ctrl+E */
            state->edit_pos = len;
            return true;

        case KEY_BACKSPACE:
        case 127:
        case 8:
            if (state->edit_pos > 0 && state->edit_buffer) {
                memmove(state->edit_buffer + state->edit_pos - 1,
                        state->edit_buffer + state->edit_pos,
                        len - state->edit_pos + 1);
                state->edit_pos--;
            }
            return true;

        case KEY_DC:  /* Delete */
            if (state->edit_pos < len && state->edit_buffer) {
                memmove(state->edit_buffer + state->edit_pos,
                        state->edit_buffer + state->edit_pos + 1,
                        len - state->edit_pos);
            }
            return true;

        case 21:  /* Ctrl+U - clear line */
            if (state->edit_buffer) {
                state->edit_buffer[0] = '\0';
                state->edit_pos = 0;
            }
            return true;

        case 14:  /* Ctrl+N - set to NULL */
            free(state->edit_buffer);
            state->edit_buffer = NULL;
            state->edit_pos = 0;
            tui_confirm_edit(state);
            return true;

        case 4:  /* Ctrl+D - set to empty string */
            free(state->edit_buffer);
            state->edit_buffer = str_dup("");
            state->edit_pos = 0;
            tui_confirm_edit(state);
            return true;

        default:
            if (ch >= 32 && ch < 127) {
                /* Insert character */
                size_t new_len = len + 2;
                char *new_buf = realloc(state->edit_buffer, new_len);
                if (new_buf) {
                    state->edit_buffer = new_buf;
                    memmove(state->edit_buffer + state->edit_pos + 1,
                            state->edit_buffer + state->edit_pos,
                            len - state->edit_pos + 1);
                    state->edit_buffer[state->edit_pos] = (char)ch;
                    state->edit_pos++;
                }
            }
            return true;
    }

    return false;
}

/* Handle sidebar input when it's focused */
static bool handle_sidebar_input(TuiState *state, int ch) {
    if (!state->sidebar_focused) return false;

    /* Handle filter input mode */
    if (state->sidebar_filter_active) {
        switch (ch) {
            case 27:  /* Escape */
                state->sidebar_filter_active = false;
                break;

            case '\n':
            case KEY_ENTER:
            case KEY_DOWN:
                state->sidebar_filter_active = false;
                state->sidebar_highlight = 0;
                state->sidebar_scroll = 0;
                break;

            case KEY_BACKSPACE:
            case 127:
            case 8:
                if (state->sidebar_filter_len > 0) {
                    state->sidebar_filter[--state->sidebar_filter_len] = '\0';
                    state->sidebar_highlight = 0;
                    state->sidebar_scroll = 0;
                }
                break;

            default:
                if (ch >= 32 && ch < 127 && state->sidebar_filter_len < sizeof(state->sidebar_filter) - 1) {
                    state->sidebar_filter[state->sidebar_filter_len++] = (char)ch;
                    state->sidebar_filter[state->sidebar_filter_len] = '\0';
                    state->sidebar_highlight = 0;
                    state->sidebar_scroll = 0;
                }
                break;
        }
        return true;
    }

    size_t filtered_count = count_filtered_tables(state);

    switch (ch) {
        case KEY_UP:
        case 'k':
            if (state->sidebar_highlight > 0) {
                state->sidebar_highlight--;
            } else {
                /* At top of list, move to filter field */
                state->sidebar_filter_active = true;
            }
            break;

        case KEY_DOWN:
        case 'j':
            if (filtered_count > 0 && state->sidebar_highlight < filtered_count - 1) {
                state->sidebar_highlight++;
            }
            break;

        case KEY_RIGHT:
        case 'l':
            /* Move focus to table view */
            state->sidebar_focused = false;
            break;

        case '\n':
        case KEY_ENTER:
            /* Select the highlighted table - open in current tab or create first tab */
            if (filtered_count > 0 && state->sidebar_highlight < filtered_count) {
                size_t actual_idx = get_filtered_table_index(state, state->sidebar_highlight);

                if (state->num_workspaces == 0) {
                    /* No tabs yet - create first one */
                    workspace_create(state, actual_idx);
                } else if (actual_idx != state->current_table) {
                    /* Replace current tab's table */
                    Workspace *ws = &state->workspaces[state->current_workspace];

                    /* Free old data */
                    db_result_free(ws->data);
                    db_schema_free(ws->schema);
                    free(ws->col_widths);
                    free(ws->table_name);

                    /* Update workspace */
                    ws->table_index = actual_idx;
                    ws->table_name = str_dup(state->tables[actual_idx]);

                    /* Clear state and load new table */
                    state->data = NULL;
                    state->schema = NULL;
                    state->col_widths = NULL;
                    state->num_col_widths = 0;
                    state->current_table = actual_idx;

                    tui_load_table_data(state, state->tables[actual_idx]);

                    /* Save to workspace */
                    ws->data = state->data;
                    ws->schema = state->schema;
                    ws->col_widths = state->col_widths;
                    ws->num_col_widths = state->num_col_widths;
                    ws->total_rows = state->total_rows;
                    ws->loaded_offset = state->loaded_offset;
                    ws->loaded_count = state->loaded_count;
                    ws->cursor_row = state->cursor_row;
                    ws->cursor_col = state->cursor_col;
                    ws->scroll_row = state->scroll_row;
                    ws->scroll_col = state->scroll_col;
                }
                state->sidebar_focused = false;
            }
            break;

        case '+':
        case '=':
            /* Open highlighted table in new tab */
            if (filtered_count > 0 && state->sidebar_highlight < filtered_count) {
                size_t actual_idx = get_filtered_table_index(state, state->sidebar_highlight);
                workspace_create(state, actual_idx);
                state->sidebar_focused = false;
            }
            break;

        case 'f':
        case 'F':
        case '/':
            /* Activate filter */
            state->sidebar_filter_active = true;
            break;

        case 27:  /* Escape */
            /* Clear filter if set, otherwise unfocus sidebar */
            if (state->sidebar_filter_len > 0) {
                state->sidebar_filter[0] = '\0';
                state->sidebar_filter_len = 0;
                state->sidebar_highlight = 0;
                state->sidebar_scroll = 0;
            } else {
                state->sidebar_focused = false;
            }
            break;

        case 't':
        case 'T':
        case KEY_F(9):  /* F9 - toggle sidebar */
            /* Close sidebar immediately */
            state->sidebar_visible = false;
            state->sidebar_focused = false;
            state->sidebar_filter[0] = '\0';
            state->sidebar_filter_len = 0;
            tui_recreate_windows(state);
            tui_calculate_column_widths(state);
            break;

        case '?':
        case KEY_F(1):
            tui_show_help(state);
            break;

        case 'q':
        case 'Q':
            state->running = false;
            break;

        case '[':
        case ']':
        case '-':
        case '_':
        case KEY_F(6):
        case KEY_F(7):
            /* Pass through to main handler for tab switching */
            return false;

        default:
            break;
    }

    return true;
}

/* Handle mouse events */
static bool handle_mouse_event(TuiState *state) {
    MEVENT event;
    if (getmouse(&event) != OK) return false;

    int mouse_y = event.y;
    int mouse_x = event.x;
    bool is_double = (event.bstate & BUTTON1_DOUBLE_CLICKED) != 0;
    bool is_click = (event.bstate & BUTTON1_CLICKED) != 0;

    if (!is_click && !is_double) return false;

    /* Determine click location */
    int sidebar_width = state->sidebar_visible ? SIDEBAR_WIDTH : 0;

    /* Check if click is in sidebar */
    if (state->sidebar_visible && mouse_x < sidebar_width) {
        /* If currently editing, save the edit first */
        if (state->editing) {
            tui_confirm_edit(state);
        }

        /* Sidebar layout (inside sidebar_win which starts at screen y=1):
           row 0 = border+title, row 1 = filter, row 2 = separator, row 3+ = table list */
        int sidebar_row = mouse_y - 1;  /* Convert to sidebar_win coordinates */

        /* Click on filter field (row 1 in sidebar_win = screen row 2) */
        if (sidebar_row == 1) {
            state->sidebar_focused = true;
            state->sidebar_filter_active = true;
            return true;
        }

        /* Clicking elsewhere in sidebar deactivates filter */
        state->sidebar_filter_active = false;

        /* Click on table list */
        int list_start_y = 3;  /* First table entry row in sidebar window */
        int clicked_row = sidebar_row - list_start_y;

        if (clicked_row >= 0) {
            size_t filtered_count = 0;
            for (size_t i = 0; i < state->num_tables; i++) {
                if (state->sidebar_filter_len == 0 ||
                    (state->tables[i] && str_istr(state->tables[i], state->sidebar_filter))) {
                    filtered_count++;
                }
            }

            size_t target_idx = state->sidebar_scroll + (size_t)clicked_row;
            if (target_idx < filtered_count) {
                /* Find the actual table index */
                size_t count = 0;
                for (size_t i = 0; i < state->num_tables; i++) {
                    if (state->sidebar_filter_len == 0 ||
                        (state->tables[i] && str_istr(state->tables[i], state->sidebar_filter))) {
                        if (count == target_idx) {
                            /* Select this table */
                            state->sidebar_highlight = target_idx;
                            state->sidebar_focused = true;
                            if (state->current_table != i) {
                                state->current_table = i;
                                tui_load_table_data(state, state->tables[i]);
                            }
                            return true;
                        }
                        count++;
                    }
                }
            }
        }
        return true;
    }

    /* Check if click is in main table area */
    if (mouse_x >= sidebar_width) {
        /* Clicking in main area deactivates sidebar filter */
        state->sidebar_filter_active = false;

        /* If currently editing, save the edit first */
        if (state->editing) {
            tui_confirm_edit(state);
        }

        if (!state->data || state->data->num_rows == 0) {
            return true;  /* No data to select, but filter is deactivated */
        }

        /* Adjust x coordinate relative to main window */
        int rel_x = mouse_x - sidebar_width;
        int rel_y = mouse_y - 1;  /* -1 for header row */

        /* Data rows start at y=3 in main window (after header line, column names, separator) */
        int data_start_y = 3;
        int clicked_data_row = rel_y - data_start_y;

        if (clicked_data_row >= 0) {
            /* Calculate which row was clicked */
            size_t target_row = state->scroll_row + (size_t)clicked_data_row;

            if (target_row < state->data->num_rows) {
                /* Calculate which column was clicked */
                int x_pos = 1;  /* Data starts at x=1 */
                size_t target_col = state->scroll_col;

                for (size_t col = state->scroll_col; col < state->data->num_columns; col++) {
                    int width = tui_get_column_width(state, col);
                    if (rel_x >= x_pos && rel_x < x_pos + width) {
                        target_col = col;
                        break;
                    }
                    x_pos += width + 1;  /* +1 for separator */
                    if (x_pos > state->term_cols) break;
                    target_col = col + 1;
                }

                if (target_col < state->data->num_columns) {
                    /* Update cursor position */
                    state->cursor_row = target_row;
                    state->cursor_col = target_col;
                    state->sidebar_focused = false;

                    /* Check if we need to load more rows (pagination) */
                    tui_check_load_more(state);

                    /* Double-click: enter edit mode */
                    if (is_double) {
                        tui_start_edit(state);
                    }

                    return true;
                }
            }
        }
    }

    return false;
}

/* Update sidebar name scroll animation */
static void update_sidebar_scroll_animation(TuiState *state) {
    if (!state || !state->sidebar_focused || !state->tables) return;

    /* Reset scroll when highlight changes */
    if (state->sidebar_highlight != state->sidebar_last_highlight) {
        state->sidebar_name_scroll = 0;
        state->sidebar_name_scroll_dir = 1;
        state->sidebar_name_scroll_delay = 3;  /* Initial pause */
        state->sidebar_last_highlight = state->sidebar_highlight;
        return;
    }

    /* Get highlighted table name */
    size_t actual_idx = get_filtered_table_index(state, state->sidebar_highlight);
    if (actual_idx >= state->num_tables) return;

    const char *name = state->tables[actual_idx];
    if (!name) return;

    int max_name_len = SIDEBAR_WIDTH - 4;
    int name_len = (int)strlen(name);

    /* Only animate if name is truncated */
    if (name_len <= max_name_len) {
        state->sidebar_name_scroll = 0;
        return;
    }

    int max_scroll = name_len - max_name_len;

    /* Handle pause at ends */
    if (state->sidebar_name_scroll_delay > 0) {
        state->sidebar_name_scroll_delay--;
        return;
    }

    /* Update scroll position */
    if (state->sidebar_name_scroll_dir > 0) {
        /* Scrolling right (showing more of the end) */
        if ((int)state->sidebar_name_scroll < max_scroll) {
            state->sidebar_name_scroll++;
        } else {
            /* Reached end, pause and reverse */
            state->sidebar_name_scroll_dir = -1;
            state->sidebar_name_scroll_delay = 5;
        }
    } else {
        /* Scrolling left (back to start) */
        if (state->sidebar_name_scroll > 0) {
            state->sidebar_name_scroll--;
        } else {
            /* Reached start, pause and reverse */
            state->sidebar_name_scroll_dir = 1;
            state->sidebar_name_scroll_delay = 5;
        }
    }
}

void tui_run(TuiState *state) {
    if (!state) return;

    tui_refresh(state);

    /* Set timeout for animation (80ms) */
    wtimeout(state->main_win, 80);
    if (state->sidebar_win) {
        wtimeout(state->sidebar_win, 80);
    }

    while (state->running) {
        /* Get input from appropriate window */
        WINDOW *input_win = state->sidebar_focused && state->sidebar_win
                            ? state->sidebar_win
                            : state->main_win;
        int ch = wgetch(input_win);

        /* Handle timeout - update animations */
        if (ch == ERR) {
            update_sidebar_scroll_animation(state);
            tui_draw_sidebar(state);
            continue;
        }

        /* Clear status message on any keypress */
        if (state->status_msg) {
            free(state->status_msg);
            state->status_msg = NULL;
            state->status_is_error = false;
        }

        /* Handle mouse events first - they should work regardless of mode */
        if (ch == KEY_MOUSE) {
            if (handle_mouse_event(state)) {
                tui_refresh(state);
            }
            continue;
        }

        /* Handle edit mode input (no key translation - user is typing) */
        if (state->editing && handle_edit_input(state, ch)) {
            tui_refresh(state);
            continue;
        }

        /* Handle sidebar filter input (no translation - user is typing filter text) */
        if (state->sidebar_focused && state->sidebar_filter_active) {
            if (handle_sidebar_input(state, ch)) {
                tui_refresh(state);
                continue;
            }
        }

        /* Translate non-Latin keyboard layouts for navigation hotkeys */
        ch = translate_key(ch);

        /* Handle sidebar navigation if focused */
        if (state->sidebar_focused && handle_sidebar_input(state, ch)) {
            tui_refresh(state);
            continue;
        }

        switch (ch) {
            case 'q':
            case 'Q':
            case 17:        /* Ctrl+Q - universal quit */
            case KEY_F(10): /* F10 - universal quit */
                state->running = false;
                break;

            case '\n':
            case KEY_ENTER:
                /* Start editing current cell */
                if (!state->sidebar_focused && state->data && state->data->num_rows > 0) {
                    tui_start_edit(state);
                }
                break;

            case 'e':        /* 'e' - always open modal editor */
            case KEY_F(4):   /* F4 - universal modal edit */
                if (!state->sidebar_focused && state->data && state->data->num_rows > 0) {
                    tui_start_modal_edit(state);
                }
                break;

            case 14:  /* Ctrl+N - set selected cell to NULL */
            case 'n':
                if (!state->sidebar_focused && state->data && state->data->num_rows > 0) {
                    tui_set_cell_direct(state, true);
                }
                break;

            case 4:  /* Ctrl+D - set selected cell to empty string */
            case 'd':
                if (!state->sidebar_focused && state->data && state->data->num_rows > 0) {
                    tui_set_cell_direct(state, false);
                }
                break;

            case 'x':      /* Delete row */
            case KEY_DC:   /* Delete key */
                if (!state->sidebar_focused && state->data && state->data->num_rows > 0) {
                    tui_delete_row(state);
                }
                break;

            case KEY_UP:
            case 'k':
                tui_move_cursor(state, -1, 0);
                break;

            case KEY_DOWN:
            case 'j':
                tui_move_cursor(state, 1, 0);
                break;

            case KEY_LEFT:
            case 'h':
                /* If at leftmost column and sidebar is visible, focus sidebar */
                if (state->cursor_col == 0 && state->sidebar_visible) {
                    state->sidebar_focused = true;
                    state->sidebar_highlight = get_sidebar_highlight_for_table(state, state->current_table);
                } else {
                    tui_move_cursor(state, 0, -1);
                }
                break;

            case KEY_RIGHT:
            case 'l':
                tui_move_cursor(state, 0, 1);
                break;

            case KEY_PPAGE:
                tui_page_up(state);
                break;

            case KEY_NPAGE:
                tui_page_down(state);
                break;

            case KEY_HOME:
                /* Move to first column */
                state->cursor_col = 0;
                state->scroll_col = 0;
                break;

            case KEY_END:
                /* Move to last column */
                if (state->schema) {
                    state->cursor_col = state->schema->num_columns > 0
                                        ? state->schema->num_columns - 1 : 0;
                }
                break;

            case KEY_F(61):  /* Ctrl+Home */
            case 'g':
            case 'a':
                tui_home(state);
                break;

            case KEY_F(62):  /* Ctrl+End */
            case 'G':
            case 'z':
                tui_end(state);
                break;

            case ']':
            case KEY_F(6):  /* F6 - next tab */
                if (state->num_workspaces > 1) {
                    size_t next = (state->current_workspace + 1) % state->num_workspaces;
                    workspace_switch(state, next);
                }
                break;

            case '[':
            case KEY_F(7):  /* F7 - previous tab */
                if (state->num_workspaces > 1) {
                    size_t prev = state->current_workspace > 0
                                ? state->current_workspace - 1
                                : state->num_workspaces - 1;
                    workspace_switch(state, prev);
                }
                break;

            case '-':
            case '_':
                /* Close current tab */
                if (state->num_workspaces > 0) {
                    workspace_close(state);
                }
                break;

            case 's':
            case 'S':
            case KEY_F(3):  /* F3 - universal schema */
                tui_show_schema(state);
                break;

            case '/':
            case 7:         /* Ctrl+G - universal go to line */
            case KEY_F(5):  /* F5 - universal go to row */
                tui_show_goto_dialog(state);
                break;

            case 'c':
            case 'C':
            case KEY_F(2):  /* F2 - universal connect */
                tui_show_connect_dialog(state);
                break;

            case 't':
            case 'T':
            case KEY_F(9):  /* F9 - universal toggle sidebar */
                /* Toggle sidebar */
                if (state->sidebar_visible) {
                    /* Hide sidebar */
                    state->sidebar_visible = false;
                    state->sidebar_focused = false;
                } else {
                    /* Show and focus sidebar */
                    state->sidebar_visible = true;
                    state->sidebar_focused = true;
                    state->sidebar_highlight = get_sidebar_highlight_for_table(state, state->current_table);
                    state->sidebar_scroll = 0;
                }
                tui_recreate_windows(state);
                tui_calculate_column_widths(state);
                break;

            case '?':
            case KEY_F(1):
                tui_show_help(state);
                break;

            case KEY_RESIZE:
                /* Handle terminal resize */
                getmaxyx(stdscr, state->term_rows, state->term_cols);
                wresize(state->header_win, 1, state->term_cols);
                wresize(state->status_win, 1, state->term_cols);
                mvwin(state->status_win, state->term_rows - 1, 0);
                state->content_rows = state->term_rows - 4;
                tui_recreate_windows(state);
                tui_calculate_column_widths(state);
                break;

            default:
                break;
        }

        tui_refresh(state);
    }
}

void tui_show_connect_dialog(TuiState *state) {
    char *connstr = connect_view_show(state);
    if (connstr) {
        tui_disconnect(state);
        if (tui_connect(state, connstr)) {
            tui_set_status(state, "Connected successfully");
        }
        free(connstr);
    }
    tui_refresh(state);
}

void tui_show_table_selector(TuiState *state) {
    if (!state || !state->tables || state->num_tables == 0) {
        tui_set_error(state, "No tables available");
        return;
    }

    int height = (int)state->num_tables + 4;
    if (height > state->term_rows - 4) {
        height = state->term_rows - 4;
    }
    int width = 40;
    int starty = (state->term_rows - height) / 2;
    int startx = (state->term_cols - width) / 2;

    WINDOW *menu_win = newwin(height, width, starty, startx);
    if (!menu_win) return;

    keypad(menu_win, TRUE);
    box(menu_win, 0, 0);

    wattron(menu_win, A_BOLD);
    mvwprintw(menu_win, 0, 2, " Select Table ");
    wattroff(menu_win, A_BOLD);

    /* Create menu items */
    ITEM **items = calloc(state->num_tables + 1, sizeof(ITEM *));
    if (!items) {
        delwin(menu_win);
        return;
    }

    for (size_t i = 0; i < state->num_tables; i++) {
        items[i] = new_item(state->tables[i], "");
    }
    items[state->num_tables] = NULL;

    MENU *menu = new_menu(items);
    if (!menu) {
        for (size_t i = 0; i < state->num_tables; i++) {
            free_item(items[i]);
        }
        free(items);
        delwin(menu_win);
        return;
    }

    /* Set menu options */
    set_menu_win(menu, menu_win);
    set_menu_sub(menu, derwin(menu_win, height - 4, width - 4, 2, 2));
    set_menu_mark(menu, "> ");
    set_menu_format(menu, height - 4, 1);

    /* Set current item */
    if (state->current_table < state->num_tables) {
        set_current_item(menu, items[state->current_table]);
    }

    post_menu(menu);
    wrefresh(menu_win);

    mvwprintw(menu_win, height - 1, 2, "Enter:Select  Esc:Cancel");
    wrefresh(menu_win);

    bool running = true;
    while (running) {
        int ch = wgetch(menu_win);
        switch (ch) {
            case KEY_DOWN:
            case 'j':
                menu_driver(menu, REQ_DOWN_ITEM);
                break;

            case KEY_UP:
            case 'k':
                menu_driver(menu, REQ_UP_ITEM);
                break;

            case KEY_NPAGE:
                menu_driver(menu, REQ_SCR_DPAGE);
                break;

            case KEY_PPAGE:
                menu_driver(menu, REQ_SCR_UPAGE);
                break;

            case '\n':
            case KEY_ENTER: {
                ITEM *cur = current_item(menu);
                if (cur) {
                    int idx = item_index(cur);
                    if (idx >= 0 && (size_t)idx < state->num_tables) {
                        state->current_table = idx;
                        tui_load_table_data(state, state->tables[idx]);
                    }
                }
                running = false;
                break;
            }

            case 27:  /* Escape */
            case 'q':
                running = false;
                break;
        }
        wrefresh(menu_win);
    }

    /* Cleanup */
    unpost_menu(menu);
    free_menu(menu);
    for (size_t i = 0; i < state->num_tables; i++) {
        free_item(items[i]);
    }
    free(items);
    delwin(menu_win);

    touchwin(stdscr);
    tui_refresh(state);
}

void tui_show_help(TuiState *state) {
    int height = 44;
    int width = 60;
    int starty = (state->term_rows - height) / 2;
    int startx = (state->term_cols - width) / 2;

    WINDOW *help_win = newwin(height, width, starty, startx);
    if (!help_win) return;

    box(help_win, 0, 0);

    wattron(help_win, A_BOLD | COLOR_PAIR(COLOR_TITLE));
    mvwprintw(help_win, 0, (width - 6) / 2, " Help ");
    wattroff(help_win, A_BOLD | COLOR_PAIR(COLOR_TITLE));

    int y = 2;
    wattron(help_win, A_BOLD);
    mvwprintw(help_win, y++, 2, "Navigation:");
    wattroff(help_win, A_BOLD);
    mvwprintw(help_win, y++, 4, "Arrow keys, hjkl    Move cursor");
    mvwprintw(help_win, y++, 4, "Page Up/Down        Scroll pages");
    mvwprintw(help_win, y++, 4, "Home/End            First/last column");
    mvwprintw(help_win, y++, 4, "a, g, Ctrl+Home     First row");
    mvwprintw(help_win, y++, 4, "z, G, Ctrl+End      Last row");
    mvwprintw(help_win, y++, 4, "/, F5, Ctrl+G       Go to row number");
    y++;

    wattron(help_win, A_BOLD);
    mvwprintw(help_win, y++, 2, "Tabs:");
    wattroff(help_win, A_BOLD);
    mvwprintw(help_win, y++, 4, "]/F6, [/F7          Next/previous tab");
    mvwprintw(help_win, y++, 4, "+/= (in sidebar)    Open table in new tab");
    mvwprintw(help_win, y++, 4, "-/_                 Close current tab");
    y++;

    wattron(help_win, A_BOLD);
    mvwprintw(help_win, y++, 2, "Editing:");
    wattroff(help_win, A_BOLD);
    mvwprintw(help_win, y++, 4, "Enter               Edit cell (inline)");
    mvwprintw(help_win, y++, 4, "e, F4               Edit cell (modal)");
    mvwprintw(help_win, y++, 4, "n, Ctrl+N           Set cell to NULL");
    mvwprintw(help_win, y++, 4, "d, Ctrl+D           Set cell to empty");
    mvwprintw(help_win, y++, 4, "x, Delete           Delete row");
    mvwprintw(help_win, y++, 4, "Enter (in edit)     Save changes");
    mvwprintw(help_win, y++, 4, "Esc (in edit)       Cancel edit");
    y++;

    wattron(help_win, A_BOLD);
    mvwprintw(help_win, y++, 2, "Tables Sidebar:");
    wattroff(help_win, A_BOLD);
    mvwprintw(help_win, y++, 4, "t, F9               Toggle sidebar");
    mvwprintw(help_win, y++, 4, "f, /                Filter tables");
    mvwprintw(help_win, y++, 4, "Up/Down, j/k        Navigate list");
    mvwprintw(help_win, y++, 4, "Left/Right          Switch focus");
    mvwprintw(help_win, y++, 4, "Enter               Open in current tab");
    mvwprintw(help_win, y++, 4, "Esc                 Clear filter / unfocus");
    y++;

    wattron(help_win, A_BOLD);
    mvwprintw(help_win, y++, 2, "Mouse:");
    wattroff(help_win, A_BOLD);
    mvwprintw(help_win, y++, 4, "Click               Select cell/table");
    mvwprintw(help_win, y++, 4, "Double-click        Edit cell");
    y++;

    wattron(help_win, A_BOLD);
    mvwprintw(help_win, y++, 2, "Other:");
    wattroff(help_win, A_BOLD);
    mvwprintw(help_win, y++, 4, "s, F3               Show table schema");
    mvwprintw(help_win, y++, 4, "c, F2               Connection dialog");
    mvwprintw(help_win, y++, 4, "?, F1               This help");
    mvwprintw(help_win, y++, 4, "q, F10, Ctrl+Q      Quit");

    mvwprintw(help_win, height - 2, (width - 24) / 2, "Press any key to close");

    wrefresh(help_win);
    wgetch(help_win);

    delwin(help_win);
    touchwin(stdscr);
    tui_refresh(state);
}
