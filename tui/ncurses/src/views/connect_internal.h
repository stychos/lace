/*
 * Lace
 * Connection Manager dialog: state and helpers shared by its source files
 * (connect_view.c: dialog loop, URL panel, buttons; connect_tree.c: saved
 * connections tree; connect_forms.c: save and connection forms)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONNECT_INTERNAL_H
#define LACE_CONNECT_INTERNAL_H


#include "connect_view.h"
#include "../config/config.h"
#include "../config/connections.h"
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

/* Local dialog-specific constants */
#define TREE_PANEL_WIDTH 30
#define MIN_DIALOG_WIDTH 70
#define MIN_DIALOG_HEIGHT 18

/* Dialog focus states */
typedef enum {
  FOCUS_TREE,   /* Saved connections tree */
  FOCUS_URL,    /* URL input field */
  FOCUS_BUTTONS /* Bottom buttons */
} DialogFocus;

/* Button IDs */
typedef enum {
  BTN_CONNECT,
  BTN_NEW_WS,
  BTN_TEST,
  BTN_SAVE,
  BTN_DELETE,
  BTN_CLOSE,
  BTN_QUIT,
  BTN_COUNT
} ButtonId;

/* Move operation state - tracks folder and position for hierarchical navigation
 */
typedef struct {
  bool active;                   /* Is move in progress? */
  ConnectionItem *source;        /* Item being moved */
  size_t source_idx;             /* Original visible index of source item */
  ConnectionItem *target_folder; /* Folder where item will be inserted */
  size_t insert_pos; /* Position within folder (0=before first, N=after last) */
} MoveState;

/* Dialog state */
typedef struct {
  ConnectionManager *mgr;
  const Config *config; /* App configuration for hotkeys */
  lace_client_t *client; /* liblace client for connections */
  TextInput url_input;  /* Wiped when freed: it may hold a password */
  int url_input_width;
  DialogFocus focus;
  DialogFocus prev_panel_focus; /* Previous panel focus (TREE or URL) for
                                   returning from buttons */
  size_t tree_highlight;
  size_t tree_scroll;
  int selected_button;
  char *error_msg;
  char *success_msg;
  bool has_existing_tabs;
  int height;
  int width;
  int tree_height;
  WINDOW *dialog_win;
  MoveState move; /* Move operation state */
} DialogState;

/* One-line prompt (a name); NULL if cancelled or empty */
char *connect_prompt_text(WINDOW *parent, const char *title, const char *label,
                          const char *initial_value);

/* Yes/No question; true for Yes */
bool connect_confirm(WINDOW *parent, const char *title, const char *message);

/* Draw the saved connections tree panel */
void connect_draw_tree(WINDOW *win, DialogState *ds, int start_y, int start_x,
                       int height, int width);

/* Keys while the tree has focus; true if handled */
bool connect_tree_handle_key(DialogState *ds, const UiEvent *event);

/* New (edit_item NULL) or edit a saved connection */
bool connect_connection_form(WINDOW *parent, ConnectionManager *mgr,
                             ConnectionItem *edit_item,
                             ConnectionItem *parent_folder, char **error);

/* Save the quick-connect URL as a named connection */
bool connect_save_dialog(WINDOW *parent, ConnectionManager *mgr,
                         const char *url, char **error);

#endif /* LACE_CONNECT_INTERNAL_H */
