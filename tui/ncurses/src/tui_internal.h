/*
 * Lace
 * TUI internal declarations shared between modules
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_INTERNAL_H
#define LACE_TUI_INTERNAL_H

#include "core/actions.h"
#include "core/constants.h"
#include "core/utf8.h"
#include "db_compat.h"
#include "../../liblace/include/util/str.h"
#include "render_helpers.h"
#include "tui.h"
#include <menu.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * ViewModel accessor helpers
 * ============================================================================
 * These eliminate duplicate get_vm_* functions across TUI modules.
 */

/* Get VmTable if valid, otherwise NULL */
static inline VmTable *tui_vm_table(TuiState *state) {
  if (!state || !state->vm_table)
    return NULL;
  return table_vm_valid(state->vm_table) ? state->vm_table : NULL;
}

/* Shared constants are now in core/constants.h */

/* ============================================================================
 * Drawing helper macros
 * ============================================================================
 * These reduce boilerplate for common ncurses attribute patterns.
 */

/* Draw with temporary attribute - automatically turns off after */
#define WITH_ATTR(win, attr, ...)                                              \
  do {                                                                         \
    wattron(win, attr);                                                        \
    __VA_ARGS__;                                                               \
    wattroff(win, attr);                                                       \
  } while (0)

/* Draw box with color - common border pattern */
#define DRAW_BOX(win, color)                                                   \
  do {                                                                         \
    wattron(win, COLOR_PAIR(color));                                           \
    box(win, 0, 0);                                                            \
    wattroff(win, COLOR_PAIR(color));                                          \
  } while (0)

/* Draw horizontal line with color */
#define DRAW_HLINE(win, y, x, width, color)                                    \
  do {                                                                         \
    wattron(win, COLOR_PAIR(color));                                           \
    mvwhline(win, y, x, ACS_HLINE, width);                                     \
    wattroff(win, COLOR_PAIR(color));                                          \
  } while (0)

/* Focus cycling helpers for dialog navigation */
#define FOCUS_NEXT(f, n) (((f) + 1) % (n))
#define FOCUS_PREV(f, n) (((f) + (n) - 1) % (n))

/* Center text within a width */
#define TEXT_CENTER_X(width, text_len) (((width) - (text_len)) / 2)

/* ============================================================================
 * Scroll position helpers
 * ============================================================================
 * These reduce boilerplate for common scroll/cursor clamping patterns.
 */

/* Subtract with underflow protection - returns 0 if subtraction would underflow.
 * Use for scroll/cursor adjustments: position = subtract_clamped(pos, amount) */
static inline size_t subtract_clamped(size_t value, size_t amount) {
  return (value > amount) ? value - amount : 0;
}

/* Calculate maximum valid scroll position given total items and visible count.
 * Returns 0 if all items fit in view. */
static inline size_t scroll_max(size_t total, size_t visible) {
  return (total > visible) ? total - visible : 0;
}

/* Adjust scroll position to keep cursor visible within view.
 * Updates *scroll in place if cursor is outside visible range.
 * visible must be > 0. */
static inline void scroll_clamp_to_cursor(size_t *scroll, size_t cursor,
                                          size_t visible) {
  if (cursor < *scroll) {
    *scroll = cursor;
  } else if (cursor >= *scroll + visible) {
    *scroll = cursor - visible + 1;
  }
}

/* Clamp scroll to maximum valid position.
 * Updates *scroll in place if it exceeds max_scroll. */
static inline void scroll_clamp_to_max(size_t *scroll, size_t max_scroll) {
  if (*scroll > max_scroll)
    *scroll = max_scroll;
}

/* ============================================================================
 * Layout calculation helpers
 * ============================================================================
 * These eliminate repeated getmaxyx + filters_height + visible_rows patterns.
 */

typedef struct {
  int win_rows;       /* Total window height */
  int win_cols;       /* Total window width */
  int filters_height; /* Height of filters panel (0 if hidden) */
  int header_rows;    /* Number of header rows (typically 3) */
  int visible_rows;   /* Rows available for data display */
  int data_start_y;   /* Y offset where data rows begin */
} LayoutInfo;

