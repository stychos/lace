/*
 * Lace
 * Connection Manager dialog - Combined saved connections tree + quick connect
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connect_view.h"
#include "../../../config/config.h"
#include "../../../config/connections.h"
#include "../../../db/connstr.h"
#include "../../../util/str.h"
#include "../render_helpers.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONNSTR_LEN 512
#define TREE_PANEL_WIDTH 30
#define MIN_DIALOG_WIDTH 70
#define MIN_DIALOG_HEIGHT 18

/* Input field structure */
typedef struct {
  char buffer[MAX_CONNSTR_LEN];
  size_t len;
  size_t cursor;
  size_t scroll;
  int width;
} InputField;

/* Dialog focus states */
typedef enum {
  FOCUS_TREE,    /* Saved connections tree */
  FOCUS_URL,     /* URL input field */
  FOCUS_BUTTONS  /* Bottom buttons */
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

/* Move operation state - tracks folder and position for hierarchical navigation */
typedef struct {
  bool active;                    /* Is move in progress? */
  ConnectionItem *source;         /* Item being moved */
  size_t source_idx;              /* Original visible index of source item */
  ConnectionItem *target_folder;  /* Folder where item will be inserted */
  size_t insert_pos;              /* Position within folder (0=before first, N=after last) */
} MoveState;

/* Dialog state */
typedef struct {
  ConnectionManager *mgr;
  const Config *config;       /* App configuration for hotkeys */
  InputField url_input;
  DialogFocus focus;
  DialogFocus prev_panel_focus; /* Previous panel focus (TREE or URL) for returning from buttons */
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
  MoveState move;             /* Move operation state */
} DialogState;

/* ============================================================================
 * Input Field
 * ============================================================================
 */

static void input_init(InputField *input, int width) {
  memset(input, 0, sizeof(InputField));
  input->width = width < 3 ? 3 : width;
}

static void input_draw(WINDOW *win, InputField *input, int y, int x,
                       bool focused, int *cursor_y, int *cursor_x) {
  size_t visible_start = input->scroll;
  size_t visible_len = (size_t)input->width;

  /* Clear the input area */
  if (focused) {
    wattron(win, COLOR_PAIR(COLOR_SELECTED));
  }
  mvwhline(win, y, x, ' ', input->width);

  /* Draw text */
  size_t draw_len = (visible_start <= input->len) ? input->len - visible_start : 0;
  if (draw_len > visible_len)
    draw_len = visible_len;

  if (draw_len > 0) {
    mvwaddnstr(win, y, x, input->buffer + visible_start, (int)draw_len);
  }

  if (focused) {
    wattroff(win, COLOR_PAIR(COLOR_SELECTED));
  }

  /* Draw underline */
  wattron(win, A_DIM);
  mvwhline(win, y + 1, x, ACS_HLINE, input->width);
  wattroff(win, A_DIM);

  *cursor_y = y;
  *cursor_x = x + (int)(input->cursor - input->scroll);
}

static void input_handle_key(InputField *input, const UiEvent *event) {
  if (!event || event->type != UI_EVENT_KEY)
    return;

  int key_char = render_event_get_char(event);

  if (render_event_is_special(event, UI_KEY_LEFT)) {
    if (input->cursor > 0) {
      input->cursor--;
      if (input->cursor < input->scroll)
        input->scroll = input->cursor;
    }
    return;
  }

  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    if (input->cursor < input->len) {
      input->cursor++;
      if (input->cursor >= input->scroll + (size_t)input->width - 2) {
        input->scroll = input->cursor - (size_t)input->width + 3;
      }
    }
    return;
  }

  if (render_event_is_special(event, UI_KEY_HOME) || render_event_is_ctrl(event, 'A')) {
    input->cursor = 0;
    input->scroll = 0;
    return;
  }

  if (render_event_is_special(event, UI_KEY_END) || render_event_is_ctrl(event, 'E')) {
    input->cursor = input->len;
    if (input->cursor >= input->scroll + (size_t)input->width - 2) {
      input->scroll = input->cursor > (size_t)input->width - 3
                    ? input->cursor - (size_t)input->width + 3 : 0;
    }
    return;
  }

  if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
    if (input->cursor > 0 && input->cursor <= input->len) {
      memmove(input->buffer + input->cursor - 1, input->buffer + input->cursor,
              input->len - input->cursor + 1);
      input->cursor--;
      input->len--;
      if (input->cursor < input->scroll)
        input->scroll = input->cursor;
    }
    return;
  }

  if (render_event_is_special(event, UI_KEY_DELETE) || render_event_is_ctrl(event, 'D')) {
    if (input->cursor < input->len) {
      memmove(input->buffer + input->cursor, input->buffer + input->cursor + 1,
              input->len - input->cursor);
      input->len--;
    }
    return;
  }

  if (render_event_is_ctrl(event, 'U')) {
    input->buffer[0] = '\0';
    input->len = 0;
    input->cursor = 0;
    input->scroll = 0;
    return;
  }

  if (render_event_is_ctrl(event, 'K')) {
    input->buffer[input->cursor] = '\0';
    input->len = input->cursor;
    return;
  }

  /* Printable character */
  if (render_event_is_char(event) && key_char >= 32 && key_char < 127 &&
      input->len < MAX_CONNSTR_LEN - 1 && input->cursor <= input->len) {
    memmove(input->buffer + input->cursor + 1, input->buffer + input->cursor,
            input->len - input->cursor + 1);
    input->buffer[input->cursor] = (char)key_char;
    input->cursor++;
    input->len++;
    if (input->cursor >= input->scroll + (size_t)input->width - 2) {
      input->scroll = input->cursor > (size_t)input->width - 3
                    ? input->cursor - (size_t)input->width + 3 : 0;
    }
  }
}

/* ============================================================================
 * Tree Drawing
 * ============================================================================
 */

static void draw_tree_item(WINDOW *win, int y, int x, int width,
                           ConnectionItem *item, bool selected, bool focused,
                           bool is_move_source) {
  int depth = connmgr_get_item_depth(item);
  int indent = depth * 2;

  /* Clear line */
  mvwhline(win, y, x, ' ', width);

  /* Highlight if selected */
  if (is_move_source) {
    /* Item being moved - show dimmed */
    wattron(win, A_DIM);
  } else if (selected) {
    if (focused) {
      wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    } else {
      wattron(win, A_REVERSE);
    }
  }

  mvwhline(win, y, x, ' ', width);

  /* Draw indent */
  wmove(win, y, x + indent);

  /* Draw name and folder indicator */
  const char *name = connmgr_item_name(item);
  int name_width = width - indent - 2;

  if (connmgr_is_folder(item)) {
    /* Folder: name followed by expand/collapse arrow */
    const char *arrow = item->folder.expanded
                        ? " \xE2\x96\xBC"   /* ▼ UTF-8 (expanded) */
                        : " \xE2\x96\xB6";  /* ▶ UTF-8 (collapsed) */
    int arrow_width = 2;  /* space + arrow */
    int max_name = name_width - arrow_width;
    if (max_name > 0) {
      if ((int)strlen(name) > max_name) {
        waddnstr(win, name, max_name - 1);
        waddch(win, '~');
      } else {
        waddstr(win, name);
      }
      waddstr(win, arrow);
    }
  } else {
    /* Connection: show cut marker if move source, then name */
    if (is_move_source) {
      waddstr(win, "~ ");
      name_width -= 2;
    }
    if (name_width > 0) {
      if ((int)strlen(name) > name_width) {
        waddnstr(win, name, name_width - 1);
        waddch(win, '~');
      } else {
        waddstr(win, name);
      }
    }
  }

  if (is_move_source) {
    wattroff(win, A_DIM);
  } else if (selected) {
    if (focused) {
      wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    } else {
      wattroff(win, A_REVERSE);
    }
  }
}

/*
 * Move system helpers - hierarchical folder+position navigation
 *
 * Navigation moves one folder level at a time for natural tree traversal.
 */

/* Count children in folder excluding the source item */
static size_t count_children_excluding_source(ConnectionItem *folder, ConnectionItem *source) {
  if (!folder || folder->type != CONN_ITEM_FOLDER)
    return 0;
  size_t count = 0;
  for (size_t i = 0; i < folder->folder.num_children; i++) {
    if (&folder->folder.children[i] != source)
      count++;
  }
  return count;
}

/* Get child at index (excluding source) */
static ConnectionItem *get_child_excluding_source(ConnectionItem *folder, size_t idx, ConnectionItem *source) {
  if (!folder || folder->type != CONN_ITEM_FOLDER)
    return NULL;
  size_t count = 0;
  for (size_t i = 0; i < folder->folder.num_children; i++) {
    ConnectionItem *child = &folder->folder.children[i];
    if (child == source)
      continue;
    if (count == idx)
      return child;
    count++;
  }
  return NULL;
}

/* Find index of item in parent folder (excluding source) */
static size_t find_index_in_parent(ConnectionItem *item, ConnectionItem *source) {
  if (!item || !item->parent || item->parent->type != CONN_ITEM_FOLDER)
    return 0;
  ConnectionFolder *folder = &item->parent->folder;
  size_t idx = 0;
  for (size_t i = 0; i < folder->num_children; i++) {
    ConnectionItem *child = &folder->children[i];
    if (child == source)
      continue;
    if (child == item)
      return idx;
    idx++;
  }
  return idx;
}

/* Get the insert_after item for current position (NULL = insert at beginning) */
static ConnectionItem *get_insert_after_for_pos(DialogState *ds) {
  if (!ds->move.active || !ds->move.target_folder)
    return NULL;
  if (ds->move.insert_pos == 0)
    return NULL;
  return get_child_excluding_source(ds->move.target_folder, ds->move.insert_pos - 1, ds->move.source);
}

/* Get the depth for current target position */
static int get_move_target_depth(DialogState *ds) {
  if (!ds->move.active || !ds->move.target_folder)
    return 0;
  /* Items in root are at depth 0 */
  if (ds->move.target_folder == &ds->mgr->root)
    return 0;
  /* Items in any subfolder are at folder's depth + 1 */
  int folder_depth = connmgr_get_item_depth(ds->move.target_folder);
  return folder_depth + 1;
}

/* Helper: clear move state */
static void clear_move_state(MoveState *move) {
  move->active = false;
  move->source = NULL;
  move->source_idx = 0;
  move->target_folder = NULL;
  move->insert_pos = 0;
}

