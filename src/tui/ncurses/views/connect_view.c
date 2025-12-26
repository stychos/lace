/*
 * Lace
 * Connection Manager dialog - Combined saved connections tree + quick connect
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connect_view.h"
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

/* Dialog state */
typedef struct {
  ConnectionManager *mgr;
  InputField url_input;
  DialogFocus focus;
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
                           ConnectionItem *item, bool selected, bool focused) {
  int depth = connmgr_get_item_depth(item);
  int indent = depth * 2;

  /* Clear line */
  mvwhline(win, y, x, ' ', width);

  /* Highlight if selected */
  if (selected) {
    if (focused) {
      wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    } else {
      wattron(win, A_REVERSE);
    }
  }

  mvwhline(win, y, x, ' ', width);

  /* Draw indent */
  wmove(win, y, x + indent);

  /* Draw expand/collapse indicator or connection icon */
  if (connmgr_is_folder(item)) {
    if (item->folder.expanded) {
      waddch(win, 'v');
    } else {
      waddch(win, '>');
    }
    waddch(win, ' ');
  } else {
    waddstr(win, "  ");
  }

  /* Draw name (truncate if needed) */
  const char *name = connmgr_item_name(item);
  int name_width = width - indent - 3;
  if (name_width > 0) {
    if ((int)strlen(name) > name_width) {
      waddnstr(win, name, name_width - 1);
      waddch(win, '~');
    } else {
      waddstr(win, name);
    }
  }

  if (selected) {
    if (focused) {
      wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    } else {
      wattroff(win, A_REVERSE);
    }
  }
}

static void draw_tree_panel(WINDOW *win, DialogState *ds, int start_y, int start_x,
                            int height, int width) {
  /* Panel header */
  wattron(win, A_BOLD);
  mvwprintw(win, start_y, start_x, "Saved Connections");
  wattroff(win, A_BOLD);

  /* Draw border around tree area */
  int tree_y = start_y + 1;
  int tree_height = height - 2;

  /* Draw tree contents */
  size_t visible_count = connmgr_count_visible(ds->mgr);

  if (visible_count == 0) {
    wattron(win, A_DIM);
    mvwprintw(win, tree_y + 1, start_x + 1, "(no saved connections)");
    wattroff(win, A_DIM);
    return;
  }

  /* Adjust scroll to keep highlight visible */
  if (ds->tree_highlight >= ds->tree_scroll + (size_t)tree_height) {
    ds->tree_scroll = ds->tree_highlight - (size_t)tree_height + 1;
  }
  if (ds->tree_highlight < ds->tree_scroll) {
    ds->tree_scroll = ds->tree_highlight;
  }

  /* Draw visible items */
  for (int i = 0; i < tree_height && ds->tree_scroll + (size_t)i < visible_count; i++) {
    size_t idx = ds->tree_scroll + (size_t)i;
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, idx);
    if (item) {
      bool selected = (idx == ds->tree_highlight);
      draw_tree_item(win, tree_y + i, start_x, width, item, selected,
                     ds->focus == FOCUS_TREE);
    }
  }
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
  if (ds->error_msg && ds->error_msg[0]) {
    wattron(win, COLOR_PAIR(COLOR_ERROR));
    mvwprintw(win, msg_y, 2, "%.60s", ds->error_msg);
    wattroff(win, COLOR_PAIR(COLOR_ERROR));
  } else if (ds->success_msg && ds->success_msg[0]) {
    wattron(win, COLOR_PAIR(COLOR_NUMBER));
    mvwprintw(win, msg_y, 2, "%.60s", ds->success_msg);
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

    wattron(dlg, COLOR_PAIR(COLOR_SELECTED));
    mvwhline(dlg, 3, 2, ' ', dlg_width - 4);
    mvwaddnstr(dlg, 3, 2, buf, dlg_width - 5);
    wattroff(dlg, COLOR_PAIR(COLOR_SELECTED));

    wattron(dlg, A_DIM);
    mvwaddch(dlg, 4, 0, ACS_LTEE);
    mvwhline(dlg, 4, 1, ACS_HLINE, dlg_width - 2);
    mvwaddch(dlg, 4, dlg_width - 1, ACS_RTEE);
    wattroff(dlg, A_DIM);

    mvwprintw(dlg, 6, dlg_width / 2 - 10, "[ OK ]  [ Cancel ]");

    wmove(dlg, 3, 2 + (int)cursor);
    curs_set(1);
    wrefresh(dlg);

    int ch = wgetch(dlg);
    UiEvent event;
    render_translate_key(ch, &event);

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (len > 0) {
        result = str_dup(buf);
      }
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_BACKSPACE)) {
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
  }

  curs_set(0);
  delwin(dlg);
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

  int parent_h, parent_w;
  getmaxyx(parent, parent_h, parent_w);
  (void)parent_h;

  /* Small dialog to get connection name */
  int dlg_height = 8;
  int dlg_width = 50;
  if (dlg_width > parent_w - 10) dlg_width = parent_w - 10;

  int dlg_y = 5;
  int dlg_x = (parent_w - dlg_width) / 2;

  WINDOW *dlg = derwin(parent, dlg_height, dlg_width, dlg_y, dlg_x);
  if (!dlg) {
    connmgr_free_connection(conn);
    free(conn);
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

  bool result = false;
  bool running = true;

  while (running) {
    werase(dlg);
    box(dlg, 0, 0);

    wattron(dlg, A_BOLD);
    mvwprintw(dlg, 0, (dlg_width - 18) / 2, " Save Connection ");
    wattroff(dlg, A_BOLD);

    mvwprintw(dlg, 2, 2, "Name:");

    /* Draw name input */
    wattron(dlg, COLOR_PAIR(COLOR_SELECTED));
    mvwhline(dlg, 3, 2, ' ', dlg_width - 4);
    mvwaddnstr(dlg, 3, 2, name_buf, dlg_width - 5);
    wattroff(dlg, COLOR_PAIR(COLOR_SELECTED));

    /* Separator with T-junctions */
    wattron(dlg, A_DIM);
    mvwaddch(dlg, 4, 0, ACS_LTEE);
    mvwhline(dlg, 4, 1, ACS_HLINE, dlg_width - 2);
    mvwaddch(dlg, 4, dlg_width - 1, ACS_RTEE);
    wattroff(dlg, A_DIM);

    /* Buttons */
    mvwprintw(dlg, 6, dlg_width / 2 - 12, "[ Save ]  [ Cancel ]");

    /* Position cursor */
    wmove(dlg, 3, 2 + (int)name_cursor);
    curs_set(1);

    wrefresh(dlg);

    int ch = wgetch(dlg);
    UiEvent event;
    render_translate_key(ch, &event);

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (name_len > 0) {
        /* Save the connection */
        free(conn->name);
        conn->name = str_dup(name_buf);

        if (connmgr_add_connection(&mgr->root, conn)) {
          mgr->modified = true;
          result = true;
        } else {
          *error = str_dup("Failed to add connection");
          connmgr_free_connection(conn);
          free(conn);
        }
        running = false;
      }
    } else if (render_event_is_special(&event, UI_KEY_BACKSPACE)) {
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
  }

  curs_set(0);
  delwin(dlg);

  if (!result && !*error) {
    connmgr_free_connection(conn);
    free(conn);
  }

  return result;
}