/* Calculate layout information for main table view.
 * Returns populated LayoutInfo, or zeros if state/main_win is invalid. */
static inline LayoutInfo tui_get_layout_info(TuiState *state) {
  LayoutInfo layout = {0, 0, 0, 3, 1, 3}; /* Default: 3 header rows */

  if (!state || !state->main_win)
    return layout;

  getmaxyx(state->main_win, layout.win_rows, layout.win_cols);

  /* Calculate filters panel height */
  layout.filters_height =
      tui_filters_visible(state) ? tui_get_filters_panel_height(state) : 0;

  /* Header rows: border + column names + separator = 3
   * Add 1 more row when sorting is active (sort indicator row) */
  Tab *tab = TUI_TAB(state);
  if (tab && tab->num_sort_entries > 0) {
    layout.header_rows = 4;
  }

  /* Visible rows = window height - header rows - filters panel */
  layout.visible_rows =
      layout.win_rows - layout.header_rows - layout.filters_height;
  if (layout.visible_rows < 1)
    layout.visible_rows = 1;

  /* Data rows start after filters and headers */
  layout.data_start_y = layout.filters_height + layout.header_rows;

  return layout;
}

/* ============================================================================
 * Dialog geometry helpers
 * ============================================================================
 */

/* Calculate centered dialog position with bounds clamping */
static inline void dialog_center_position(int *y, int *x, int height, int width,
                                          int term_h, int term_w) {
  *y = (term_h - height) / 2;
  *x = (term_w - width) / 2;
  if (*y < 0)
    *y = 0;
  if (*x < 0)
    *x = 0;
}

/* Clamp dialog width to fit within parent with margin */
static inline int dialog_clamp_width(int width, int parent_w, int margin) {
  int max_w = parent_w - margin;
  return (width > max_w) ? max_w : width;
}

/* Clamp dialog dimensions to min/max bounds and terminal size */
static inline void dialog_clamp_dimensions(int *h, int *w, int min_h, int min_w,
                                           int max_h, int max_w, int term_h,
                                           int term_w) {
  if (*h < min_h)
    *h = min_h;
  if (*w < min_w)
    *w = min_w;
  if (max_h > 0 && *h > max_h)
    *h = max_h;
  if (max_w > 0 && *w > max_w)
    *w = max_w;
  if (*h > term_h - 2)
    *h = term_h - 2;
  if (*w > term_w - 2)
    *w = term_w - 2;
}

/* Create a centered dialog window with keypad enabled.
 * Returns NULL if newwin fails. Caller must delwin() when done. */
static inline WINDOW *dialog_create(int height, int width, int term_h,
                                    int term_w) {
  int y, x;
  dialog_center_position(&y, &x, height, width, term_h, term_w);
  WINDOW *win = newwin(height, width, y, x);
  if (win)
    keypad(win, TRUE);
  return win;
}

/* Create a centered dialog with box border drawn */
static inline WINDOW *dialog_create_boxed(int height, int width, int term_h,
                                          int term_w, int border_color) {
  WINDOW *win = dialog_create(height, width, term_h, term_w);
  if (win) {
    wattron(win, COLOR_PAIR(border_color));
    box(win, 0, 0);
    wattroff(win, COLOR_PAIR(border_color));
  }
  return win;
}

/* ============================================================================
 * DialogContext - Encapsulates modal dialog state and lifecycle
 * ============================================================================
 */

typedef struct {
  WINDOW *win;       /* Dialog window */
  int height;        /* Dialog height */
  int width;         /* Dialog width */
  int term_h;        /* Terminal height at creation */
  int term_w;        /* Terminal width at creation */
  int selected;      /* Selected button/item index */
  bool running;      /* Event loop running flag */
  int border_color;  /* Border color pair */
  const char *title; /* Dialog title (not owned) */
} DialogContext;

/* Initialize dialog context and create window.
 * Returns true on success, false if window creation fails.
 * Caller must call dialog_ctx_destroy() when done. */