/* Helper: draw the moving item at its target position */
static void draw_moving_item_at_depth(WINDOW *win, DialogState *ds, int y, int x, int width, int depth) {
  if (!ds->move.source)
    return;
  const char *name = connmgr_item_name(ds->move.source);
  if (!name)
    name = "(unnamed)";
  wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
  mvwhline(win, y, x, ' ', width);
  if (connmgr_is_folder(ds->move.source)) {
    const char *arrow = ds->move.source->folder.expanded ? " \xE2\x96\xBC" : " \xE2\x96\xB6";
    mvwprintw(win, y, x + depth * 2, "%s%s", name, arrow);
  } else {
    mvwprintw(win, y, x + depth * 2, "%s", name);
  }
  wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
}

/* Helper: check if item is the source being moved */
static bool is_source_item(DialogState *ds, ConnectionItem *item) {
  return ds->move.active && item == ds->move.source;
}

/* Check if an item is a descendant of an ancestor */
static bool is_descendant_of(ConnectionItem *item, ConnectionItem *ancestor) {
  if (!item || !ancestor)
    return false;
  ConnectionItem *p = item->parent;
  while (p) {
    if (p == ancestor) return true;
    p = p->parent;
  }
  return false;
}

/* Compute the visual index where source should appear (excluding source from count) */
static ssize_t compute_source_visual_position(DialogState *ds) {
  if (!ds->move.active || !ds->move.target_folder)
    return -1;

  ConnectionItem *target = ds->move.target_folder;
  size_t insert_pos = ds->move.insert_pos;
  size_t visible_count = connmgr_count_visible(ds->mgr);

  /* Case 1: Insert at beginning of folder */
  if (insert_pos == 0) {
    if (target == &ds->mgr->root) {
      return 0;  /* Very first position */
    }
    /* Find the target folder in the visible list */
    size_t idx = 0;
    for (size_t i = 0; i < visible_count; i++) {
      ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
      if (item == ds->move.source) continue;
      if (item == target) {
        return (ssize_t)(idx + 1);  /* Right after the folder header */
      }
      idx++;
    }
    return -1;
  }

  /* Case 2: Insert after a specific child */
  ConnectionItem *after_child = get_child_excluding_source(target, insert_pos - 1, ds->move.source);
  if (!after_child) {
    /* Fallback: insert at end of folder, find folder's last visible descendant */
    size_t idx = 0;
    ssize_t folder_idx = -1;
    ssize_t last_descendant_idx = -1;
    for (size_t i = 0; i < visible_count; i++) {
      ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
      if (item == ds->move.source) continue;
      if (item == target) {
        folder_idx = (ssize_t)idx;
        last_descendant_idx = (ssize_t)idx;
      } else if (folder_idx >= 0 && is_descendant_of(item, target)) {
        last_descendant_idx = (ssize_t)idx;
      } else if (folder_idx >= 0) {
        break;
      }
      idx++;
    }
    return (last_descendant_idx >= 0) ? last_descendant_idx + 1 : -1;
  }

  /* Find after_child and its last visible descendant */
  size_t idx = 0;
  ssize_t after_child_idx = -1;
  ssize_t last_descendant_idx = -1;

  for (size_t i = 0; i < visible_count; i++) {
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
    if (item == ds->move.source) continue;

    if (item == after_child) {
      after_child_idx = (ssize_t)idx;
      last_descendant_idx = (ssize_t)idx;
    } else if (after_child_idx >= 0) {
      if (is_descendant_of(item, after_child)) {
        last_descendant_idx = (ssize_t)idx;
      } else {
        break;  /* No longer a descendant */
      }
    }
    idx++;
  }

  return (last_descendant_idx >= 0) ? last_descendant_idx + 1 : -1;
}

