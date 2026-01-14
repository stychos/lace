/*
 * Lace
 * TUI internal declarations shared between modules
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_INTERNAL_H
#define LACE_TUI_INTERNAL_H

#include "../../core/constants.h"
#include "../../db/db.h"
#include "../../util/str.h"
#include "render_helpers.h"
#include "tui.h"
#include <menu.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>
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
  return vm_table_valid(state->vm_table) ? state->vm_table : NULL;
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
      state->filters_visible ? tui_get_filters_panel_height(state) : 0;

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

/* Save current TUI state to current tab */
void tab_save(TuiState *state);

/* Restore TUI state from current tab */
void tab_restore(TuiState *state);

/* Sync focus and panel state from TuiState to current Tab */
void tab_sync_focus(TuiState *state);

/* Switch to a different tab */
void tab_switch(TuiState *state, size_t index);

/* Create new tab for a table */
bool tab_create(TuiState *state, size_t table_index);

/* Create new query tab */
bool tab_create_query(TuiState *state);

/* Close current tab */
void tab_close(TuiState *state);

/* Legacy wrappers for compatibility */
void workspace_save(TuiState *state);
void workspace_restore(TuiState *state);
void workspace_switch(TuiState *state, size_t index);
bool workspace_create(TuiState *state, size_t table_index);
void workspace_close(TuiState *state);

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

/* Check if more rows need to be loaded based on cursor position */
void tui_check_load_more(TuiState *state);

/* Load a page with blocking dialog (for fast scrolling past loaded data) */
bool tui_load_page_with_dialog(TuiState *state, bool forward);

/* Load rows at specific offset with blocking dialog (for goto/home/end) */
bool tui_load_rows_at_with_dialog(TuiState *state, size_t offset);

/* Start background load (non-blocking) - returns true if started */
bool tui_start_background_load(TuiState *state, bool forward);

/* Poll background load, merge if complete - call from main loop */
bool tui_poll_background_load(TuiState *state);

/* Cancel pending background load */
void tui_cancel_background_load(TuiState *state);

/* Check if speculative prefetch should start */
void tui_check_speculative_prefetch(TuiState *state);

/* ============================================================================
 * Edit functions (edit.c)
 * ============================================================================
 */

/* Find primary key column indices */
size_t tui_find_pk_columns(TuiState *state, size_t *pk_indices, size_t max_pks);

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

/* Handle edit mode input */
bool tui_handle_edit_input(TuiState *state, const UiEvent *event);

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

/* ============================================================================
 * Widget lifecycle functions (tab_widgets.c)
 * ============================================================================
 */

/* Initialize widgets for a table tab */
bool tui_init_table_tab_widgets(TuiState *state, UITabState *ui, Tab *tab);

/* Initialize widgets for a query tab */
bool tui_init_query_tab_widgets(TuiState *state, UITabState *ui, Tab *tab);

/* Cleanup widgets when tab is closed */
void tui_cleanup_tab_widgets(UITabState *ui);

/* Get query widget for current tab (NULL if not a query tab) */
QueryWidget *tui_query_widget_for_tab(TuiState *state);

/* Get sidebar widget for current tab (NULL if not available) */
SidebarWidget *tui_sidebar_widget(TuiState *state);

/* Get or create sidebar widget for current tab */
SidebarWidget *tui_ensure_sidebar_widget(TuiState *state);

#endif /* LACE_TUI_INTERNAL_H */