static inline bool dialog_ctx_init(DialogContext *ctx, int height, int width,
                                   int border_color, const char *title) {
  if (!ctx)
    return false;

  memset(ctx, 0, sizeof(*ctx));
  getmaxyx(stdscr, ctx->term_h, ctx->term_w);

  /* Clamp dimensions to terminal */
  if (height > ctx->term_h - 2)
    height = ctx->term_h - 2;
  if (width > ctx->term_w - 2)
    width = ctx->term_w - 2;
  if (height < 3)
    height = 3;
  if (width < 10)
    width = 10;

  ctx->height = height;
  ctx->width = width;
  ctx->border_color = border_color;
  ctx->title = title;
  ctx->running = true;

  ctx->win = dialog_create(height, width, ctx->term_h, ctx->term_w);
  return ctx->win != NULL;
}

/* Destroy dialog and cleanup */
static inline void dialog_ctx_destroy(DialogContext *ctx) {
  if (!ctx)
    return;
  if (ctx->win) {
    delwin(ctx->win);
    ctx->win = NULL;
  }
  touchwin(stdscr);
}

/* Draw dialog border and title */
static inline void dialog_ctx_draw_frame(DialogContext *ctx) {
  if (!ctx || !ctx->win)
    return;
  werase(ctx->win);
  wattron(ctx->win, COLOR_PAIR(ctx->border_color));
  box(ctx->win, 0, 0);
  wattroff(ctx->win, COLOR_PAIR(ctx->border_color));

  if (ctx->title) {
    int title_len = (int)strlen(ctx->title);
    int title_x = (ctx->width - title_len - 2) / 2;
    if (title_x < 1)
      title_x = 1;
    wattron(ctx->win, A_BOLD);
    mvwprintw(ctx->win, 0, title_x, " %s ", ctx->title);
    wattroff(ctx->win, A_BOLD);
  }
}

/* Draw button row at bottom of dialog.
 * buttons: array of button labels, num_buttons: count
 * selected: which button is highlighted (0-indexed) */
static inline void dialog_ctx_draw_buttons(DialogContext *ctx,
                                           const char **buttons,
                                           size_t num_buttons, int selected) {
  if (!ctx || !ctx->win || !buttons || num_buttons == 0)
    return;

  int btn_y = ctx->height - 2;
  int total_width = 0;
  for (size_t i = 0; i < num_buttons; i++) {
    total_width += (int)strlen(buttons[i]) + 4; /* "[ label ]" + space */
  }
  total_width -= 1; /* No trailing space */

  int start_x = (ctx->width - total_width) / 2;
  if (start_x < 2)
    start_x = 2;

  int x = start_x;
  for (size_t i = 0; i < num_buttons; i++) {
    if ((int)i == selected)
      wattron(ctx->win, A_REVERSE);
    mvwprintw(ctx->win, btn_y, x, "[ %s ]", buttons[i]);
    if ((int)i == selected)
      wattroff(ctx->win, A_REVERSE);
    x += (int)strlen(buttons[i]) + 5;
  }
}

/* Get next key from dialog window */
static inline int dialog_ctx_getch(DialogContext *ctx) {
  if (!ctx || !ctx->win)
    return ERR;
  return wgetch(ctx->win);
}

/* Cycle selected button (for Tab, Left, Right) */
static inline void dialog_ctx_cycle_button(DialogContext *ctx,
                                           int num_buttons) {
  if (!ctx || num_buttons <= 1)
    return;
  ctx->selected = (ctx->selected + 1) % num_buttons;
}

/* ============================================================================
 * Menu helpers
 * ============================================================================
 */

/* Setup a menu within a window with standard configuration.
 * Returns the menu subwindow (caller should NOT free it - owned by menu_win).
 * padding: inset from window border for the menu area */
static inline WINDOW *menu_setup(MENU *menu, WINDOW *menu_win, int height,
                                 int width, int padding) {
  set_menu_win(menu, menu_win);
  WINDOW *sub = derwin(menu_win, height - (padding * 2), width - (padding * 2),
                       padding, padding);
  set_menu_sub(menu, sub);
  set_menu_mark(menu, "> ");
  set_menu_format(menu, height - (padding * 2), 1);
  post_menu(menu);
  return sub;
}