/* Get first key display for a hotkey action (returns static buffer, max 8 chars) */
static const char *get_first_key_hint(const Config *config, HotkeyAction action) {
  static char buf[16];
  buf[0] = '\0';

  if (!config)
    return buf;

  char *display = hotkey_get_display(config, action);
  if (display) {
    /* Take only first key (before comma) */
    char *comma = strchr(display, ',');
    if (comma) {
      size_t len = (size_t)(comma - display);
      if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
      strncpy(buf, display, len);
      buf[len] = '\0';
    } else {
      strncpy(buf, display, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
    }
    free(display);
  }
  return buf;
}

static void draw_tree_panel(WINDOW *win, DialogState *ds, int start_y, int start_x,
                            int height, int width) {
  /* Panel header */
  wattron(win, A_BOLD);
  mvwprintw(win, start_y, start_x, "Saved Connections");
  wattroff(win, A_BOLD);

  /* Show move target folder info */
  if (ds->move.active && ds->move.target_folder) {
    const char *folder_name = (ds->move.target_folder == &ds->mgr->root)
                              ? "(root)"
                              : connmgr_item_name(ds->move.target_folder);
    wattron(win, COLOR_PAIR(COLOR_NUMBER));
    mvwprintw(win, start_y, start_x + 18, " -> %s", folder_name);
    wattroff(win, COLOR_PAIR(COLOR_NUMBER));
  }

  int tree_y = start_y + 1;
  int tree_height = height - 4;

  size_t visible_count = connmgr_count_visible(ds->mgr);

  if (visible_count == 0 || (ds->move.active && visible_count == 1)) {
    wattron(win, A_DIM);
    mvwprintw(win, tree_y + 1, start_x + 1, "(no saved connections)");
    wattroff(win, A_DIM);

    /* If moving and tree is empty (only source), show the item */
    if (ds->move.active) {
      int depth = get_move_target_depth(ds);
      draw_moving_item_at_depth(win, ds, tree_y + 2, start_x, width, depth);
    }
  } else if (!ds->move.active) {
    /* Normal mode - draw items in order */
    if (ds->tree_highlight >= ds->tree_scroll + (size_t)tree_height) {
      ds->tree_scroll = ds->tree_highlight - (size_t)tree_height + 1;
    }
    if (ds->tree_highlight < ds->tree_scroll) {
      ds->tree_scroll = ds->tree_highlight;
    }

    int draw_y = tree_y;
    for (size_t i = ds->tree_scroll; i < visible_count && draw_y < tree_y + tree_height; i++) {
      ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
      if (!item) continue;
      bool is_cursor = (i == ds->tree_highlight);
      draw_tree_item(win, draw_y, start_x, width, item, is_cursor, ds->focus == FOCUS_TREE, false);
      draw_y++;
    }
  } else {
    /* Move mode - draw tree with source at computed visual position */
    int draw_y = tree_y;
    bool source_drawn = false;
    ssize_t source_visual_pos = compute_source_visual_position(ds);
    size_t idx_excluding_source = 0;

    /* Handle insert at very beginning (position 0) */
    if (source_visual_pos == 0) {
      int depth = get_move_target_depth(ds);
      draw_moving_item_at_depth(win, ds, draw_y, start_x, width, depth);
      draw_y++;
      source_drawn = true;
    }

    for (size_t i = 0; i < visible_count && draw_y < tree_y + tree_height; i++) {
      ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
      if (!item) continue;

      /* Skip the source item (it's drawn at target position) */
      if (is_source_item(ds, item))
        continue;

      /* Check if source should be drawn BEFORE this item */
      if (!source_drawn && (ssize_t)idx_excluding_source == source_visual_pos) {
        int depth = get_move_target_depth(ds);
        draw_moving_item_at_depth(win, ds, draw_y, start_x, width, depth);
        draw_y++;
        source_drawn = true;
        if (draw_y >= tree_y + tree_height) break;
      }

      /* Draw the regular item */
      draw_tree_item(win, draw_y, start_x, width, item, false, false, false);
      draw_y++;
      idx_excluding_source++;
    }

    /* If source not yet drawn, draw at end */
    if (!source_drawn && draw_y < tree_y + tree_height) {
      int depth = get_move_target_depth(ds);
      draw_moving_item_at_depth(win, ds, draw_y, start_x, width, depth);
    }
  }

  /* Draw shortcut hints at bottom */
  int hint_y = start_y + height - 2;
  wattron(win, A_DIM);
  if (ds->move.active) {
    mvwprintw(win, hint_y, start_x, "Space:drop Esc:cancel");
  } else {
    /* Build hints from configured hotkeys (copy to avoid static buffer reuse) */
    char new_key[16], folder_key[16], edit_key[16], del_key[16], rename_key[16];
    strncpy(new_key, get_first_key_hint(ds->config, HOTKEY_CONN_NEW), sizeof(new_key) - 1);
    new_key[sizeof(new_key) - 1] = '\0';
    strncpy(folder_key, get_first_key_hint(ds->config, HOTKEY_CONN_NEW_FOLDER), sizeof(folder_key) - 1);
    folder_key[sizeof(folder_key) - 1] = '\0';
    strncpy(edit_key, get_first_key_hint(ds->config, HOTKEY_CONN_EDIT), sizeof(edit_key) - 1);
    edit_key[sizeof(edit_key) - 1] = '\0';
    strncpy(del_key, get_first_key_hint(ds->config, HOTKEY_CONN_DELETE), sizeof(del_key) - 1);
    del_key[sizeof(del_key) - 1] = '\0';
    strncpy(rename_key, get_first_key_hint(ds->config, HOTKEY_CONN_RENAME), sizeof(rename_key) - 1);
    rename_key[sizeof(rename_key) - 1] = '\0';

    mvwprintw(win, hint_y, start_x, "%s:new %s:folder %s:edit",
              new_key[0] ? new_key : "n",
              folder_key[0] ? folder_key : "N",
              edit_key[0] ? edit_key : "e");
    mvwprintw(win, hint_y + 1, start_x, "Space:move %s:del %s:rename",
              del_key[0] ? del_key : "d",
              rename_key[0] ? rename_key : "r");
  }
  wattroff(win, A_DIM);
}

/* ============================================================================
 * URL Panel
 * ============================================================================
 */

static void draw_url_panel(WINDOW *win, DialogState *ds, int start_y, int start_x,
                           int width, int *cursor_y, int *cursor_x) {
  (void)width; /* May be used for layout adjustments later */

  /* Panel header */
  wattron(win, A_BOLD);
  mvwprintw(win, start_y, start_x, "Quick Connect");
  wattroff(win, A_BOLD);

  int y = start_y + 2;

  /* URL label */
  mvwprintw(win, y, start_x, "URL:");
  y++;

  /* Input field */
  input_draw(win, &ds->url_input, y, start_x, ds->focus == FOCUS_URL,
             cursor_y, cursor_x);
  y += 3;

  /* Examples */
  wattron(win, A_DIM);
  mvwprintw(win, y++, start_x, "Examples:");
  mvwprintw(win, y++, start_x + 2, "sqlite:///path/to/db.sqlite");
  mvwprintw(win, y++, start_x + 2, "postgres://user:pass@host/db");
  mvwprintw(win, y++, start_x + 2, "mysql://user@host:3306/db");
  wattroff(win, A_DIM);
}

/* ============================================================================
 * Buttons
 * ============================================================================
 */

static void draw_buttons(WINDOW *win, DialogState *ds, int y, int width) {
  const char *buttons[] = {"Connect", "New WS", "Test", "Save", "Delete", "Close", "Quit"};
  int btn_widths[] = {9, 8, 6, 6, 8, 7, 6};
  int total_width = 0;

  for (int i = 0; i < BTN_COUNT; i++) {
    total_width += btn_widths[i] + 4; /* [ ] + spaces */
  }

  int x = (width - total_width) / 2;
  bool btn_focused = (ds->focus == FOCUS_BUTTONS);

  for (int i = 0; i < BTN_COUNT; i++) {
    bool selected = (ds->selected_button == i);

    if (selected && btn_focused) {
      wattron(win, A_REVERSE | A_BOLD);
    }

    mvwprintw(win, y, x, "[ %s ]", buttons[i]);

    if (selected && btn_focused) {
      wattroff(win, A_REVERSE | A_BOLD);
    }

    x += btn_widths[i] + 4;
  }
}

/* ============================================================================
 * Main Dialog Drawing
 * ============================================================================
 */

static void draw_dialog(WINDOW *win, DialogState *ds, int *cursor_y, int *cursor_x) {
  werase(win);
  box(win, 0, 0);

  /* Title */
  wattron(win, A_BOLD);
  mvwprintw(win, 0, (ds->width - 20) / 2, " Connection Manager ");
  wattroff(win, A_BOLD);

  /* Horizontal line above buttons (calculate first for divider) */
  int btn_line_y = ds->height - 4;

  /* Vertical divider between panels - stop at button bar */
  int divider_x = TREE_PANEL_WIDTH + 2;
  for (int i = 1; i < btn_line_y; i++) {
    mvwaddch(win, i, divider_x, ACS_VLINE);
  }
  mvwaddch(win, 0, divider_x, ACS_TTEE);

  /* Left panel: Saved connections tree */
  draw_tree_panel(win, ds, 2, 2, ds->tree_height, TREE_PANEL_WIDTH);

  /* Right panel: Quick connect URL */
  int url_panel_x = divider_x + 2;
  int url_panel_width = ds->width - url_panel_x - 2;
  draw_url_panel(win, ds, 2, url_panel_x, url_panel_width, cursor_y, cursor_x);

  /* Horizontal line above buttons */
  mvwaddch(win, btn_line_y, 0, ACS_LTEE);
  mvwhline(win, btn_line_y, 1, ACS_HLINE, ds->width - 2);
  mvwaddch(win, btn_line_y, ds->width - 1, ACS_RTEE);
  mvwaddch(win, btn_line_y, divider_x, ACS_BTEE);

  /* Error/success messages */
  int msg_y = ds->height - 3;
  int msg_max_len = ds->width - 4;  /* Leave margin on both sides */
  if (msg_max_len < 10) msg_max_len = 10;
  if (ds->error_msg && ds->error_msg[0]) {
    wattron(win, COLOR_PAIR(COLOR_ERROR));
    mvwprintw(win, msg_y, 2, "%.*s", msg_max_len, ds->error_msg);
    wattroff(win, COLOR_PAIR(COLOR_ERROR));
  } else if (ds->success_msg && ds->success_msg[0]) {
    wattron(win, COLOR_PAIR(COLOR_NUMBER));
    mvwprintw(win, msg_y, 2, "%.*s", msg_max_len, ds->success_msg);
    wattroff(win, COLOR_PAIR(COLOR_NUMBER));
  }

  /* Buttons */
  draw_buttons(win, ds, ds->height - 2, ds->width);

  /* Position cursor */
  if (ds->focus == FOCUS_URL && cursor_y && cursor_x) {
    wmove(win, *cursor_y, *cursor_x);
  }

  wrefresh(win);
}

/* ============================================================================
 * Input Dialog (for names)
 * ============================================================================
 */

static char *show_input_dialog(WINDOW *parent, const char *title,
                               const char *label, const char *initial_value) {
  int parent_h, parent_w;
  getmaxyx(parent, parent_h, parent_w);
  (void)parent_h;

  int dlg_height = 8;
  int dlg_width = 50;
  if (dlg_width > parent_w - 10) dlg_width = parent_w - 10;

  int dlg_y = 5;
  int dlg_x = (parent_w - dlg_width) / 2;

  WINDOW *dlg = derwin(parent, dlg_height, dlg_width, dlg_y, dlg_x);
  if (!dlg) return NULL;

  keypad(dlg, TRUE);

  char buf[128];
  size_t len = 0;
  size_t cursor = 0;

  if (initial_value && initial_value[0]) {
    strncpy(buf, initial_value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    len = strlen(buf);
    cursor = len;
  } else {
    buf[0] = '\0';
  }

  /* Focus: 0 = input, 1 = OK button, 2 = Cancel button */
  int focus = 0;

  char *result = NULL;
  bool running = true;

  while (running) {
    werase(dlg);
    box(dlg, 0, 0);

    int title_len = (int)strlen(title) + 2;
    wattron(dlg, A_BOLD);
    mvwprintw(dlg, 0, (dlg_width - title_len) / 2, " %s ", title);
    wattroff(dlg, A_BOLD);

    mvwprintw(dlg, 2, 2, "%s", label);

    if (focus == 0) {
      wattron(dlg, COLOR_PAIR(COLOR_SELECTED));
    }
    mvwhline(dlg, 3, 2, ' ', dlg_width - 4);
    mvwaddnstr(dlg, 3, 2, buf, dlg_width - 5);
    if (focus == 0) {
      wattroff(dlg, COLOR_PAIR(COLOR_SELECTED));
    }

    wattron(dlg, A_DIM);
    mvwaddch(dlg, 4, 0, ACS_LTEE);
    mvwhline(dlg, 4, 1, ACS_HLINE, dlg_width - 2);
    mvwaddch(dlg, 4, dlg_width - 1, ACS_RTEE);
    wattroff(dlg, A_DIM);

    /* Buttons */
    int btn_x = dlg_width / 2 - 10;
    if (focus == 1) wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 6, btn_x, "[ OK ]");
    if (focus == 1) wattroff(dlg, A_REVERSE);
    if (focus == 2) wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 6, btn_x + 8, "[ Cancel ]");
    if (focus == 2) wattroff(dlg, A_REVERSE);

    if (focus == 0) {
      wmove(dlg, 3, 2 + (int)cursor);
      curs_set(1);
    } else {
      curs_set(0);
    }
    wrefresh(dlg);

    int ch = wgetch(dlg);
    UiEvent event;
    render_translate_key(ch, &event);

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_DOWN)) {
      focus = (focus + 1) % 3;
    } else if (render_event_is_special(&event, UI_KEY_UP)) {
      focus = (focus + 2) % 3;
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (focus == 2) {
        /* Cancel */
        running = false;
      } else {
        /* OK (from input or OK button) */
        if (len > 0) {
          result = str_dup(buf);
        }
        running = false;
      }
    } else if (focus == 0) {
      /* Input field handling */
      if (render_event_is_special(&event, UI_KEY_BACKSPACE)) {
        if (cursor > 0 && cursor <= len) {
          memmove(buf + cursor - 1, buf + cursor, len - cursor + 1);
          cursor--;
          len--;
        }
      } else if (render_event_is_special(&event, UI_KEY_LEFT)) {
        if (cursor > 0) cursor--;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (cursor < len) cursor++;
      } else if (render_event_is_special(&event, UI_KEY_HOME)) {
        cursor = 0;
      } else if (render_event_is_special(&event, UI_KEY_END)) {
        cursor = len;
      } else {
        int key_char = render_event_get_char(&event);
        if (render_event_is_char(&event) && key_char >= 32 && key_char < 127 &&
            len < sizeof(buf) - 1) {
          memmove(buf + cursor + 1, buf + cursor, len - cursor + 1);
          buf[cursor] = (char)key_char;
          cursor++;
          len++;
        }
      }
    } else {
      /* Button navigation with left/right */
      if (render_event_is_special(&event, UI_KEY_LEFT)) {
        if (focus == 2) focus = 1;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (focus == 1) focus = 2;
      }
    }
  }

  curs_set(0);
  delwin(dlg);
  return result;
}

/* Confirmation dialog - returns true if user confirms */
static bool show_confirm_dialog(WINDOW *parent, const char *title,
                                const char *message) {
  int parent_h, parent_w;
  getmaxyx(parent, parent_h, parent_w);
  (void)parent_h;

  int dlg_height = 7;
  int dlg_width = 50;
  if (dlg_width > parent_w - 10) dlg_width = parent_w - 10;

  int dlg_y = 5;
  int dlg_x = (parent_w - dlg_width) / 2;

  WINDOW *dlg = derwin(parent, dlg_height, dlg_width, dlg_y, dlg_x);
  if (!dlg) return false;

  keypad(dlg, TRUE);

  /* Focus: 0 = Yes, 1 = No */
  int focus = 1;  /* Default to No for safety */

  bool result = false;
  bool running = true;

  while (running) {
    werase(dlg);
    box(dlg, 0, 0);

    int title_len = (int)strlen(title) + 2;
    wattron(dlg, A_BOLD);
    mvwprintw(dlg, 0, (dlg_width - title_len) / 2, " %s ", title);
    wattroff(dlg, A_BOLD);

    /* Message */
    int msg_x = (dlg_width - (int)strlen(message)) / 2;
    if (msg_x < 2) msg_x = 2;
    mvwprintw(dlg, 2, msg_x, "%s", message);

    /* Buttons */
    int btn_x = dlg_width / 2 - 10;
    if (focus == 0) wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 5, btn_x, "[ Yes ]");
    if (focus == 0) wattroff(dlg, A_REVERSE);
    if (focus == 1) wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 5, btn_x + 9, "[ No ]");
    if (focus == 1) wattroff(dlg, A_REVERSE);

    curs_set(0);
    wrefresh(dlg);

    int ch = wgetch(dlg);
    UiEvent event;
    render_translate_key(ch, &event);

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_LEFT) ||
               render_event_is_special(&event, UI_KEY_RIGHT) ||
               render_event_get_char(&event) == 'h' ||
               render_event_get_char(&event) == 'l') {
      focus = 1 - focus;  /* Toggle between 0 and 1 */
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      result = (focus == 0);
      running = false;
    } else if (render_event_get_char(&event) == 'y' ||
               render_event_get_char(&event) == 'Y') {
      result = true;
      running = false;
    } else if (render_event_get_char(&event) == 'n' ||
               render_event_get_char(&event) == 'N') {
      result = false;
      running = false;
    }
  }

  delwin(dlg);
  return result;
}