/* ============================================================================
 * Actions
 * ============================================================================
 */

static char *try_connect(DialogState *ds, bool test_only, ConnectMode *mode) {
  char *connstr = NULL;
  char *err = NULL;

  /* Determine source: tree selection or URL */
  if (ds->focus == FOCUS_TREE || (ds->focus == FOCUS_BUTTONS && ds->url_input.len == 0)) {
    /* Use selected saved connection */
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (!item || !connmgr_is_connection(item)) {
      ds->error_msg = str_dup("Select a connection first");
      return NULL;
    }
    connstr = connmgr_build_connstr(&item->connection);
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
    ds->error_msg = err ? err : str_dup("Connection failed");
    free(connstr);
    return NULL;
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

  /* Navigation */
  if (render_event_is_special(event, UI_KEY_UP) || key_char == 'k') {
    if (ds->tree_highlight > 0) {
      ds->tree_highlight--;
    }
    return true;
  }

  if (render_event_is_special(event, UI_KEY_DOWN) || key_char == 'j') {
    if (visible_count > 0 && ds->tree_highlight < visible_count - 1) {
      ds->tree_highlight++;
    } else if (visible_count == 0 ||
               ds->tree_highlight >= visible_count - 1) {
      /* At bottom of tree, move to buttons panel */
      ds->focus = FOCUS_BUTTONS;
    }
    return true;
  }

  /* Spatial navigation: 'l' moves focus to URL panel */
  if (key_char == 'l') {
    ds->focus = FOCUS_URL;
    return true;
  }

  /* Toggle folder expand/collapse */
  if (key_char == ' ' || render_event_is_special(event, UI_KEY_RIGHT)) {
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item && connmgr_is_folder(item)) {
      connmgr_toggle_folder(item);
      ds->mgr->modified = true;
    } else if (render_event_is_special(event, UI_KEY_RIGHT)) {
      /* Right arrow on connection moves to URL panel */
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

  /* New folder (N) */
  if (key_char == 'N') {
    char *name = show_input_dialog(ds->dialog_win, "New Folder", "Name:", "");
    if (name) {
      ConnectionFolder *folder = connmgr_new_folder(name);
      if (folder) {
        /* Add to root for simplicity - could add to selected folder */
        if (connmgr_add_folder(&ds->mgr->root, folder)) {
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

  /* Rename (r) */
  if (key_char == 'r') {
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

  /* Delete (d or Delete key) */
  if (key_char == 'd' || render_event_is_special(event, UI_KEY_DELETE)) {
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item && item->parent) {
      connmgr_remove_item(ds->mgr, item);
      if (ds->tree_highlight > 0) {
        ds->tree_highlight--;
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
        connmgr_remove_item(ds->mgr, item);
        if (ds->tree_highlight > 0) {
          ds->tree_highlight--;
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
  ConnectResult result = {NULL, CONNECT_MODE_CANCELLED};

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
  ds.focus = connmgr_count_visible(mgr) > 0 ? FOCUS_TREE : FOCUS_URL;
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
        ds.focus = FOCUS_URL;
      } else if (ds.focus == FOCUS_URL) {
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

    /* Ctrl+T = test */
    if (render_event_is_ctrl(&event, 'T')) {
      try_connect(&ds, true, NULL);
      continue;
    }

    /* Ctrl+S = save URL to list */
    if (render_event_is_ctrl(&event, 'S') && ds.url_input.len > 0) {
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
      if (render_event_is_special(&event, UI_KEY_UP)) {
        ds.focus = FOCUS_URL;
      } else if (render_event_get_char(&event) == 'k') {
        /* 'k' moves up to URL panel */
        ds.focus = FOCUS_URL;
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