/* Cleanup a menu and its items. Does NOT delete the window. */
static inline void menu_cleanup(MENU *menu, ITEM **items, size_t num_items) {
  unpost_menu(menu);
  free_menu(menu);
  for (size_t i = 0; i < num_items; i++) {
    if (items[i])
      free_item(items[i]);
  }
  free(items);
}

/* ============================================================================
 * Helper functions (tui.c)
 * ============================================================================
 */

/* Sanitize string for single-line cell display */
char *tui_sanitize_for_display(const char *str);

/* Print text clipped (and for _fit, padded) to width display columns */
void tui_print_clip(WINDOW *win, int y, int x, int width, const char *str);
void tui_print_fit(WINDOW *win, int y, int x, int width, const char *str);

/* Draw a single-line edit field: buf scrolled so the cursor (byte offset
 * pos) is visible, in attr, with the character under the cursor reversed.
 * Returns the cursor's screen column, or -1 if it is not visible. */
int tui_draw_edit_field(WINDOW *win, int y, int x, int width, const char *buf,
                        size_t pos, attr_t attr);

/* Case-insensitive substring search */
const char *tui_str_istr(const char *haystack, const char *needle);

/* Recreate windows after resize or sidebar toggle */
void tui_recreate_windows(TuiState *state);

/* ============================================================================
 * Workspace functions (workspace.c)
 * ============================================================================
 */

/* Initialize a workspace struct (zeroes and inits filters) */
void workspace_init(Workspace *ws);

/* Restore TUI state from current tab */
void tab_restore(TuiState *state);

/* Columns a tab takes in the tab bar (label, padding, separator) */
int tui_tab_width(const Tab *tab);
void tui_bind_connection(TuiState *state);
void sidebar_open_table(TuiState *state, size_t actual_idx);

/* Switch to a different tab */
void tab_switch(TuiState *state, size_t index);

/* Create new tab for a table */
bool tab_create(TuiState *state, size_t table_index);

/* Create new query tab */
bool tab_create_query(TuiState *state);

/* Close current tab */
void tab_close(TuiState *state);

void workspace_close(TuiState *state);

/* Make workspace index current (the screen follows) */
void tui_workspace_switch(TuiState *state, size_t index);

/* Close workspace index; the current one stays current. With none left,
 * nothing more happens (the caller is the Workspaces window). */
void tui_workspace_close_at(TuiState *state, size_t index);

/* ============================================================================
 * Sidebar functions (sidebar.c)
 * ============================================================================
 */

/* Count tables matching current filter */
size_t tui_count_filtered_tables(TuiState *state);

/* Get actual table index from filtered index */
size_t tui_get_filtered_table_index(TuiState *state, size_t filtered_idx);

/* Get sidebar highlight for a table index */
size_t tui_get_sidebar_highlight_for_table(TuiState *state, size_t table_idx);

/* Handle sidebar input when focused */
bool tui_handle_sidebar_input(TuiState *state, const UiEvent *event);

/* Update sidebar name scroll animation */
void tui_update_sidebar_scroll_animation(TuiState *state);

/* ============================================================================
 * Pagination functions (pagination.c)
 * ============================================================================
 */

/* Load more rows at end of current data */
bool tui_load_more_rows(TuiState *state);

/* Load rows at specific offset (replaces current data) */
bool tui_load_rows_at(TuiState *state, size_t offset);

/* Load previous rows (prepend to current data) */
bool tui_load_prev_rows(TuiState *state);

/* Trim loaded data to keep memory bounded */
void tui_trim_loaded_data(TuiState *state);

/* Load a page with blocking dialog (for fast scrolling past loaded data) */
bool tui_load_page_with_dialog(TuiState *state, bool forward);

/* Load rows at specific offset with blocking dialog (for goto/home/end) */
bool tui_load_rows_at_with_dialog(TuiState *state, size_t offset);

/* Start background load (non-blocking) - returns true if started */
bool tui_start_background_load(TuiState *state, bool forward);

/* Background poll result */
typedef enum {
  BG_POLL_NONE,     /* No background operation */
  BG_POLL_RUNNING,  /* Operation still running */
  BG_POLL_MERGED,   /* Operation completed and data was merged */
  BG_POLL_ERROR     /* Operation completed with error */
} BgPollResult;