/* Password input dialog (masks input with asterisks) */
static char *show_password_dialog(WINDOW *parent, const char *title,
                                  const char *label) {
  int parent_h, parent_w;
  getmaxyx(parent, parent_h, parent_w);
  (void)parent_h;

  int dlg_height = 8;
  int dlg_width = 50;
  if (dlg_width > parent_w - 10) dlg_width = parent_w - 10;

  int dlg_y = 5;
  int dlg_x = (parent_w - dlg_width) / 2;

  WINDOW *dlg = derwin(parent, dlg_height, dlg_width, dlg_y, dlg_x);
  if (!dlg) return NULL;

  keypad(dlg, TRUE);

  char buf[128];
  size_t len = 0;
  size_t cursor = 0;
  buf[0] = '\0';

  /* Focus: 0 = input, 1 = OK button, 2 = Cancel button */
  int focus = 0;

  char *result = NULL;
  bool running = true;

  while (running) {
    werase(dlg);
    box(dlg, 0, 0);

    int title_len = (int)strlen(title) + 2;
    wattron(dlg, A_BOLD);
    mvwprintw(dlg, 0, (dlg_width - title_len) / 2, " %s ", title);
    wattroff(dlg, A_BOLD);

    mvwprintw(dlg, 2, 2, "%s", label);

    if (focus == 0) {
      wattron(dlg, COLOR_PAIR(COLOR_SELECTED));
    }
    mvwhline(dlg, 3, 2, ' ', dlg_width - 4);
    /* Show asterisks instead of actual password */
    for (size_t i = 0; i < len && (int)i < dlg_width - 5; i++) {
      mvwaddch(dlg, 3, 2 + (int)i, '*');
    }
    if (focus == 0) {
      wattroff(dlg, COLOR_PAIR(COLOR_SELECTED));
    }

    wattron(dlg, A_DIM);
    mvwaddch(dlg, 4, 0, ACS_LTEE);
    mvwhline(dlg, 4, 1, ACS_HLINE, dlg_width - 2);
    mvwaddch(dlg, 4, dlg_width - 1, ACS_RTEE);
    wattroff(dlg, A_DIM);

    /* Buttons */
    int btn_x = dlg_width / 2 - 10;
    if (focus == 1) wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 6, btn_x, "[ OK ]");
    if (focus == 1) wattroff(dlg, A_REVERSE);
    if (focus == 2) wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 6, btn_x + 8, "[ Cancel ]");
    if (focus == 2) wattroff(dlg, A_REVERSE);

    if (focus == 0) {
      wmove(dlg, 3, 2 + (int)cursor);
      curs_set(1);
    } else {
      curs_set(0);
    }
    wrefresh(dlg);

    int ch = wgetch(dlg);
    UiEvent event;
    render_translate_key(ch, &event);

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_DOWN)) {
      focus = (focus + 1) % 3;
    } else if (render_event_is_special(&event, UI_KEY_UP)) {
      focus = (focus + 2) % 3;
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (focus == 2) {
        /* Cancel */
        running = false;
      } else {
        /* OK - return even if empty (user might want empty password) */
        result = str_dup(buf);
        running = false;
      }
    } else if (focus == 0) {
      /* Input field handling */
      if (render_event_is_special(&event, UI_KEY_BACKSPACE)) {
        if (cursor > 0 && cursor <= len) {
          memmove(buf + cursor - 1, buf + cursor, len - cursor + 1);
          cursor--;
          len--;
        }
      } else if (render_event_is_special(&event, UI_KEY_LEFT)) {
        if (cursor > 0) cursor--;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (cursor < len) cursor++;
      } else if (render_event_is_special(&event, UI_KEY_HOME)) {
        cursor = 0;
      } else if (render_event_is_special(&event, UI_KEY_END)) {
        cursor = len;
      } else {
        int key_char = render_event_get_char(&event);
        if (render_event_is_char(&event) && key_char >= 32 && key_char < 127 &&
            len < sizeof(buf) - 1) {
          memmove(buf + cursor + 1, buf + cursor, len - cursor + 1);
          buf[cursor] = (char)key_char;
          cursor++;
          len++;
        }
      }
    } else {
      /* Button navigation with left/right */
      if (render_event_is_special(&event, UI_KEY_LEFT)) {
        if (focus == 2) focus = 1;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (focus == 1) focus = 2;
      }
    }
  }

  curs_set(0);
  delwin(dlg);
  return result;
}

/* ============================================================================
 * Folder Picker Helper
 * ============================================================================
 */

typedef struct {
  ConnectionItem **folders;
  size_t num_folders;
  size_t capacity;
} FolderList;

static void collect_folders_recursive(ConnectionItem *item, FolderList *list) {
  if (item->type != CONN_ITEM_FOLDER)
    return;

  /* Add this folder */
  if (list->num_folders >= list->capacity) {
    size_t new_cap = list->capacity == 0 ? 8 : list->capacity * 2;
    ConnectionItem **new_folders = realloc(list->folders, new_cap * sizeof(ConnectionItem *));
    if (!new_folders)
      return;
    list->folders = new_folders;
    list->capacity = new_cap;
  }
  list->folders[list->num_folders++] = item;

  /* Recurse into children */
  for (size_t i = 0; i < item->folder.num_children; i++) {
    collect_folders_recursive(&item->folder.children[i], list);
  }
}

static FolderList collect_all_folders(ConnectionManager *mgr) {
  FolderList list = {NULL, 0, 0};
  collect_folders_recursive(&mgr->root, &list);
  return list;
}

static void free_folder_list(FolderList *list) {
  free(list->folders);
  list->folders = NULL;
  list->num_folders = 0;
  list->capacity = 0;
}

/* Get display name with indentation for folder */
static char *folder_display_name(ConnectionItem *folder) {
  int depth = connmgr_get_item_depth(folder);
  const char *name = connmgr_item_name(folder);

  /* Root folder shows as "(root)" */
  if (!folder->parent) {
    return str_dup("(root)");
  }

  char *result = malloc(depth * 2 + strlen(name) + 1);
  if (!result)
    return str_dup(name);

  for (int i = 0; i < depth * 2; i++) {
    result[i] = ' ';
  }
  strcpy(result + depth * 2, name);
  return result;
}

/* ============================================================================
 * Save Connection Dialog
 * ============================================================================
 */

static bool show_save_dialog(WINDOW *parent, ConnectionManager *mgr,
                             const char *url, char **error) {
  /* Parse URL into connection */
  char *parse_err = NULL;
  SavedConnection *conn = connmgr_parse_connstr(url, &parse_err);
  if (!conn) {
    *error = parse_err ? parse_err : str_dup("Invalid connection URL");
    return false;
  }

  /* Collect all folders for the picker */
  FolderList folders = collect_all_folders(mgr);
  size_t selected_folder = 0;  /* Default to root */

  int parent_h, parent_w;
  getmaxyx(parent, parent_h, parent_w);
  (void)parent_h;

  /* Dialog with name input and folder picker */
  int dlg_height = 12;
  int dlg_width = 50;
  if (dlg_width > parent_w - 10) dlg_width = parent_w - 10;

  int dlg_y = 4;
  int dlg_x = (parent_w - dlg_width) / 2;

  WINDOW *dlg = derwin(parent, dlg_height, dlg_width, dlg_y, dlg_x);
  if (!dlg) {
    connmgr_free_connection(conn);
    free(conn);
    free_folder_list(&folders);
    *error = str_dup("Failed to create dialog");
    return false;
  }

  keypad(dlg, TRUE);

  char name_buf[128];
  size_t name_len = 0;
  size_t name_cursor = 0;

  /* Pre-fill with parsed name */
  if (conn->name && conn->name[0]) {
    strncpy(name_buf, conn->name, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    name_len = strlen(name_buf);
    name_cursor = name_len;
  } else {
    name_buf[0] = '\0';
  }

  /* Focus: 0 = name, 1 = folder, 2 = Save button, 3 = Cancel button */
  int focus = 0;

  bool result = false;
  bool running = true;

  while (running) {
    werase(dlg);
    box(dlg, 0, 0);

    wattron(dlg, A_BOLD);
    mvwprintw(dlg, 0, (dlg_width - 18) / 2, " Save Connection ");
    wattroff(dlg, A_BOLD);

    /* Name input */
    mvwprintw(dlg, 2, 2, "Name:");
    if (focus == 0) {
      wattron(dlg, COLOR_PAIR(COLOR_SELECTED));
    }
    mvwhline(dlg, 3, 2, ' ', dlg_width - 4);
    mvwaddnstr(dlg, 3, 2, name_buf, dlg_width - 5);
    if (focus == 0) {
      wattroff(dlg, COLOR_PAIR(COLOR_SELECTED));
    }

    /* Folder picker */
    mvwprintw(dlg, 5, 2, "Folder:");
    if (focus == 1) {
      wattron(dlg, COLOR_PAIR(COLOR_SELECTED));
    }
    mvwhline(dlg, 6, 2, ' ', dlg_width - 4);

    /* Show selected folder with arrows */
    char *folder_name = NULL;
    if (selected_folder < folders.num_folders) {
      folder_name = folder_display_name(folders.folders[selected_folder]);
    } else {
      folder_name = str_dup("(root)");
    }
    mvwprintw(dlg, 6, 2, "< %s >", folder_name ? folder_name : "(root)");
    free(folder_name);

    if (focus == 1) {
      wattroff(dlg, COLOR_PAIR(COLOR_SELECTED));
    }

    /* Separator with T-junctions */
    wattron(dlg, A_DIM);
    mvwaddch(dlg, 8, 0, ACS_LTEE);
    mvwhline(dlg, 8, 1, ACS_HLINE, dlg_width - 2);
    mvwaddch(dlg, 8, dlg_width - 1, ACS_RTEE);
    wattroff(dlg, A_DIM);

    /* Buttons */
    int btn_x = dlg_width / 2 - 12;
    if (focus == 2) wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 10, btn_x, "[ Save ]");
    if (focus == 2) wattroff(dlg, A_REVERSE);
    if (focus == 3) wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 10, btn_x + 10, "[ Cancel ]");
    if (focus == 3) wattroff(dlg, A_REVERSE);

    /* Position cursor */
    if (focus == 0) {
      wmove(dlg, 3, 2 + (int)name_cursor);
      curs_set(1);
    } else {
      curs_set(0);
    }

    wrefresh(dlg);

    int ch = wgetch(dlg);
    UiEvent event;
    render_translate_key(ch, &event);

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_DOWN)) {
      focus = (focus + 1) % 4;
    } else if (render_event_is_special(&event, UI_KEY_UP)) {
      focus = (focus + 3) % 4;
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (focus == 3) {
        /* Cancel */
        running = false;
      } else if (name_len > 0) {
        /* Save the connection */
        free(conn->name);
        conn->name = str_dup(name_buf);

        /* Get target folder */
        ConnectionItem *target = &mgr->root;
        if (selected_folder < folders.num_folders) {
          target = folders.folders[selected_folder];
        }

        if (connmgr_add_connection(target, conn)) {
          mgr->modified = true;
          result = true;
        } else {
          *error = str_dup("Failed to add connection");
          connmgr_free_connection(conn);
          free(conn);
        }
        running = false;
      }
    } else if (focus == 0) {
      /* Name input handling */
      if (render_event_is_special(&event, UI_KEY_BACKSPACE)) {
        if (name_cursor > 0 && name_cursor <= name_len) {
          memmove(name_buf + name_cursor - 1, name_buf + name_cursor,
                  name_len - name_cursor + 1);
          name_cursor--;
          name_len--;
        }
      } else if (render_event_is_special(&event, UI_KEY_LEFT)) {
        if (name_cursor > 0) name_cursor--;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (name_cursor < name_len) name_cursor++;
      } else if (render_event_is_special(&event, UI_KEY_HOME)) {
        name_cursor = 0;
      } else if (render_event_is_special(&event, UI_KEY_END)) {
        name_cursor = name_len;
      } else {
        int key_char = render_event_get_char(&event);
        if (render_event_is_char(&event) && key_char >= 32 && key_char < 127 &&
            name_len < sizeof(name_buf) - 1) {
          memmove(name_buf + name_cursor + 1, name_buf + name_cursor,
                  name_len - name_cursor + 1);
          name_buf[name_cursor] = (char)key_char;
          name_cursor++;
          name_len++;
        }
      }
    } else if (focus == 1) {
      /* Folder picker handling */
      if (render_event_is_special(&event, UI_KEY_LEFT) ||
          render_event_get_char(&event) == 'h') {
        if (selected_folder > 0) {
          selected_folder--;
        } else {
          selected_folder = folders.num_folders - 1;
        }
      } else if (render_event_is_special(&event, UI_KEY_RIGHT) ||
                 render_event_get_char(&event) == 'l') {
        if (folders.num_folders > 0) {
          selected_folder = (selected_folder + 1) % folders.num_folders;
        }
      }
    } else {
      /* Button navigation with left/right */
      if (render_event_is_special(&event, UI_KEY_LEFT)) {
        if (focus == 3) focus = 2;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (focus == 2) focus = 3;
      }
    }
  }

  curs_set(0);
  delwin(dlg);
  free_folder_list(&folders);

  if (!result && !*error) {
    connmgr_free_connection(conn);
    free(conn);
  }

  return result;
}

