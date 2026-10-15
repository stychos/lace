/*
 * Lace
 * Workspaces window: state and helpers shared by its source files
 * (connect_view.c: the window, its list, details, footer and actions;
 * connect_tree.c: the saved connections tree and moving items;
 * connect_forms.c: the save and connection forms)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONNECT_INTERNAL_H
#define LACE_CONNECT_INTERNAL_H


#include "connect_view.h"
#include "../config/config.h"
#include "../config/connections.h"
#include "../config/recent.h"
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
  ROW_URL,       /* "Connect to" what is typed in the input */
  ROW_WORKSPACE, /* An open workspace (index into app->workspaces) */
  ROW_SAVED_WS,  /* A saved workspace (index into saved_ws) */
  ROW_TREE,      /* A saved connection or folder (index in the tree) */
  ROW_MOVING,    /* The item being moved, where it would go */
  ROW_RECENT     /* A recent connection (index into recent) */
} RowKind;

typedef struct {
  RowKind kind;
  size_t index;
  ConnectionItem *item; /* ROW_TREE, ROW_MOVING */
  int depth;            /* Tree indentation */
  const char *text;     /* ROW_HEADER, ROW_NOTE */
} ListRow;

/* What a key, a footer hint or a click asks for; what it does depends on
 * the selected row */
typedef enum {
  ACT_NONE,
  ACT_ENTER,      /* Open, switch, connect, open/close a folder */
  ACT_TAB_HERE,   /* Open as a tab in the current workspace */
  ACT_EDIT,       /* Edit a saved connection */
  ACT_TEST,       /* Test a connection */
  ACT_SAVE,       /* Save a workspace's layout, or a URL as a connection */
  ACT_RENAME,
  ACT_DELETE,     /* Delete, close (a workspace) or forget (recent) */
  ACT_DUPLICATE,
  ACT_MOVE,
  ACT_NEW,        /* New connection */
  ACT_NEW_FOLDER,
  ACT_SEARCH,     /* Focus the input */
  ACT_DETAILS,    /* Narrow terminals: details in place of the list */
  ACT_HELP,
  ACT_QUIT,
  ACT_COUNT
} ActionId;

/* Move operation state - tracks folder and position for hierarchical
 * navigation */
typedef struct {
  bool active;                   /* Is move in progress? */
  ConnectionItem *source;        /* Item being moved */
  ConnectionItem *target_folder; /* Folder where item will be inserted */
  size_t insert_pos; /* Position within folder (0=before first, N=after last) */
} MoveState;

#define MAX_HINTS 16

typedef struct {
  TuiState *state;      /* The app (may be NULL in tests of the forms) */
  ConnectionManager *mgr;
  char *load_error;     /* connections.json could not be read (read-only) */
  const Config *config; /* App configuration for hotkeys */
  lace_client_t *client; /* liblace client for connections */
  RecentList recent;
  SavedWorkspaces saved_ws;

  TextInput input;     /* Filter, or a URL/file to connect (wiped: it may
                          hold a password) */
  bool input_focused;  /* Keys type into the input */

  ListRow *rows;           /* The list as shown, rebuilt after every key */
  size_t num_rows, rows_cap;
  size_t sel;          /* Selected row (always a selectable one, if any) */
  size_t scroll;
  RowKind sel_kind;    /* What is selected, to find it again after a rebuild */
  size_t sel_index;
  size_t tree_highlight; /* Tree index of the selected tree row */

  char *error_msg;
  char *success_msg;

  WINDOW *win;           /* The whole terminal */
  int term_h, term_w;    /* The size the layout was made for */
  bool too_small;        /* Below MIN_DIALOG_*: only a note is shown */
  bool wide;             /* The details pane shows next to the list */
  bool show_details;     /* Narrow: details in place of the list */
  int list_y, list_x, list_h, list_w;
  int pane_y, pane_x, pane_h, pane_w;

  MoveState move;

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

/* A window for a form, in the details pane (or the list's place on a
 * narrow terminal): as wide as that, want_h tall at most */
WINDOW *connect_pane_window(DialogState *ds, int want_h, int *h, int *w);

/* The height connect_pane_window() gives for want_h */
int connect_pane_height(DialogState *ds, int want_h);

/* New (edit_item NULL) or edit a saved connection */
bool connect_connection_form(DialogState *ds, ConnectionItem *edit_item,
                             ConnectionItem *parent_folder, char **error);

/* Save a URL as a named connection */
bool connect_save_dialog(DialogState *ds, const char *url, char **error);

/* One line from a server message (newlines and tabs to spaces); caller frees */
char *connect_flatten_message(const char *msg);

/* Redraw the window (after a popup drew over it, or a resize) */
void connect_redraw(DialogState *ds);

/* Connect with conn's fields (asking for a password if the server wants
 * one), then disconnect; *msg says how it went. True if it worked. */
bool connect_test_saved(DialogState *ds, const SavedConnection *conn, char **msg);

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

/* The tree as listed now (under the input's filter, if any) */
const char *connect_filter(DialogState *ds);
size_t connect_tree_count(DialogState *ds);
ConnectionItem *connect_tree_item(DialogState *ds, size_t index);

/* Add a row to the list (the tree adds its own) */
void connect_add_row(DialogState *ds, RowKind kind, size_t index,
                     ConnectionItem *item, int depth, const char *text);

/* The tree's rows: as filtered, or during a move with the moved item where
 * it would go */
void connect_tree_rows(DialogState *ds);

/* The tree's actions, on the highlighted item (tree_highlight) or, with
 * none, the root */
void connect_tree_new_connection(DialogState *ds, ConnectionItem *at);
void connect_tree_new_folder(DialogState *ds, ConnectionItem *at);
void connect_tree_edit(DialogState *ds);
void connect_tree_rename(DialogState *ds);
void connect_tree_duplicate(DialogState *ds);
void connect_tree_delete(DialogState *ds);
/* Left/Right on a folder: close/open (Left on an item: to its folder) */
void connect_tree_left_right(DialogState *ds, bool right);
/* Start a move, or drop the item */
void connect_tree_move(DialogState *ds);
/* Keys during a move (Up/Down, drop, Esc); true if one was used */
bool connect_tree_move_key(DialogState *ds, const UiEvent *event);

#endif /* LACE_CONNECT_INTERNAL_H */