/* Poll background load, merge if complete - call from main loop */
BgPollResult tui_poll_background_load(TuiState *state);

/* Cancel pending background load */
void tui_cancel_background_load(TuiState *state);

/* Check if speculative prefetch should start */
void tui_check_speculative_prefetch(TuiState *state);

/* ============================================================================
 * Edit functions (edit.c)
 * ============================================================================
 */

/* Start inline editing */
void tui_start_edit(TuiState *state);

/* Start modal editing */
void tui_start_modal_edit(TuiState *state);

/* Cancel editing */
void tui_cancel_edit(TuiState *state);

/* Confirm edit and update database */
void tui_confirm_edit(TuiState *state);

/* Set cell value directly (NULL or empty string) */
void tui_set_cell_direct(TuiState *state, bool set_null);

/* Copy current cell value to clipboard */
void tui_cell_copy(TuiState *state);

/* Paste clipboard content to current cell and update database */
void tui_cell_paste(TuiState *state);

/* Clipboard helpers - copy text to system clipboard and internal buffer */
bool tui_clipboard_copy(TuiState *state, const char *text);

/* Clipboard helpers - read text from clipboard (returns malloc'd string) */
char *tui_clipboard_read(TuiState *state);

/* Delete current row */
void tui_delete_row(TuiState *state);
/* Revert the current tab's last write (undo.c) */
void tui_undo(TuiState *state);

/* Where an edit writes: one row source, a table tab's window or a query
 * tab's results (EditTarget.query_result). Query results require every key
 * column exactly once, verify every loaded column of the table before any
 * write, and keep an edit on screen only when the row has no usable key or
 * the source table is unknown; a table tab refuses such a write. */
typedef struct {
  DbConnection *conn;
  const char *table;         /* Source table (NULL: unknown) */
  const TableSchema *schema; /* Its schema (NULL: not loaded) */
  ResultSet *data;           /* The rows on screen */
  Tab *tab;                  /* For marking the table's other tabs dirty */
  bool query_result;
} EditTarget;

/* The active table tab; false if it has no connection, table or schema */
bool tui_edit_target_table(TuiState *state, EditTarget *t);
void tui_edit_target_query(TuiState *state, Tab *tab, EditTarget *t);

/* Write text (NULL = an explicit NULL) into cell (row, col): unchanged text
 * writes nothing; otherwise key, row check, UPDATE, then the cell on
 * screen. Shows done_msg or why it was refused. True if the cell changed. */
bool tui_edit_write_cell(TuiState *state, const EditTarget *t, size_t row,
                         size_t col, const char *text, const char *done_msg);

/* Paste the clipboard into a cell (an empty clipboard writes nothing) */
void tui_edit_paste_cell(TuiState *state, const EditTarget *t, size_t row,
                         size_t col);

/* Delete row `row` of t->data in the database: key, check of every loaded
 * column, DELETE. *err is allocated on failure. */
bool tui_edit_delete_row(const EditTarget *t, size_t row, char **err);

/* Ask before deleting count rows, if the delete_confirmation setting says
 * so; false (status set) if the user declined */
bool tui_edit_confirm_delete(TuiState *state, size_t count);

/* The tab's selected rows, highest first (free it) */
size_t *tui_edit_selection_desc(const Tab *tab);

/* Handle edit mode input */
bool tui_handle_edit_input(TuiState *state, const UiEvent *event);

/* Line-editing keys (see TextInput) on an inline cell editor's text, which
 * is NULL while the cell is NULL: typing starts text, other keys leave a
 * NULL alone. Returns true if the key was an editing key. */
bool tui_edit_buffer_key(char **buf, size_t *pos, const UiEvent *event);

/* Paste text at the cursor (line breaks become spaces); a NULL text becomes
 * the pasted text. */
void tui_edit_buffer_insert(char **buf, size_t *pos, const char *text);