/* ============================================================================
 * New/Edit Connection Dialog
 * ============================================================================
 */

typedef struct {
  char name[128];
  char driver[32];   /* sqlite, postgres, mysql, mariadb */
  char host[128];
  char port[8];
  char database[256];
  char user[64];
  char password[64];
  bool save_password;
} ConnectionFormData;

static bool show_connection_form(WINDOW *parent, ConnectionManager *mgr,
                                 ConnectionItem *edit_item, /* NULL for new */
                                 ConnectionItem *parent_folder,
                                 char **error) {
  int parent_h, parent_w;
  getmaxyx(parent, parent_h, parent_w);
  (void)parent_h;

  int dlg_height = 20;
  int dlg_width = 60;
  if (dlg_width > parent_w - 6) dlg_width = parent_w - 6;

  int dlg_y = 2;
  int dlg_x = (parent_w - dlg_width) / 2;

  WINDOW *dlg = derwin(parent, dlg_height, dlg_width, dlg_y, dlg_x);
  if (!dlg) {
    *error = str_dup("Failed to create dialog");
    return false;
  }

  keypad(dlg, TRUE);

  ConnectionFormData form = {0};
  strcpy(form.driver, "postgres");  /* Default driver */

  /* Pre-fill if editing */
  if (edit_item && connmgr_is_connection(edit_item)) {
    SavedConnection *conn = &edit_item->connection;
    if (conn->name) strncpy(form.name, conn->name, sizeof(form.name) - 1);
    if (conn->driver) strncpy(form.driver, conn->driver, sizeof(form.driver) - 1);
    if (conn->host) strncpy(form.host, conn->host, sizeof(form.host) - 1);
    if (conn->port > 0) snprintf(form.port, sizeof(form.port), "%d", conn->port);
    if (conn->database) strncpy(form.database, conn->database, sizeof(form.database) - 1);
    if (conn->user) strncpy(form.user, conn->user, sizeof(form.user) - 1);
    if (conn->password) strncpy(form.password, conn->password, sizeof(form.password) - 1);
    form.save_password = conn->save_password;
  }

  const char *drivers[] = {"sqlite", "postgres", "mysql", "mariadb"};
  int num_drivers = 4;
  int current_driver = 1;  /* postgres default */

  /* Find current driver index */
  for (int i = 0; i < num_drivers; i++) {
    if (str_eq(form.driver, drivers[i])) {
      current_driver = i;
      break;
    }
  }

  /* Field definitions */
  enum { FLD_NAME, FLD_DRIVER, FLD_HOST, FLD_PORT, FLD_DATABASE, FLD_USER, FLD_PASSWORD, FLD_SAVE_PWD, FLD_SAVE_BTN, FLD_CANCEL_BTN, FLD_COUNT };
  int focus = FLD_NAME;
  size_t cursors[FLD_COUNT] = {0};

  /* Set initial cursor positions to end of each field */
  cursors[FLD_NAME] = strlen(form.name);
  cursors[FLD_HOST] = strlen(form.host);
  cursors[FLD_PORT] = strlen(form.port);
  cursors[FLD_DATABASE] = strlen(form.database);
  cursors[FLD_USER] = strlen(form.user);
  cursors[FLD_PASSWORD] = strlen(form.password);

  bool result = false;
  bool running = true;

  while (running) {
    werase(dlg);
    box(dlg, 0, 0);

    const char *title = edit_item ? " Edit Connection " : " New Connection ";
    wattron(dlg, A_BOLD);
    mvwprintw(dlg, 0, (dlg_width - (int)strlen(title)) / 2, "%s", title);
    wattroff(dlg, A_BOLD);

    int y = 2;
    int label_w = 12;
    int field_w = dlg_width - label_w - 4;

    /* Helper macro to draw field */
    #define DRAW_FIELD(fld, label, buf, is_password) do { \
      mvwprintw(dlg, y, 2, "%s:", label); \
      if (focus == fld) wattron(dlg, COLOR_PAIR(COLOR_SELECTED)); \
      mvwhline(dlg, y, label_w + 2, ' ', field_w); \
      if (is_password) { \
        for (size_t _i = 0; _i < strlen(buf); _i++) mvwaddch(dlg, y, label_w + 2 + (int)_i, '*'); \
      } else { \
        mvwaddnstr(dlg, y, label_w + 2, buf, field_w - 1); \
      } \
      if (focus == fld) wattroff(dlg, COLOR_PAIR(COLOR_SELECTED)); \
      y++; \
    } while(0)

    DRAW_FIELD(FLD_NAME, "Name", form.name, false);
    y++;  /* Spacer */

    /* Driver selector */
    mvwprintw(dlg, y, 2, "Driver:");
    if (focus == FLD_DRIVER) wattron(dlg, COLOR_PAIR(COLOR_SELECTED));
    mvwprintw(dlg, y, label_w + 2, "< %s >", drivers[current_driver]);
    if (focus == FLD_DRIVER) wattroff(dlg, COLOR_PAIR(COLOR_SELECTED));
    y++;

    DRAW_FIELD(FLD_HOST, "Host", form.host, false);
    DRAW_FIELD(FLD_PORT, "Port", form.port, false);
    DRAW_FIELD(FLD_DATABASE, "Database", form.database, false);
    y++;  /* Spacer */
    DRAW_FIELD(FLD_USER, "User", form.user, false);
    DRAW_FIELD(FLD_PASSWORD, "Password", form.password, true);

    /* Save password checkbox */
    mvwprintw(dlg, y, label_w + 2, "[%c] Save password", form.save_password ? 'X' : ' ');
    if (focus == FLD_SAVE_PWD) {
      mvwchgat(dlg, y, label_w + 2, 18, A_REVERSE, 0, NULL);
    }
    y += 2;

    #undef DRAW_FIELD

    /* Separator */
    mvwaddch(dlg, y, 0, ACS_LTEE);
    mvwhline(dlg, y, 1, ACS_HLINE, dlg_width - 2);
    mvwaddch(dlg, y, dlg_width - 1, ACS_RTEE);
    y += 2;

    /* Buttons */
    int btn_x = dlg_width / 2 - 12;
    if (focus == FLD_SAVE_BTN) wattron(dlg, A_REVERSE);
    mvwprintw(dlg, y, btn_x, "[ Save ]");
    if (focus == FLD_SAVE_BTN) wattroff(dlg, A_REVERSE);
    if (focus == FLD_CANCEL_BTN) wattron(dlg, A_REVERSE);
    mvwprintw(dlg, y, btn_x + 10, "[ Cancel ]");
    if (focus == FLD_CANCEL_BTN) wattroff(dlg, A_REVERSE);

    /* Position cursor */
    if (focus >= FLD_NAME && focus <= FLD_PASSWORD && focus != FLD_DRIVER) {
      char *field = NULL;
      int field_y = 0;
      switch (focus) {
        case FLD_NAME: field = form.name; field_y = 2; break;
        case FLD_HOST: field = form.host; field_y = 5; break;
        case FLD_PORT: field = form.port; field_y = 6; break;
        case FLD_DATABASE: field = form.database; field_y = 7; break;
        case FLD_USER: field = form.user; field_y = 9; break;
        case FLD_PASSWORD: field = form.password; field_y = 10; break;
      }
      if (field) {
        wmove(dlg, field_y, label_w + 2 + (int)cursors[focus]);
        curs_set(1);
      }
    } else {
      curs_set(0);
    }

    wrefresh(dlg);

    int ch = wgetch(dlg);
    UiEvent event;
    render_translate_key(ch, &event);

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_DOWN)) {
      focus = (focus + 1) % FLD_COUNT;
    } else if (render_event_is_special(&event, UI_KEY_UP)) {
      focus = (focus + FLD_COUNT - 1) % FLD_COUNT;
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (focus == FLD_CANCEL_BTN) {
        /* Cancel */
        running = false;
      } else if (focus == FLD_SAVE_PWD) {
        form.save_password = !form.save_password;
      } else if (strlen(form.name) > 0) {
        /* Create/update connection */
        SavedConnection *conn = edit_item ? &edit_item->connection : connmgr_new_connection();
        if (!conn) {
          *error = str_dup("Failed to create connection");
          running = false;
          continue;
        }

        /* Update fields */
        if (!edit_item) {
          /* New connection - need to allocate */
          free(conn->name); conn->name = str_dup(form.name);
          free(conn->driver); conn->driver = str_dup(drivers[current_driver]);
          free(conn->host); conn->host = str_dup(form.host);
          free(conn->database); conn->database = str_dup(form.database);
          free(conn->user); conn->user = str_dup(form.user);
          str_secure_free(conn->password); conn->password = str_dup(form.password);
        } else {
          /* Edit - update in place */
          free(conn->name); conn->name = str_dup(form.name);
          free(conn->driver); conn->driver = str_dup(drivers[current_driver]);
          free(conn->host); conn->host = str_dup(form.host);
          free(conn->database); conn->database = str_dup(form.database);
          free(conn->user); conn->user = str_dup(form.user);
          str_secure_free(conn->password); conn->password = str_dup(form.password);
        }
        conn->port = atoi(form.port);
        conn->save_password = form.save_password;

        if (!edit_item) {
          /* Add new connection to folder */
          ConnectionItem *target = parent_folder ? parent_folder : &mgr->root;
          if (!connmgr_add_connection(target, conn)) {
            *error = str_dup("Failed to add connection");
            connmgr_free_connection(conn);
            free(conn);
          } else {
            mgr->modified = true;
            result = true;
          }
        } else {
          mgr->modified = true;
          result = true;
        }
        running = false;
      }
    } else if (focus == FLD_DRIVER) {
      /* Driver selector */
      if (render_event_is_special(&event, UI_KEY_LEFT) ||
          render_event_get_char(&event) == 'h') {
        current_driver = (current_driver + num_drivers - 1) % num_drivers;
        strcpy(form.driver, drivers[current_driver]);
      } else if (render_event_is_special(&event, UI_KEY_RIGHT) ||
                 render_event_get_char(&event) == 'l') {
        current_driver = (current_driver + 1) % num_drivers;
        strcpy(form.driver, drivers[current_driver]);
      }
    } else if (focus == FLD_SAVE_PWD) {
      if (render_event_get_char(&event) == ' ') {
        form.save_password = !form.save_password;
      }
    } else if (focus == FLD_SAVE_BTN || focus == FLD_CANCEL_BTN) {
      /* Button navigation with left/right */
      if (render_event_is_special(&event, UI_KEY_LEFT)) {
        if (focus == FLD_CANCEL_BTN) focus = FLD_SAVE_BTN;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (focus == FLD_SAVE_BTN) focus = FLD_CANCEL_BTN;
      }
    } else {
      /* Text field input */
      char *field = NULL;
      size_t max_len = 0;
      switch (focus) {
        case FLD_NAME: field = form.name; max_len = sizeof(form.name) - 1; break;
        case FLD_HOST: field = form.host; max_len = sizeof(form.host) - 1; break;
        case FLD_PORT: field = form.port; max_len = sizeof(form.port) - 1; break;
        case FLD_DATABASE: field = form.database; max_len = sizeof(form.database) - 1; break;
        case FLD_USER: field = form.user; max_len = sizeof(form.user) - 1; break;
        case FLD_PASSWORD: field = form.password; max_len = sizeof(form.password) - 1; break;
      }

      if (field) {
        size_t len = strlen(field);
        size_t cursor = cursors[focus];

        if (render_event_is_special(&event, UI_KEY_BACKSPACE)) {
          if (cursor > 0 && cursor <= len) {
            memmove(field + cursor - 1, field + cursor, len - cursor + 1);
            cursors[focus]--;
          }
        } else if (render_event_is_special(&event, UI_KEY_LEFT)) {
          if (cursor > 0) cursors[focus]--;
        } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
          if (cursor < len) cursors[focus]++;
        } else if (render_event_is_special(&event, UI_KEY_HOME)) {
          cursors[focus] = 0;
        } else if (render_event_is_special(&event, UI_KEY_END)) {
          cursors[focus] = len;
        } else {
          int key_char = render_event_get_char(&event);
          /* For port field, only allow digits */
          bool valid = render_event_is_char(&event) && key_char >= 32 && key_char < 127;
          if (focus == FLD_PORT && (key_char < '0' || key_char > '9')) {
            valid = false;
          }
          if (valid && len < max_len) {
            memmove(field + cursor + 1, field + cursor, len - cursor + 1);
            field[cursor] = (char)key_char;
            cursors[focus]++;
          }
        }
      }
    }
  }

  curs_set(0);
  delwin(dlg);
  return result;
}

