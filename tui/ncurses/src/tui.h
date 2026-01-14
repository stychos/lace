/*
 * Lace ncurses frontend
 * TUI interface
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FRONTEND_TUI_H
#define LACE_FRONTEND_TUI_H

#include "app.h"
#include <ncurses.h>

/* ==========================================================================
 * Color Definitions
 * ========================================================================== */

#define COLOR_HEADER    1
#define COLOR_SELECTED  2
#define COLOR_STATUS    3
#define COLOR_ERROR     4
#define COLOR_BORDER    5
#define COLOR_TITLE     6
#define COLOR_NULL      7
#define COLOR_NUMBER    8
#define COLOR_SIDEBAR   9
#define COLOR_EDIT      10

/* ==========================================================================
 * Edit State
 * ========================================================================== */

typedef struct {
  bool active;           /* Currently in edit mode */
  size_t row;            /* Row being edited */
  size_t col;            /* Column being edited */
  char *buffer;          /* Edit buffer */
  size_t buffer_len;     /* Buffer length */
  size_t buffer_cap;     /* Buffer capacity */
  size_t cursor_pos;     /* Cursor position in buffer */
  bool is_null;          /* Editing a NULL value */
} EditState;

/* ==========================================================================
 * Filter Panel State
 * ========================================================================== */

typedef struct {
  bool visible;          /* Panel is visible */
  bool focused;          /* Panel has focus */
  size_t cursor_row;     /* Currently selected filter row */
  size_t cursor_field;   /* 0=column, 1=operator, 2=value */
  size_t scroll;         /* First visible filter row */

  /* Editing state */
  bool editing;          /* Currently editing value */
  char edit_buffer[256]; /* Edit buffer for value */
  size_t edit_pos;       /* Cursor position in edit buffer */
} FilterPanelState;

/* ==========================================================================
 * TUI State
 * ========================================================================== */

typedef struct TuiState {
  AppState *app;         /* Application state (not owned) */

  /* Windows */
  WINDOW *main_win;      /* Main content window */
  WINDOW *sidebar_win;   /* Sidebar window */
  WINDOW *status_win;    /* Status bar window */
  WINDOW *tab_win;       /* Tab bar window */

  /* Dimensions */
  int term_rows;
  int term_cols;
  int sidebar_width;
  int content_width;
  int content_height;

  /* Input state */
  bool in_sidebar;       /* Focus in sidebar vs main content */

  /* Edit state */
  EditState edit;        /* Cell editing state */

  /* Filter panel state */
  FilterPanelState filters; /* Filter panel state */

} TuiState;

/* ==========================================================================
 * TUI Lifecycle
 * ========================================================================== */

/*
 * Initialize TUI.
 *
 * @param app  Application state
 * @return     TUI state, or NULL on failure
 */
TuiState *tui_init(AppState *app);

/*
 * Cleanup TUI.
 *
 * @param tui  TUI state (NULL is safe)
 */
void tui_cleanup(TuiState *tui);

/*
 * Run TUI main loop.
 *
 * @param tui  TUI state
 */
void tui_run(TuiState *tui);

/* ==========================================================================
 * Drawing Functions
 * ========================================================================== */

/*
 * Redraw entire screen.
 *
 * @param tui  TUI state
 */
void tui_draw(TuiState *tui);

/*
 * Draw sidebar.
 *
 * @param tui  TUI state
 */
void tui_draw_sidebar(TuiState *tui);

/*
 * Draw main content area.
 *
 * @param tui  TUI state
 */
void tui_draw_content(TuiState *tui);

/*
 * Draw tab bar.
 *
 * @param tui  TUI state
 */
void tui_draw_tabs(TuiState *tui);

/*
 * Draw status bar.
 *
 * @param tui  TUI state
 */
void tui_draw_status(TuiState *tui);

/* ==========================================================================
 * Input Handling
 * ========================================================================== */

/*
 * Handle a single input event.
 *
 * @param tui  TUI state
 * @param ch   Character/key code
 * @return     true if should continue, false to quit
 */
bool tui_handle_input(TuiState *tui, int ch);

#endif /* LACE_FRONTEND_TUI_H */
