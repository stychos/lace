/*
 * Lace
 * Workspaces window: its state and helpers (connect_view.c), some of which
 * other views use (buttons, key hints, flattening server messages)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONNECT_INTERNAL_H
#define LACE_CONNECT_INTERNAL_H


#include "connect_view.h"
#include "../config/config.h"
#include "../config/connections.h"
#include "../config/saved_workspaces.h"
#include "util/connstr.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include "../render_helpers.h"
#include "../tui_internal.h"
#include "prompt.h"
#include "text_input.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* The smallest terminal the window lays itself out in (below: a note) */
#define MIN_DIALOG_WIDTH 40
#define MIN_DIALOG_HEIGHT 12
/* From this width on, the details pane shows next to the list */
#define WIDE_DIALOG_WIDTH 90

/* One line of the list */
typedef enum {
  ROW_HEADER,    /* A section's title (not selectable) */
  ROW_NOTE,      /* A dim note, e.g. "nothing matches" (not selectable) */
  ROW_WORKSPACE, /* An open workspace (index into app->workspaces) */
  ROW_SAVED_WS   /* A saved workspace (index into saved_ws) */
} RowKind;

typedef struct {
  RowKind kind;
  size_t index;
  ConnectionItem *item; /* unused */
  int depth;
  const char *text;     /* ROW_HEADER, ROW_NOTE */
} ListRow;

/* What a key, a footer hint or a click asks for; what it does depends on
 * the selected row */
typedef enum {
  ACT_NONE,
  ACT_ENTER,      /* Switch to, or open, a workspace */
  ACT_SAVE,       /* Save a workspace's layout */
  ACT_RENAME,
  ACT_DELETE,     /* Close a workspace, delete a saved one */
  ACT_NEW,        /* New workspace, with a connection form */
  ACT_SEARCH,     /* Focus the input */
  ACT_DETAILS,    /* Narrow terminals: details in place of the list */
  ACT_HELP,
  ACT_QUIT,
  ACT_COUNT
} ActionId;

#define MAX_HINTS 16

typedef struct {
  TuiState *state;      /* The app */
  ConnectionManager *mgr; /* Read only: names of saved connections */
  const Config *config; /* App configuration for hotkeys */
  SavedWorkspaces saved_ws;

  TextInput input;     /* Search */
  bool input_focused;  /* Keys type into the input */

  ListRow *rows;           /* The list as shown, rebuilt after every key */
  size_t num_rows, rows_cap;
  size_t sel;          /* Selected row (always a selectable one, if any) */
  size_t scroll;
  RowKind sel_kind;    /* What is selected, to find it again after a rebuild */
  size_t sel_index;

  char *error_msg;
  char *success_msg;

  WINDOW *win;           /* The whole terminal */
  int term_h, term_w;    /* The size the layout was made for */
  bool too_small;        /* Below MIN_DIALOG_*: only a note is shown */
  bool wide;             /* The details pane shows next to the list */
  bool show_details;     /* Narrow: details in place of the list */
  int list_y, list_x, list_h, list_w;
  int pane_y, pane_x, pane_h, pane_w;

  /* The footer hints as drawn, for clicks */
  int num_hints;
  ActionId hint_act[MAX_HINTS];
  int hint_x[MAX_HINTS], hint_w[MAX_HINTS];
} DialogState;

/* One-line prompt (a name); NULL if cancelled or empty */
char *connect_prompt_text(WINDOW *parent, const char *title, const char *label,
                          const char *initial_value);

/* Yes/No question; true for Yes */
bool connect_confirm(WINDOW *parent, const char *title, const char *message);

/* A window in the details pane's place, want_h tall at most */
WINDOW *connect_pane_window(DialogState *ds, int want_h, int *h, int *w);
int connect_pane_height(DialogState *ds, int want_h);

/* One line from a server message (newlines and tabs to spaces); caller frees */
char *connect_flatten_message(const char *msg);

/* Redraw the window (after a popup drew over it, or a resize) */
void connect_redraw(DialogState *ds);

/* A row of buttons centered in a window w wide: where each one goes
 * (x[i], width[i]); roomy picks "[ Label ]" over "[Label]" when it fits.
 * Drawing and mouse hits both use it. */
void connect_button_layout(const char *const *labels, int count, int w,
                           int gap, bool roomy, int *x, int *width);

/* Index of the button at column cx, or -1 */
int connect_button_at(const int *x, const int *width, int count, int cx);

/* The first key bound to a hotkey, for hints ("" if none) */
const char *connect_key_hint(const Config *config, HotkeyAction action,
                             const char *fallback);

/* Add a row to the list */
void connect_add_row(DialogState *ds, RowKind kind, size_t index,
                     ConnectionItem *item, int depth, const char *text);

#endif /* LACE_CONNECT_INTERNAL_H */