/* ============================================================================
 * Actions
 * ============================================================================
 */

/* Check if error message indicates authentication failure */
static bool is_auth_error(const char *err) {
  if (!err)
    return false;

  /* PostgreSQL auth errors */
  if (strstr(err, "password authentication failed"))
    return true;
  if (strstr(err, "authentication failed"))
    return true;
  if (strstr(err, "no password supplied"))
    return true;
  if (strstr(err, "FATAL:  password"))
    return true;

  /* MySQL/MariaDB auth errors */
  if (strstr(err, "Access denied"))
    return true;

  return false;
}

static char *try_connect(DialogState *ds, bool test_only, ConnectMode *mode) {
  char *connstr = NULL;
  char *err = NULL;
  SavedConnection *saved_conn = NULL;

  /* Determine source: tree selection or URL */
  if (ds->focus == FOCUS_TREE || (ds->focus == FOCUS_BUTTONS && ds->url_input.len == 0)) {
    /* Use selected saved connection */
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (!item || !connmgr_is_connection(item)) {
      ds->error_msg = str_dup("Select a connection first");
      return NULL;
    }
    saved_conn = &item->connection;
    connstr = connmgr_build_connstr(saved_conn);
    if (!connstr) {
      ds->error_msg = str_dup("Failed to build connection string");
      return NULL;
    }
  } else {
    /* Use URL input */
    if (ds->url_input.len == 0) {
      ds->error_msg = str_dup("Enter a connection URL");
      return NULL;
    }

    /* Check if it's a file path or URL */
    if (!strstr(ds->url_input.buffer, "://")) {
      connstr = connstr_from_path(ds->url_input.buffer, &err);
      if (!connstr) {
        ds->error_msg = err ? err : str_dup("Invalid file path");
        return NULL;
      }
    } else {
      connstr = str_dup(ds->url_input.buffer);
    }
  }

  /* Test connection */
  DbConnection *conn = db_connect(connstr, &err);
  if (!conn) {
    /* Connection failed - only prompt for password if it's an auth error */
    if (saved_conn && saved_conn->driver &&
        strcmp(saved_conn->driver, "sqlite") != 0 &&
        is_auth_error(err)) {
      /* Prompt for password */
      char *password = show_password_dialog(ds->dialog_win, "Password Required",
                                            "Enter password:");
      if (password) {
        /* Rebuild connection string with password */
        free(connstr);
        free(err);
        err = NULL;

        connstr = connstr_build(
            saved_conn->driver,
            (saved_conn->user && saved_conn->user[0]) ? saved_conn->user : NULL,
            password,
            (saved_conn->host && saved_conn->host[0]) ? saved_conn->host : NULL,
            saved_conn->port,
            (saved_conn->database && saved_conn->database[0]) ? saved_conn->database : NULL,
            NULL, NULL, 0);

        str_secure_free(password);

        if (connstr) {
          /* Retry connection */
          conn = db_connect(connstr, &err);
        }
      }
    }

    if (!conn) {
      ds->error_msg = err ? err : str_dup("Connection failed");
      free(connstr);
      return NULL;
    }
  }

  db_disconnect(conn);

  if (test_only) {
    ds->success_msg = str_dup("Connection successful!");
    free(connstr);
    return NULL;
  }

  /* Determine mode */
  if (mode) {
    *mode = ds->has_existing_tabs ? CONNECT_MODE_NEW_TAB : CONNECT_MODE_NEW_WORKSPACE;
  }

  return connstr;
}

/* ============================================================================
 * Input Handling
 * ============================================================================
 */