/* What a key in an inline cell editor asks of its owner */
typedef enum {
  CELL_EDIT_CONTINUE,  /* Handled (or ignored); keep editing */
  CELL_EDIT_CONFIRM,   /* Enter */
  CELL_EDIT_SET_NULL,  /* Explicit NULL (*buf is NULL); confirm it */
  CELL_EDIT_SET_EMPTY, /* Explicit '' (*buf is ""); confirm it */
  CELL_EDIT_CANCEL,    /* Escape */
} CellEditAction;

/* The keys of every inline cell editor (table cells, query results, new
 * rows): Esc, Enter, Ctrl+K copy, Ctrl+U paste, the editor NULL / empty
 * hotkeys, and line editing on *buf (NULL = the cell is NULL). */
/* Add-row editor: store an explicit NULL (set_null) or '' in the cell being
 * edited and finish its edit */
void tui_add_row_set_cell(TuiState *state, bool set_null);

CellEditAction tui_cell_edit_key(TuiState *state, char **buf, size_t *pos,
                                 const UiEvent *event);

/* ============================================================================
 * Navigation functions (navigation.c)
 * ============================================================================
 */

/* Note: tui_move_cursor, tui_page_up/down, tui_home/end,
 * tui_next/prev_table are declared in tui.h as public API */

/* ============================================================================
 * Draw functions (draw.c)
 * ============================================================================
 */

/* Note: tui_draw_header, tui_draw_table, tui_draw_status, tui_draw_sidebar
 * are declared in tui.h as public API */

/* Parameters for drawing a result grid */
typedef struct {
  WINDOW *win;             /* Window to draw in */
  int start_y;             /* Starting Y position in window */
  int start_x;             /* Starting X position in window */
  int height;              /* Available height for grid */
  int width;               /* Available width for grid */
  ResultSet *data;         /* Data to display */
  int *col_widths;         /* Column widths array */
  size_t num_col_widths;   /* Number of column widths */
  size_t cursor_row;       /* Current cursor row */
  size_t cursor_col;       /* Current cursor column */
  size_t scroll_row;       /* Vertical scroll offset */
  size_t scroll_col;       /* Horizontal scroll offset */
  size_t selection_offset; /* Offset for selection calculation (global row) */
  bool is_focused;         /* Whether this grid has focus */
  bool is_editing;         /* Whether editing is active */
  char *edit_buffer;       /* Edit buffer content */
  size_t edit_pos;         /* Cursor position in edit buffer */
  bool show_header_line;   /* Whether to draw top border line */
  SortEntry *sort_entries; /* Array of sort columns (NULL if none) */
  size_t num_sort_entries; /* Number of sort columns */
  const TableSchema *sort_schema; /* What sort_entries[].column indexes */
} GridDrawParams;

/* Draw a result set grid (used by table view and query results) */
void tui_draw_result_grid(TuiState *state, GridDrawParams *params);

/* Handle mouse events (using pre-translated UiEvent) */
bool tui_handle_mouse_event(TuiState *state, const UiEvent *event);

/* ============================================================================
 * Dialog functions (dialogs.c)
 * ============================================================================
 */

/* Show confirmation dialog - returns true if user confirms */
bool tui_show_confirm_dialog(TuiState *state, const char *message);

/* Show go-to row dialog */
void tui_show_goto_dialog(TuiState *state);

/* Note: tui_show_schema, tui_show_connect_dialog, tui_show_table_selector,
 * tui_show_config are declared in tui.h as public API */

/* New value for an edited cell. buffer NULL = set NULL. *unchanged is set
 * (nothing to write) for an untouched value or a NULL cell left empty. The
 * text is converted like the original value (BLOB hex -> bytes, DECIMAL
 * exact); an empty text is the empty string, never NULL. */
bool tui_edit_new_value(const DbValue *orig, LaceValueType col_type,
                        const char *buffer, DbValue *out, bool *unchanged,
                        char **err);

/* Detect arrival at another tab (by Tab.uid) and leave an arriving query
 * tab unfocused; tui_refresh() calls it first (focus.c) */
void tui_note_tab_switch(TuiState *state);

/* The UICallbacks through which core/actions.c reaches the TUI
 * (callbacks.c) */
UICallbacks tui_make_callbacks(TuiState *state);

#endif /* LACE_TUI_INTERNAL_H */