static bool handle_tree_input(DialogState *ds, const UiEvent *event) {
  size_t visible_count = connmgr_count_visible(ds->mgr);
  int key_char = render_event_get_char(event);

  /* In move mode, hierarchical navigation through folders */
  if (ds->move.active) {
    ConnectionItem *target = ds->move.target_folder;
    size_t pos = ds->move.insert_pos;
    size_t num_children = count_children_excluding_source(target, ds->move.source);

    if (render_event_is_special(event, UI_KEY_UP) || key_char == 'k') {
      if (pos > 0) {
        /* Move up within folder */
        ds->move.insert_pos--;
        /* Check if we're now after an expanded folder - enter it at end */
        ConnectionItem *prev = get_child_excluding_source(target, ds->move.insert_pos, ds->move.source);
        if (prev && connmgr_is_folder(prev) && prev->folder.expanded) {
          ds->move.target_folder = prev;
          ds->move.insert_pos = count_children_excluding_source(prev, ds->move.source);
        }
      } else {
        /* At start of folder - exit to parent (before this folder) */
        if (target != &ds->mgr->root && target->parent) {
          size_t folder_idx = find_index_in_parent(target, ds->move.source);
          ds->move.target_folder = target->parent;
          ds->move.insert_pos = folder_idx;
        }
      }
      return true;
    }

    if (render_event_is_special(event, UI_KEY_DOWN) || key_char == 'j') {
      if (pos < num_children) {
        /* Check if item at current position is an expanded folder - enter it */
        ConnectionItem *curr = get_child_excluding_source(target, pos, ds->move.source);
        if (curr && connmgr_is_folder(curr) && curr->folder.expanded) {
          ds->move.target_folder = curr;
          ds->move.insert_pos = 0;
        } else {
          /* Move down within folder */
          ds->move.insert_pos++;
        }
      } else {
        /* At end of folder - exit to parent (after this folder) */
        if (target != &ds->mgr->root && target->parent) {
          size_t folder_idx = find_index_in_parent(target, ds->move.source);
          ds->move.target_folder = target->parent;
          ds->move.insert_pos = folder_idx + 1;
        }
      }
      return true;
    }
  } else {
    /* Normal navigation */
    if (render_event_is_special(event, UI_KEY_UP) || key_char == 'k') {
      if (ds->tree_highlight > 0) {
        ds->tree_highlight--;
      }
      return true;
    }

    if (render_event_is_special(event, UI_KEY_DOWN) || key_char == 'j') {
      if (visible_count > 0 && ds->tree_highlight < visible_count - 1) {
        ds->tree_highlight++;
      } else if (visible_count == 0 || ds->tree_highlight >= visible_count - 1) {
        /* At bottom of tree, move to buttons panel */
        ds->prev_panel_focus = FOCUS_TREE;
        ds->focus = FOCUS_BUTTONS;
      }
      return true;
    }
  }

  /* Spatial navigation: 'l' moves focus to URL panel */
  if (key_char == 'l') {
    ds->focus = FOCUS_URL;
    return true;
  }

  /* Space: select item for move or drop item at current location */
  if (key_char == ' ') {
    if (ds->move.active) {
      /* Drop the item at target position */
      ConnectionItem *target = ds->move.target_folder;
      ConnectionItem *insert_after = get_insert_after_for_pos(ds);
      ConnectionItem *source = ds->move.source;

      /* Get the name to find item after move (pointers may become invalid) */
      char *source_name = source ? str_dup(connmgr_item_name(source)) : NULL;
      bool is_folder = source && connmgr_is_folder(source);

      bool moved = false;
      if (target && source && target != source) {
        /* Check if trying to move into self or descendant */
        bool valid = true;
        if (connmgr_is_folder(source)) {
          ConnectionItem *p = target;
          while (p) {
            if (p == source) {
              valid = false;
              break;
            }
            p = p->parent;
          }
        }

        if (valid && connmgr_move_item(ds->mgr, source, target, insert_after)) {
          ds->success_msg = str_dup("Item moved!");
          if (target != &ds->mgr->root && target->type == CONN_ITEM_FOLDER) {
            target->folder.expanded = true;
          }
          moved = true;
        } else {
          ds->error_msg = str_dup("Cannot move here");
        }
      }

      /* Clear move state */
      clear_move_state(&ds->move);

      /* Find and highlight the moved item by name */
      if (moved && source_name) {
        size_t visible = connmgr_count_visible(ds->mgr);
        for (size_t i = 0; i < visible; i++) {
          ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
          if (item && connmgr_is_folder(item) == is_folder) {
            const char *name = connmgr_item_name(item);
            if (name && strcmp(name, source_name) == 0) {
              ds->tree_highlight = i;
              break;
            }
          }
        }
      }
      free(source_name);

      /* Ensure highlight is valid */
      size_t visible = connmgr_count_visible(ds->mgr);
      if (ds->tree_highlight >= visible && visible > 0) {
        ds->tree_highlight = visible - 1;
      }
    } else {
      /* Start move - initialize target_folder and insert_pos from source position */
      ConnectionItem *current = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
      if (current && current->parent && current->parent->type == CONN_ITEM_FOLDER) {
        ds->move.active = true;
        ds->move.source = current;
        ds->move.source_idx = ds->tree_highlight;
        ds->move.target_folder = current->parent;
        /* Find actual index of source in parent - this becomes the insert position
         * because after removing source, insert_pos=K means "after K items" */
        ConnectionFolder *pf = &current->parent->folder;
        size_t src_idx = 0;
        for (size_t i = 0; i < pf->num_children; i++) {
          if (&pf->children[i] == current) {
            src_idx = i;
            break;
          }
        }
        ds->move.insert_pos = src_idx;
        ds->success_msg = str_dup("Move with arrows, Space to drop");
      }
    }
    return true;
  }

  /* Escape cancels move mode */
  if (render_event_is_special(event, UI_KEY_ESCAPE) && ds->move.active) {
    clear_move_state(&ds->move);
    ds->success_msg = str_dup("Move cancelled");
    return true;
  }

  /* Right arrow: expand folder or move to URL panel */
  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item && connmgr_is_folder(item) && !item->folder.expanded) {
      connmgr_toggle_folder(item);
      ds->mgr->modified = true;
    } else if (!ds->move.active) {
      /* Move to URL panel only if not in move mode */
      ds->focus = FOCUS_URL;
    }
    return true;
  }

  if (render_event_is_special(event, UI_KEY_LEFT)) {
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item) {
      if (connmgr_is_folder(item) && item->folder.expanded) {
        /* Collapse folder */
        connmgr_toggle_folder(item);
        ds->mgr->modified = true;
      } else if (item->parent && item->parent->parent) {
        /* Move to parent folder */
        for (size_t i = 0; i < connmgr_count_visible(ds->mgr); i++) {
          if (connmgr_get_visible_item(ds->mgr, i) == item->parent) {
            ds->tree_highlight = i;
            break;
          }
        }
      }
    }
    return true;
  }

  /* New folder - adds to selected folder or parent of selected item */
  if (ds->config && hotkey_matches(ds->config, event, HOTKEY_CONN_NEW_FOLDER)) {
    char *name = show_input_dialog(ds->dialog_win, "New Folder", "Name:", "");
    if (name) {
      ConnectionFolder *folder = connmgr_new_folder(name);
      if (folder) {
        /* Determine parent: selected folder, or parent of selected connection */
        ConnectionItem *selected = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
        ConnectionItem *parent = &ds->mgr->root;

        if (selected) {
          if (connmgr_is_folder(selected)) {
            parent = selected;
            /* Expand parent so new folder is visible */
            selected->folder.expanded = true;
          } else if (selected->parent) {
            parent = selected->parent;
          }
        }

        if (connmgr_add_folder(parent, folder)) {
          ds->mgr->modified = true;
          ds->success_msg = str_dup("Folder created!");
        } else {
          ds->error_msg = str_dup("Failed to create folder");
          free(folder->name);
          free(folder);
        }
      }
      free(name);
    }
    return true;
  }

  /* New connection - adds to selected folder or parent of selected item */
  if (ds->config && hotkey_matches(ds->config, event, HOTKEY_CONN_NEW)) {
    ConnectionItem *selected = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    ConnectionItem *parent = &ds->mgr->root;

    if (selected) {
      if (connmgr_is_folder(selected)) {
        parent = selected;
        selected->folder.expanded = true;
      } else if (selected->parent) {
        parent = selected->parent;
      }
    }

    char *err = NULL;
    if (show_connection_form(ds->dialog_win, ds->mgr, NULL, parent, &err)) {
      ds->success_msg = str_dup("Connection created!");
    } else if (err) {
      ds->error_msg = err;
    }
    return true;
  }

  /* Edit connection */
  if (ds->config && hotkey_matches(ds->config, event, HOTKEY_CONN_EDIT)) {
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item && connmgr_is_connection(item)) {
      char *err = NULL;
      if (show_connection_form(ds->dialog_win, ds->mgr, item, NULL, &err)) {
        ds->success_msg = str_dup("Connection updated!");
      } else if (err) {
        ds->error_msg = err;
      }
    } else if (item && connmgr_is_folder(item)) {
      /* For folders, just rename */
      const char *current_name = connmgr_item_name(item);
      char *new_name = show_input_dialog(ds->dialog_win, "Rename Folder", "Name:", current_name);
      if (new_name) {
        free(item->folder.name);
        item->folder.name = new_name;
        ds->mgr->modified = true;
      }
    }
    return true;
  }

  /* Rename */
  if (ds->config && hotkey_matches(ds->config, event, HOTKEY_CONN_RENAME)) {
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item) {
      const char *current_name = connmgr_item_name(item);
      char *new_name = show_input_dialog(ds->dialog_win, "Rename", "Name:", current_name);
      if (new_name) {
        if (connmgr_is_folder(item)) {
          free(item->folder.name);
          item->folder.name = new_name;
        } else {
          free(item->connection.name);
          item->connection.name = new_name;
        }
        ds->mgr->modified = true;
      }
    }
    return true;
  }

  /* Delete */
  if (ds->config && hotkey_matches(ds->config, event, HOTKEY_CONN_DELETE)) {
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item && item->parent) {
      const char *name = connmgr_item_name(item);
      char msg[128];
      snprintf(msg, sizeof(msg), "Delete '%s'?", name ? name : "item");

      if (show_confirm_dialog(ds->dialog_win, "Confirm Delete", msg)) {
        connmgr_remove_item(ds->mgr, item);
        if (ds->tree_highlight > 0) {
          ds->tree_highlight--;
        }
      }
    }
    return true;
  }

  return false;
}

static bool handle_button_input(DialogState *ds, const UiEvent *event,
                                bool *should_exit, ConnectResult *result) {
  int key_char = render_event_get_char(event);

  if (render_event_is_special(event, UI_KEY_LEFT) || key_char == 'h') {
    if (ds->selected_button > 0) {
      ds->selected_button--;
    }
    return true;
  }

  if (render_event_is_special(event, UI_KEY_RIGHT) || key_char == 'l') {
    if (ds->selected_button < BTN_COUNT - 1) {
      ds->selected_button++;
    }
    return true;
  }

  if (render_event_is_special(event, UI_KEY_ENTER)) {
    free(ds->error_msg);
    ds->error_msg = NULL;
    free(ds->success_msg);
    ds->success_msg = NULL;

    switch (ds->selected_button) {
    case BTN_CONNECT: {
      ConnectMode mode = CONNECT_MODE_NEW_TAB;
      char *connstr = try_connect(ds, false, &mode);
      if (connstr) {
        result->connstr = connstr;
        result->mode = CONNECT_MODE_NEW_TAB;
        *should_exit = false;  /* false = stop running */
      }
      break;
    }

    case BTN_NEW_WS: {
      ConnectMode mode = CONNECT_MODE_NEW_WORKSPACE;
      char *connstr = try_connect(ds, false, &mode);
      if (connstr) {
        result->connstr = connstr;
        result->mode = CONNECT_MODE_NEW_WORKSPACE;
        *should_exit = false;  /* false = stop running */
      }
      break;
    }

    case BTN_TEST:
      try_connect(ds, true, NULL);
      break;

    case BTN_SAVE: {
      /* Save URL to list */
      if (ds->url_input.len == 0) {
        ds->error_msg = str_dup("Enter a URL to save");
        break;
      }

      char *url_to_save = NULL;
      char *err = NULL;

      /* Convert file path to URL if needed */
      if (!strstr(ds->url_input.buffer, "://")) {
        url_to_save = connstr_from_path(ds->url_input.buffer, &err);
        if (!url_to_save) {
          ds->error_msg = err ? err : str_dup("Invalid file path");
          break;
        }
      } else {
        url_to_save = str_dup(ds->url_input.buffer);
      }

      char *save_err = NULL;
      if (show_save_dialog(ds->dialog_win, ds->mgr, url_to_save, &save_err)) {
        ds->success_msg = str_dup("Connection saved!");
      } else if (save_err) {
        ds->error_msg = save_err;
      }

      free(url_to_save);
      break;
    }

    case BTN_DELETE: {
      ConnectionItem *item = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
      if (item && item->parent) {
        const char *name = connmgr_item_name(item);
        char msg[128];
        snprintf(msg, sizeof(msg), "Delete '%s'?", name ? name : "item");

        if (show_confirm_dialog(ds->dialog_win, "Confirm Delete", msg)) {
          connmgr_remove_item(ds->mgr, item);
          if (ds->tree_highlight > 0) {
            ds->tree_highlight--;
          }
        }
      }
      break;
    }

    case BTN_CLOSE:
      *should_exit = false;  /* false = stop running */
      break;

    case BTN_QUIT:
      result->mode = CONNECT_MODE_QUIT;
      *should_exit = false;  /* false = stop running */
      break;
    }
    return true;
  }

  return false;
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================
 */

ConnectResult connect_view_show(TuiState *state) {
  ConnectResult result = {NULL, NULL, CONNECT_MODE_CANCELLED};

  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

  /* Load connection manager */
  char *load_err = NULL;
  ConnectionManager *mgr = connmgr_load(&load_err);
  if (!mgr) {
    /* Create empty one */
    mgr = connmgr_new();
  }
  free(load_err);

  /* Check existing tabs */
  bool has_existing = false;
  if (state && state->app) {
    Workspace *ws = app_current_workspace(state->app);
    if (ws && ws->num_tabs > 0) {
      has_existing = true;
    }
  }

  /* Calculate dialog size */
  int width = term_cols - 10;
  if (width < MIN_DIALOG_WIDTH) width = MIN_DIALOG_WIDTH;
  if (width > 100) width = 100;

  int height = term_rows - 6;
  if (height < MIN_DIALOG_HEIGHT) height = MIN_DIALOG_HEIGHT;
  if (height > 30) height = 30;

  int starty = (term_rows - height) / 2;
  int startx = (term_cols - width) / 2;
  if (starty < 0) starty = 0;
  if (startx < 0) startx = 0;

  WINDOW *dialog = newwin(height, width, starty, startx);
  if (!dialog) {
    connmgr_free(mgr);
    return result;
  }

  keypad(dialog, TRUE);

  /* Initialize dialog state */
  DialogState ds = {0};
  ds.mgr = mgr;
  ds.config = state && state->app ? state->app->config : NULL;
  ds.focus = connmgr_count_visible(mgr) > 0 ? FOCUS_TREE : FOCUS_URL;
  ds.prev_panel_focus = ds.focus;  /* Initialize to same as initial focus */
  ds.tree_highlight = 0;
  ds.tree_scroll = 0;
  ds.selected_button = BTN_CONNECT;
  ds.has_existing_tabs = has_existing;
  ds.height = height;
  ds.width = width;
  ds.tree_height = height - 6;
  ds.dialog_win = dialog;

  int url_input_width = width - TREE_PANEL_WIDTH - 8;
  input_init(&ds.url_input, url_input_width);

  bool running = true;
  while (running) {
    curs_set(ds.focus == FOCUS_URL ? 1 : 0);

    int cursor_y = 0, cursor_x = 0;
    draw_dialog(dialog, &ds, &cursor_y, &cursor_x);

    int ch = wgetch(dialog);
    UiEvent event;
    render_translate_key(ch, &event);

    /* Clear messages on any key */
    free(ds.error_msg);
    ds.error_msg = NULL;
    free(ds.success_msg);
    ds.success_msg = NULL;

    /* Tab cycles focus */
    if (render_event_is_special(&event, UI_KEY_TAB)) {
      if (ds.focus == FOCUS_TREE) {
        ds.prev_panel_focus = FOCUS_TREE;
        ds.focus = FOCUS_URL;
      } else if (ds.focus == FOCUS_URL) {
        ds.prev_panel_focus = FOCUS_URL;
        ds.focus = FOCUS_BUTTONS;
      } else {
        ds.focus = FOCUS_TREE;
      }
      continue;
    }

    /* Escape closes */
    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
      continue;
    }

    /* 'q' quits the app */
    if (render_event_get_char(&event) == 'q' && ds.focus != FOCUS_URL) {
      result.mode = CONNECT_MODE_QUIT;
      running = false;
      continue;
    }

    /* Test connection hotkey */
    if (ds.config && hotkey_matches(ds.config, &event, HOTKEY_CONN_TEST)) {
      try_connect(&ds, true, NULL);
      continue;
    }

    /* Save URL to list hotkey */
    if (ds.config && hotkey_matches(ds.config, &event, HOTKEY_CONN_SAVE) && ds.url_input.len > 0) {
      char *url_to_save = NULL;
      char *err = NULL;

      if (!strstr(ds.url_input.buffer, "://")) {
        url_to_save = connstr_from_path(ds.url_input.buffer, &err);
        if (!url_to_save) {
          ds.error_msg = err ? err : str_dup("Invalid file path");
          continue;
        }
      } else {
        url_to_save = str_dup(ds.url_input.buffer);
      }

      char *save_err = NULL;
      if (show_save_dialog(dialog, mgr, url_to_save, &save_err)) {
        ds.success_msg = str_dup("Connection saved!");
      } else if (save_err) {
        ds.error_msg = save_err;
      }

      free(url_to_save);
      continue;
    }

    /* Global shortcuts (work regardless of focus) */

    /* New folder hotkey (global) */
    if (ds.config && hotkey_matches(ds.config, &event, HOTKEY_CONN_NEW_FOLDER) &&
        !ds.move.active && ds.focus != FOCUS_URL) {
      char *name = show_input_dialog(dialog, "New Folder", "Name:", "");
      if (name) {
        ConnectionFolder *folder = connmgr_new_folder(name);
        if (folder) {
          /* Add to root by default from global shortcut */
          if (connmgr_add_folder(&mgr->root, folder)) {
            mgr->modified = true;
            ds.success_msg = str_dup("Folder created!");
          } else {
            ds.error_msg = str_dup("Failed to create folder");
            free(folder->name);
            free(folder);
          }
        }
        free(name);
      }
      continue;
    }

    /* New connection hotkey (global) */
    if (ds.config && hotkey_matches(ds.config, &event, HOTKEY_CONN_NEW) &&
        !ds.move.active && ds.focus != FOCUS_URL) {
      char *err = NULL;
      if (show_connection_form(dialog, mgr, NULL, &mgr->root, &err)) {
        ds.success_msg = str_dup("Connection created!");
      } else if (err) {
        ds.error_msg = err;
      }
      continue;
    }

    /* Handle focus-specific input */
    switch (ds.focus) {
    case FOCUS_TREE:
      if (render_event_is_special(&event, UI_KEY_ENTER)) {
        ConnectionItem *item = connmgr_get_visible_item(mgr, ds.tree_highlight);
        if (item) {
          if (connmgr_is_folder(item)) {
            connmgr_toggle_folder(item);
            mgr->modified = true;
          } else {
            /* Connect to selected item */
            ConnectMode mode;
            char *connstr = try_connect(&ds, false, &mode);
            if (connstr) {
              result.connstr = connstr;
              result.saved_conn_id = str_dup(item->connection.id);
              result.mode = mode;
              running = false;
            }
          }
        }
      } else {
        handle_tree_input(&ds, &event);
      }
      break;

    case FOCUS_URL:
      if (render_event_is_special(&event, UI_KEY_ENTER)) {
        if (ds.url_input.len > 0) {
          ConnectMode mode;
          char *connstr = try_connect(&ds, false, &mode);
          if (connstr) {
            result.connstr = connstr;
            result.mode = mode;
            running = false;
          }
        }
      } else if (render_event_is_special(&event, UI_KEY_DOWN)) {
        ds.prev_panel_focus = FOCUS_URL;
        ds.focus = FOCUS_BUTTONS;
      } else if (render_event_is_special(&event, UI_KEY_LEFT) &&
                 ds.url_input.cursor == 0) {
        /* Left arrow at start of input moves to Tree panel */
        ds.focus = FOCUS_TREE;
      } else {
        input_handle_key(&ds.url_input, &event);
      }
      break;

    case FOCUS_BUTTONS:
      if (render_event_is_special(&event, UI_KEY_UP) ||
          render_event_get_char(&event) == 'k') {
        /* Return to previous panel (URL or Tree) */
        ds.focus = ds.prev_panel_focus;
      } else if (render_event_get_char(&event) == 'h') {
        /* 'h' moves left to Tree panel */
        ds.focus = FOCUS_TREE;
      } else if (render_event_get_char(&event) == 'l') {
        /* 'l' moves right to URL panel */
        ds.focus = FOCUS_URL;
      } else {
        handle_button_input(&ds, &event, &running, &result);
      }
      break;
    }
  }

  /* Save if modified */
  if (mgr->modified) {
    char *save_err = NULL;
    connmgr_save(mgr, &save_err);
    free(save_err);
  }

  curs_set(0);
  delwin(dialog);
  connmgr_free(mgr);
  free(ds.error_msg);
  free(ds.success_msg);

  /* Redraw main screen */
  touchwin(stdscr);
  if (state) {
    tui_refresh(state);
  }

  return result;
}

char *connect_view_recent(TuiState *state) {
  (void)state;
  return NULL;
}
