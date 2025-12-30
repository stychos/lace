/*
 * Lace
 * Configuration Editor dialog
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config_view.h"
#include "../../../config/config.h"
#include "../../../core/history.h"
#include "../../../util/str.h"
#include "../render_helpers.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MIN_DIALOG_WIDTH 60
#define MIN_DIALOG_HEIGHT 20
#define MAX_DIALOG_WIDTH 80
#define MAX_DIALOG_HEIGHT 35

/* Dialog tabs */
typedef enum { TAB_GENERAL, TAB_HOTKEYS, TAB_COUNT } ConfigTab;

/* Focus areas in General tab */
typedef enum {
  FOCUS_TABS,
  FOCUS_SETTINGS,
  FOCUS_BUTTONS
} DialogFocus;

/* General settings field indices */
typedef enum {
  FIELD_SHOW_HEADER,
  FIELD_SHOW_STATUS,
  FIELD_PAGE_SIZE,
  FIELD_PREFETCH_PAGES,
  FIELD_MAX_RESULT_ROWS,
  FIELD_DELETE_CONFIRM,
  FIELD_HISTORY_MODE,
  FIELD_HISTORY_MAX_SIZE,
  FIELD_AUTO_OPEN_TABLE,
  FIELD_CLOSE_CONN_LAST_TAB,
  FIELD_RESTORE_SESSION,
  FIELD_QUIT_CONFIRM,
  FIELD_COUNT
} GeneralField;

/* Button IDs */
typedef enum { BTN_SAVE, BTN_CANCEL, BTN_RESET, BTN_COUNT } ButtonId;

/* Number input field */
typedef struct {
  char buffer[16];
  size_t len;
  size_t cursor;
  int min_val;
  int max_val;
} NumberInput;

/* Dialog state */
typedef struct {
  Config *config;       /* Working copy of config */
  Config *original;     /* Original config for comparison */
  ConfigTab current_tab;
  DialogFocus focus;
  int selected_field;
  int selected_button;
  bool editing_number;
  NumberInput num_input;
  char *error_msg;
  char *success_msg;
  int height;
  int width;
  WINDOW *dialog_win;

  /* Hotkeys tab state */
  size_t hotkey_scroll;
  size_t hotkey_highlight;
  bool hotkey_editing;
} DialogState;

/* ============================================================================
 * Number Input Field
 * ============================================================================
 */

static void number_input_init(NumberInput *input, int value, int min_val,
                              int max_val) {
  memset(input, 0, sizeof(NumberInput));
  input->min_val = min_val;
  input->max_val = max_val;
  snprintf(input->buffer, sizeof(input->buffer), "%d", value);
  input->len = strlen(input->buffer);
  input->cursor = input->len;
}

static int number_input_value(NumberInput *input) {
  int val = atoi(input->buffer);
  if (val < input->min_val)
    val = input->min_val;
  if (val > input->max_val)
    val = input->max_val;
  return val;
}

static void number_input_handle_key(NumberInput *input, const UiEvent *event) {
  if (!event || event->type != UI_EVENT_KEY)
    return;

  int key_char = render_event_get_char(event);

  if (render_event_is_special(event, UI_KEY_LEFT)) {
    if (input->cursor > 0)
      input->cursor--;
    return;
  }

  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    if (input->cursor < input->len)
      input->cursor++;
    return;
  }

  if (render_event_is_special(event, UI_KEY_HOME)) {
    input->cursor = 0;
    return;
  }

  if (render_event_is_special(event, UI_KEY_END)) {
    input->cursor = input->len;
    return;
  }

  if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
    if (input->cursor > 0 && input->cursor <= input->len) {
      memmove(input->buffer + input->cursor - 1, input->buffer + input->cursor,
              input->len - input->cursor + 1);
      input->cursor--;
      input->len--;
    }
    return;
  }

  if (render_event_is_special(event, UI_KEY_DELETE)) {
    if (input->cursor < input->len) {
      memmove(input->buffer + input->cursor, input->buffer + input->cursor + 1,
              input->len - input->cursor);
      input->len--;
    }
    return;
  }

  /* Only allow digits */
  if (render_event_is_char(event) && key_char >= '0' && key_char <= '9' &&
      input->len < sizeof(input->buffer) - 1) {
    memmove(input->buffer + input->cursor + 1, input->buffer + input->cursor,
            input->len - input->cursor + 1);
    input->buffer[input->cursor] = (char)key_char;
    input->cursor++;
    input->len++;
  }
}

/* ============================================================================
 * Field Drawing Helpers
 * ============================================================================
 */

static void draw_checkbox(WINDOW *win, int y, int x, const char *label,
                          bool checked, bool selected, bool focused) {
  if (selected && focused) {
    wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
  }

  mvwprintw(win, y, x, "[%c] %s", checked ? 'X' : ' ', label);

  if (selected && focused) {
    wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
  }
}

static void draw_number_field(WINDOW *win, int y, int x, const char *label,
                              int value, bool selected, bool focused,
                              bool editing, NumberInput *input, int *cursor_x) {
  mvwprintw(win, y, x, "%s: ", label);
  int val_x = x + (int)strlen(label) + 2;

  if (selected && focused) {
    wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
  }

  if (editing && selected) {
    mvwprintw(win, y, val_x, "%-8s", input->buffer);
    if (cursor_x)
      *cursor_x = val_x + (int)input->cursor;
  } else {
    mvwprintw(win, y, val_x, "%-8d", value);
  }

  if (selected && focused) {
    wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
  }
}

/* Draw an option selector (cycles through options) */
static void draw_option(WINDOW *win, int y, int x, const char *label,
                        const char *value, bool selected, bool focused) {
  mvwprintw(win, y, x, "%s: ", label);
  int val_x = x + (int)strlen(label) + 2;

  if (selected && focused) {
    wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
  }

  mvwprintw(win, y, val_x, "< %s >", value);

  if (selected && focused) {
    wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
  }
}

/* Get history mode name */
static const char *history_mode_name(int mode) {
  switch (mode) {
  case HISTORY_MODE_OFF:
    return "Off";
  case HISTORY_MODE_SESSION:
    return "Session only";
  case HISTORY_MODE_PERSISTENT:
    return "Persistent";
  default:
    return "Unknown";
  }
}

/* ============================================================================
 * General Tab Drawing
 * ============================================================================
 */

static void draw_general_tab(WINDOW *win, DialogState *ds, int start_y,
                             int start_x, int width, int *cursor_y,
                             int *cursor_x) {
  (void)width;
  int y = start_y;
  bool focused = (ds->focus == FOCUS_SETTINGS);

  /* Section: Display */
  wattron(win, A_BOLD | A_UNDERLINE);
  mvwprintw(win, y++, start_x, "Display");
  wattroff(win, A_BOLD | A_UNDERLINE);
  y++;

  draw_checkbox(win, y++, start_x + 2, "Show header bar",
                ds->config->general.show_header,
                ds->selected_field == FIELD_SHOW_HEADER, focused);

  draw_checkbox(win, y++, start_x + 2, "Show status bar",
                ds->config->general.show_status_bar,
                ds->selected_field == FIELD_SHOW_STATUS, focused);

  y++;

  /* Section: Data Handling */
  wattron(win, A_BOLD | A_UNDERLINE);
  mvwprintw(win, y++, start_x, "Data Handling");
  wattroff(win, A_BOLD | A_UNDERLINE);
  y++;

  int cursor_x_temp = 0;
  draw_number_field(win, y++, start_x + 2, "Page size (rows)",
                    ds->config->general.page_size,
                    ds->selected_field == FIELD_PAGE_SIZE, focused,
                    ds->editing_number, &ds->num_input, &cursor_x_temp);
  if (ds->selected_field == FIELD_PAGE_SIZE && ds->editing_number) {
    *cursor_y = y - 1;
    *cursor_x = cursor_x_temp;
  }

  draw_number_field(win, y++, start_x + 2, "Prefetch pages",
                    ds->config->general.prefetch_pages,
                    ds->selected_field == FIELD_PREFETCH_PAGES, focused,
                    ds->editing_number, &ds->num_input, &cursor_x_temp);
  if (ds->selected_field == FIELD_PREFETCH_PAGES && ds->editing_number) {
    *cursor_y = y - 1;
    *cursor_x = cursor_x_temp;
  }

  draw_number_field(win, y++, start_x + 2, "Max query rows",
                    ds->config->general.max_result_rows,
                    ds->selected_field == FIELD_MAX_RESULT_ROWS, focused,
                    ds->editing_number, &ds->num_input, &cursor_x_temp);
  if (ds->selected_field == FIELD_MAX_RESULT_ROWS && ds->editing_number) {
    *cursor_y = y - 1;
    *cursor_x = cursor_x_temp;
  }

  draw_checkbox(win, y++, start_x + 2, "Confirm before delete",
                ds->config->general.delete_confirmation,
                ds->selected_field == FIELD_DELETE_CONFIRM, focused);

  y++;

  /* Section: Query History */
  wattron(win, A_BOLD | A_UNDERLINE);
  mvwprintw(win, y++, start_x, "Query History");
  wattroff(win, A_BOLD | A_UNDERLINE);
  y++;

  draw_option(win, y++, start_x + 2, "History mode",
              history_mode_name(ds->config->general.history_mode),
              ds->selected_field == FIELD_HISTORY_MODE, focused);

  draw_number_field(win, y++, start_x + 2, "Max entries",
                    ds->config->general.history_max_size,
                    ds->selected_field == FIELD_HISTORY_MAX_SIZE, focused,
                    ds->editing_number, &ds->num_input, &cursor_x_temp);
  if (ds->selected_field == FIELD_HISTORY_MAX_SIZE && ds->editing_number) {
    *cursor_y = y - 1;
    *cursor_x = cursor_x_temp;
  }

  y++;

  /* Section: Connections */
  wattron(win, A_BOLD | A_UNDERLINE);
  mvwprintw(win, y++, start_x, "Connections");
  wattroff(win, A_BOLD | A_UNDERLINE);
  y++;

  draw_checkbox(win, y++, start_x + 2, "Auto-open first table on connect",
                ds->config->general.auto_open_first_table,
                ds->selected_field == FIELD_AUTO_OPEN_TABLE, focused);

  draw_checkbox(win, y++, start_x + 2, "Close connection when last tab closes",
                ds->config->general.close_conn_on_last_tab,
                ds->selected_field == FIELD_CLOSE_CONN_LAST_TAB, focused);

  y++;

  /* Section: Session */
  wattron(win, A_BOLD | A_UNDERLINE);
  mvwprintw(win, y++, start_x, "Session");
  wattroff(win, A_BOLD | A_UNDERLINE);
  y++;

  draw_checkbox(win, y++, start_x + 2, "Restore session on startup",
                ds->config->general.restore_session,
                ds->selected_field == FIELD_RESTORE_SESSION, focused);

  draw_checkbox(win, y++, start_x + 2, "Confirm before quit",
                ds->config->general.quit_confirmation,
                ds->selected_field == FIELD_QUIT_CONFIRM, focused);

  (void)y; /* Suppress unused warning */
}

/* ============================================================================
 * Hotkeys Tab Drawing
 * ============================================================================
 */

/* Build display order: categories with their actions */
typedef struct {
  bool is_header;
  HotkeyCategory category;
  HotkeyAction action;
} HotkeyDisplayItem;

static size_t build_hotkey_display_list(HotkeyDisplayItem *items, size_t max_items) {
  size_t count = 0;

  /* Display order: General, Navigation, Table, Editor, Query, History, Filters, Sidebar, Connect */
  static const HotkeyCategory display_order[] = {
      HOTKEY_CAT_GENERAL,
      HOTKEY_CAT_NAVIGATION,
      HOTKEY_CAT_TABLE,
      HOTKEY_CAT_EDITOR,
      HOTKEY_CAT_QUERY,
      HOTKEY_CAT_HISTORY,
      HOTKEY_CAT_FILTERS,
      HOTKEY_CAT_SIDEBAR,
      HOTKEY_CAT_CONNECT,
  };

  for (size_t c = 0; c < sizeof(display_order) / sizeof(display_order[0]); c++) {
    HotkeyCategory cat = display_order[c];

    /* Add category header */
    if (count < max_items) {
      items[count].is_header = true;
      items[count].category = cat;
      items[count].action = HOTKEY_COUNT;
      count++;
    }

    /* Add actions in this category (exclude non-configurable actions) */
    for (HotkeyAction a = 0; a < HOTKEY_COUNT; a++) {
      /* Skip config reset actions - they're not user-configurable */
      if (a == HOTKEY_CONFIG_RESET || a == HOTKEY_CONFIG_RESET_ALL)
        continue;

      if (hotkey_get_category(a) == cat && count < max_items) {
        items[count].is_header = false;
        items[count].category = cat;
        items[count].action = a;
        count++;
      }
    }
  }

  return count;
}

/* Get action from display index (skipping headers) */
static HotkeyAction get_action_at_display_index(size_t display_index) {
  HotkeyDisplayItem items[HOTKEY_COUNT + HOTKEY_CAT_COUNT];
  size_t count = build_hotkey_display_list(items,
                                           HOTKEY_COUNT + HOTKEY_CAT_COUNT);

  if (display_index < count && !items[display_index].is_header) {
    return items[display_index].action;
  }
  return HOTKEY_COUNT;
}

/* Get total display items count */
static size_t get_hotkey_display_count(void) {
  HotkeyDisplayItem items[HOTKEY_COUNT + HOTKEY_CAT_COUNT];
  return build_hotkey_display_list(items, HOTKEY_COUNT + HOTKEY_CAT_COUNT);
}

/* Check if display index is a header */
static bool is_display_index_header(size_t display_index) {
  HotkeyDisplayItem items[HOTKEY_COUNT + HOTKEY_CAT_COUNT];
  size_t count = build_hotkey_display_list(items,
                                           HOTKEY_COUNT + HOTKEY_CAT_COUNT);
  return display_index < count && items[display_index].is_header;
}

/* Find next selectable index (skip headers) */
static size_t find_next_selectable(size_t current, size_t max_items) {
  HotkeyDisplayItem items[HOTKEY_COUNT + HOTKEY_CAT_COUNT];
  build_hotkey_display_list(items, HOTKEY_COUNT + HOTKEY_CAT_COUNT);

  size_t next = current + 1;
  while (next < max_items && items[next].is_header) {
    next++;
  }
  return (next < max_items) ? next : current;
}

/* Find previous selectable index (skip headers) */
static size_t find_prev_selectable(size_t current) {
  HotkeyDisplayItem items[HOTKEY_COUNT + HOTKEY_CAT_COUNT];
  build_hotkey_display_list(items, HOTKEY_COUNT + HOTKEY_CAT_COUNT);

  if (current == 0) return current;

  size_t prev = current - 1;
  while (prev > 0 && items[prev].is_header) {
    prev--;
  }
  /* If we landed on a header at index 0, go back to current */
  return items[prev].is_header ? current : prev;
}

static void draw_hotkeys_tab(WINDOW *win, DialogState *ds, int start_y,
                             int start_x, int height, int width) {
  int y = start_y;
  bool focused = (ds->focus == FOCUS_SETTINGS);

  /* Build display list */
  HotkeyDisplayItem items[HOTKEY_COUNT + HOTKEY_CAT_COUNT];
  size_t total_items = build_hotkey_display_list(items,
                                                  HOTKEY_COUNT + HOTKEY_CAT_COUNT);

  /* Calculate visible range (leave 2 lines: 1 empty + 1 for help text) */
  int visible_rows = height - 2;
  if (visible_rows < 1)
    visible_rows = 1;

  /* Adjust scroll to keep highlight visible */
  if (ds->hotkey_highlight >= ds->hotkey_scroll + (size_t)visible_rows) {
    ds->hotkey_scroll = ds->hotkey_highlight - (size_t)visible_rows + 1;
  }
  if (ds->hotkey_highlight < ds->hotkey_scroll) {
    ds->hotkey_scroll = ds->hotkey_highlight;
  }

  /* Draw items */
  for (int i = 0; i < visible_rows && ds->hotkey_scroll + (size_t)i < total_items;
       i++) {
    size_t idx = ds->hotkey_scroll + (size_t)i;
    HotkeyDisplayItem *item = &items[idx];
    bool selected = (idx == ds->hotkey_highlight);

    /* Clear line */
    mvwhline(win, y, start_x, ' ', width - 4);

    if (item->is_header) {
      /* Category header */
      wattron(win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
      mvwprintw(win, y, start_x, "%s", hotkey_category_name(item->category));
      wattroff(win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
    } else {
      /* Action row */
      if (selected && focused) {
        wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        mvwhline(win, y, start_x, ' ', width - 4);
      }

      /* Action name (indented) */
      const char *name = hotkey_action_name(item->action);
      mvwprintw(win, y, start_x + 2, "%-23s", name ? name : "???");

      /* Keys display */
      char *keys = hotkey_get_display(ds->config, item->action);
      if (keys) {
        mvwprintw(win, y, start_x + 27, "%s", keys);
        free(keys);
      }

      if (selected && focused) {
        wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
      }
    }

    y++;
  }

  /* Scroll indicator */
  if (total_items > (size_t)visible_rows) {
    int scroll_y = start_y;
    int scroll_height = visible_rows;
    int thumb_pos = (int)(ds->hotkey_scroll * (size_t)scroll_height / total_items);
    int thumb_size = (visible_rows * scroll_height) / (int)total_items;
    if (thumb_size < 1)
      thumb_size = 1;

    wattron(win, A_DIM);
    for (int i = 0; i < scroll_height; i++) {
      mvwaddch(win, scroll_y + i, width - 2,
               (i >= thumb_pos && i < thumb_pos + thumb_size) ? ACS_CKBOARD
                                                              : ACS_VLINE);
    }
    wattroff(win, A_DIM);
  }

  /* Help text (hardcoded keys - not configurable) */
  /* Empty line is naturally created by visible_rows = height - 2 */
  wattron(win, A_DIM);
  mvwprintw(win, start_y + height - 1, start_x,
            "+/=: Add key  -/x/Del: Remove key  r/Bksp: Reset");
  wattroff(win, A_DIM);
}

/* ============================================================================
 * Tab Bar Drawing
 * ============================================================================
 */

static void draw_tab_bar(WINDOW *win, DialogState *ds, int y, int width) {
  const char *tabs[] = {"General", "Hotkeys"};
  int x = 2;
  bool focused = (ds->focus == FOCUS_TABS);

  for (int i = 0; i < TAB_COUNT; i++) {
    bool selected = (ds->current_tab == (ConfigTab)i);

    if (selected) {
      if (focused) {
        wattron(win, A_REVERSE | A_BOLD);
      } else {
        wattron(win, A_BOLD);
      }
    }

    mvwprintw(win, y, x, " %s ", tabs[i]);

    if (selected) {
      if (focused) {
        wattroff(win, A_REVERSE | A_BOLD);
      } else {
        wattroff(win, A_BOLD);
      }
    }

    x += (int)strlen(tabs[i]) + 3;
  }

  /* Underline with proper T-junctions at borders */
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  mvwaddch(win, y + 1, 0, ACS_LTEE);
  mvwhline(win, y + 1, 1, ACS_HLINE, width - 2);
  mvwaddch(win, y + 1, width - 1, ACS_RTEE);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));
}

/* ============================================================================
 * Buttons Drawing
 * ============================================================================
 */

static void draw_buttons(WINDOW *win, DialogState *ds, int y, int width) {
  const char *buttons[] = {"Save", "Cancel", "Reset"};
  int btn_widths[] = {6, 8, 7};
  int total_width = 0;
  bool focused = (ds->focus == FOCUS_BUTTONS);

  for (int i = 0; i < BTN_COUNT; i++) {
    total_width += btn_widths[i] + 4;
  }

  int x = (width - total_width) / 2;

  for (int i = 0; i < BTN_COUNT; i++) {
    bool selected = (ds->selected_button == i);

    if (selected && focused) {
      wattron(win, A_REVERSE | A_BOLD);
    }

    mvwprintw(win, y, x, "[ %s ]", buttons[i]);

    if (selected && focused) {
      wattroff(win, A_REVERSE | A_BOLD);
    }

    x += btn_widths[i] + 4;
  }
}

/* ============================================================================
 * Main Dialog Drawing
 * ============================================================================
 */

static void draw_dialog(WINDOW *win, DialogState *ds, int *cursor_y,
                        int *cursor_x) {
  werase(win);
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  box(win, 0, 0);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));

  /* Title */
  wattron(win, A_BOLD);
  mvwprintw(win, 0, (ds->width - 15) / 2, " Configuration ");
  wattroff(win, A_BOLD);

  /* Tab bar */
  draw_tab_bar(win, ds, 2, ds->width);

  /* Content area */
  int content_y = 4;
  int content_height = ds->height - 8;
  int content_width = ds->width - 4;

  *cursor_y = 0;
  *cursor_x = 0;

  switch (ds->current_tab) {
  case TAB_GENERAL:
    draw_general_tab(win, ds, content_y, 2, content_width, cursor_y, cursor_x);
    break;
  case TAB_HOTKEYS:
    draw_hotkeys_tab(win, ds, content_y, 2, content_height, ds->width);
    break;
  default:
    break;
  }

  /* Error/success messages */
  int msg_y = ds->height - 4;
  if (ds->error_msg && ds->error_msg[0]) {
    wattron(win, COLOR_PAIR(COLOR_ERROR));
    mvwprintw(win, msg_y, 2, "%.60s", ds->error_msg);
    wattroff(win, COLOR_PAIR(COLOR_ERROR));
  } else if (ds->success_msg && ds->success_msg[0]) {
    wattron(win, COLOR_PAIR(COLOR_NUMBER));
    mvwprintw(win, msg_y, 2, "%.60s", ds->success_msg);
    wattroff(win, COLOR_PAIR(COLOR_NUMBER));
  }

  /* Horizontal line above buttons */
  int btn_line_y = ds->height - 3;
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  mvwaddch(win, btn_line_y, 0, ACS_LTEE);
  mvwhline(win, btn_line_y, 1, ACS_HLINE, ds->width - 2);
  mvwaddch(win, btn_line_y, ds->width - 1, ACS_RTEE);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));

  /* Buttons */
  draw_buttons(win, ds, ds->height - 2, ds->width);

  /* Position cursor for number editing */
  if (ds->editing_number && *cursor_y > 0) {
    wmove(win, *cursor_y, *cursor_x);
  }

  wrefresh(win);
}

/* ============================================================================
 * Hotkey Capture Helper
 * ============================================================================
 */

/* Capture a single key and return the key string. Returns NULL if cancelled.
 * Caller must free the returned string. */
static char *capture_hotkey(WINDOW *parent) {
  int parent_h, parent_w;
  getmaxyx(parent, parent_h, parent_w);
  (void)parent_h;

  int dlg_width = 40;
  int dlg_height = 7;
  int dlg_y = 5;
  int dlg_x = (parent_w - dlg_width) / 2;

  WINDOW *dlg = derwin(parent, dlg_height, dlg_width, dlg_y, dlg_x);
  if (!dlg)
    return NULL;

  keypad(dlg, TRUE);

  werase(dlg);
  wattron(dlg, COLOR_PAIR(COLOR_BORDER));
  box(dlg, 0, 0);
  wattroff(dlg, COLOR_PAIR(COLOR_BORDER));
  wattron(dlg, A_BOLD);
  mvwprintw(dlg, 0, (dlg_width - 14) / 2, " Capture Key ");
  wattroff(dlg, A_BOLD);
  mvwprintw(dlg, 3, (dlg_width - 20) / 2, "Press a key to add...");
  mvwprintw(dlg, 4, (dlg_width - 18) / 2, "(Esc to cancel)");
  wrefresh(dlg);

  int ch = wgetch(dlg);
  UiEvent event;
  render_translate_key(ch, &event);

  delwin(dlg);
  touchwin(parent);
  wrefresh(parent);

  if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
    return NULL;
  }

  /* Build key string from event */
  char key_str[32] = {0};
  size_t pos = 0;

  if (event.key.mods & UI_MOD_CTRL) {
    pos += (size_t)snprintf(key_str + pos, sizeof(key_str) - pos, "CTRL+");
  }

  if (event.key.is_special) {
    const char *key_name = NULL;
    switch (event.key.key) {
    case UI_KEY_UP: key_name = "UP"; break;
    case UI_KEY_DOWN: key_name = "DOWN"; break;
    case UI_KEY_LEFT: key_name = "LEFT"; break;
    case UI_KEY_RIGHT: key_name = "RIGHT"; break;
    case UI_KEY_HOME: key_name = "HOME"; break;
    case UI_KEY_END: key_name = "END"; break;
    case UI_KEY_PAGEUP: key_name = "PGUP"; break;
    case UI_KEY_PAGEDOWN: key_name = "PGDN"; break;
    case UI_KEY_ENTER: key_name = "ENTER"; break;
    case UI_KEY_TAB: key_name = "TAB"; break;
    case UI_KEY_BACKSPACE: key_name = "BACKSPACE"; break;
    case UI_KEY_DELETE: key_name = "DELETE"; break;
    case UI_KEY_F1: key_name = "F1"; break;
    case UI_KEY_F2: key_name = "F2"; break;
    case UI_KEY_F3: key_name = "F3"; break;
    case UI_KEY_F4: key_name = "F4"; break;
    case UI_KEY_F5: key_name = "F5"; break;
    case UI_KEY_F6: key_name = "F6"; break;
    case UI_KEY_F7: key_name = "F7"; break;
    case UI_KEY_F8: key_name = "F8"; break;
    case UI_KEY_F9: key_name = "F9"; break;
    case UI_KEY_F10: key_name = "F10"; break;
    case UI_KEY_F11: key_name = "F11"; break;
    case UI_KEY_F12: key_name = "F12"; break;
    default: break;
    }
    if (key_name) {
      snprintf(key_str + pos, sizeof(key_str) - pos, "%s", key_name);
    }
  } else if (event.key.key > 0) {
    /* Regular character */
    char c = (char)event.key.key;
    if (c == ' ') {
      snprintf(key_str + pos, sizeof(key_str) - pos, "SPACE");
    } else if (c == ',') {
      snprintf(key_str + pos, sizeof(key_str) - pos, "COMMA");
    } else {
      key_str[pos] = c;
      key_str[pos + 1] = '\0';
    }
  }

  if (key_str[0] == '\0') {
    return NULL;
  }

  return str_dup(key_str);
}

/* ============================================================================
 * Input Handling
 * ============================================================================
 */

static bool handle_general_input(DialogState *ds, const UiEvent *event) {
  int key_char = render_event_get_char(event);

  if (ds->editing_number) {
    /* Number input mode */
    if (render_event_is_special(event, UI_KEY_ENTER) ||
        render_event_is_special(event, UI_KEY_ESCAPE)) {
      /* Finish editing */
      int value = number_input_value(&ds->num_input);

      if (ds->selected_field == FIELD_PAGE_SIZE) {
        ds->config->general.page_size = value;
      } else if (ds->selected_field == FIELD_PREFETCH_PAGES) {
        ds->config->general.prefetch_pages = value;
      } else if (ds->selected_field == FIELD_MAX_RESULT_ROWS) {
        ds->config->general.max_result_rows = value;
      } else if (ds->selected_field == FIELD_HISTORY_MAX_SIZE) {
        ds->config->general.history_max_size = value;
      }

      ds->editing_number = false;
      return true;
    }

    number_input_handle_key(&ds->num_input, event);
    return true;
  }

  /* Navigation */
  if (render_event_is_special(event, UI_KEY_UP) || key_char == 'k') {
    if (ds->selected_field > 0) {
      ds->selected_field--;
    } else {
      ds->focus = FOCUS_TABS;
    }
    return true;
  }

  if (render_event_is_special(event, UI_KEY_DOWN) || key_char == 'j') {
    if (ds->selected_field < FIELD_COUNT - 1) {
      ds->selected_field++;
    } else {
      ds->focus = FOCUS_BUTTONS;
    }
    return true;
  }

  /* Toggle/Edit */
  if (key_char == ' ' || render_event_is_special(event, UI_KEY_ENTER)) {
    switch (ds->selected_field) {
    case FIELD_SHOW_HEADER:
      ds->config->general.show_header = !ds->config->general.show_header;
      break;
    case FIELD_SHOW_STATUS:
      ds->config->general.show_status_bar = !ds->config->general.show_status_bar;
      break;
    case FIELD_PAGE_SIZE:
      number_input_init(&ds->num_input, ds->config->general.page_size,
                        CONFIG_PAGE_SIZE_MIN, CONFIG_PAGE_SIZE_MAX);
      ds->editing_number = true;
      break;
    case FIELD_PREFETCH_PAGES:
      number_input_init(&ds->num_input, ds->config->general.prefetch_pages,
                        CONFIG_PREFETCH_PAGES_MIN, CONFIG_PREFETCH_PAGES_MAX);
      ds->editing_number = true;
      break;
    case FIELD_MAX_RESULT_ROWS:
      number_input_init(&ds->num_input, ds->config->general.max_result_rows,
                        CONFIG_MAX_RESULT_ROWS_MIN, CONFIG_MAX_RESULT_ROWS_MAX);
      ds->editing_number = true;
      break;
    case FIELD_RESTORE_SESSION:
      ds->config->general.restore_session = !ds->config->general.restore_session;
      break;
    case FIELD_QUIT_CONFIRM:
      ds->config->general.quit_confirmation =
          !ds->config->general.quit_confirmation;
      break;
    case FIELD_DELETE_CONFIRM:
      ds->config->general.delete_confirmation =
          !ds->config->general.delete_confirmation;
      break;
    case FIELD_HISTORY_MODE:
      /* Cycle through history modes: Off -> Session -> Persistent -> Off */
      ds->config->general.history_mode =
          (ds->config->general.history_mode + 1) % 3;
      break;
    case FIELD_HISTORY_MAX_SIZE:
      number_input_init(&ds->num_input, ds->config->general.history_max_size,
                        HISTORY_SIZE_MIN, HISTORY_SIZE_MAX);
      ds->editing_number = true;
      break;
    case FIELD_AUTO_OPEN_TABLE:
      ds->config->general.auto_open_first_table =
          !ds->config->general.auto_open_first_table;
      break;
    case FIELD_CLOSE_CONN_LAST_TAB:
      ds->config->general.close_conn_on_last_tab =
          !ds->config->general.close_conn_on_last_tab;
      break;
    default:
      break;
    }
    return true;
  }

  return false;
}

static bool handle_hotkeys_input(DialogState *ds, const UiEvent *event) {
  int key_char = render_event_get_char(event);
  size_t total_items = get_hotkey_display_count();

  /* Navigation - skip category headers */
  if (render_event_is_special(event, UI_KEY_UP) || key_char == 'k') {
    size_t prev = find_prev_selectable(ds->hotkey_highlight);
    if (prev < ds->hotkey_highlight) {
      ds->hotkey_highlight = prev;
    } else if (ds->hotkey_highlight == prev) {
      /* Already at first item, move to tabs */
      ds->focus = FOCUS_TABS;
    }
    return true;
  }

  if (render_event_is_special(event, UI_KEY_DOWN) || key_char == 'j') {
    size_t next = find_next_selectable(ds->hotkey_highlight, total_items);
    if (next > ds->hotkey_highlight) {
      ds->hotkey_highlight = next;
    } else {
      /* Already at last item, move to buttons */
      ds->focus = FOCUS_BUTTONS;
    }
    return true;
  }

  if (render_event_is_special(event, UI_KEY_PAGEUP)) {
    size_t target = (ds->hotkey_highlight > 10) ? ds->hotkey_highlight - 10 : 0;
    /* Find first selectable at or after target */
    while (target < total_items && is_display_index_header(target)) {
      target++;
    }
    if (target < total_items) {
      ds->hotkey_highlight = target;
    }
    return true;
  }

  if (render_event_is_special(event, UI_KEY_PAGEDOWN)) {
    size_t target = ds->hotkey_highlight + 10;
    if (target >= total_items) {
      target = total_items - 1;
    }
    /* Find last selectable at or before target */
    while (target > 0 && is_display_index_header(target)) {
      target--;
    }
    if (!is_display_index_header(target)) {
      ds->hotkey_highlight = target;
    }
    return true;
  }

  /* Add hotkey - hardcoded +/= */
  if (key_char == '+' || key_char == '=') {
    HotkeyAction action = get_action_at_display_index(ds->hotkey_highlight);
    if (action < HOTKEY_COUNT) {
      char *new_key = capture_hotkey(ds->dialog_win);
      if (new_key) {
        if (hotkey_add_key(ds->config, action, new_key)) {
          free(ds->success_msg);
          ds->success_msg = str_printf("Added key: %s", new_key);
        }
        free(new_key);
      }
    }
    return true;
  }

  /* Remove hotkey - hardcoded -/x/Delete */
  if (key_char == '-' || key_char == 'x' ||
      render_event_is_special(event, UI_KEY_DELETE)) {
    HotkeyAction action = get_action_at_display_index(ds->hotkey_highlight);
    if (action < HOTKEY_COUNT) {
      HotkeyBinding *binding = &ds->config->hotkeys[action];
      if (binding->num_keys > 0) {
        /* Remove the last key */
        char *removed = str_dup(binding->keys[binding->num_keys - 1]);
        hotkey_remove_key(ds->config, action, binding->num_keys - 1);
        free(ds->success_msg);
        ds->success_msg = str_printf("Removed key: %s", removed ? removed : "");
        free(removed);
      }
    }
    return true;
  }

  /* Reset single hotkey - hardcoded r/Backspace */
  if (key_char == 'r' || render_event_is_special(event, UI_KEY_BACKSPACE)) {
    HotkeyAction action = get_action_at_display_index(ds->hotkey_highlight);
    if (action < HOTKEY_COUNT) {
      config_reset_hotkey(ds->config, action);
      free(ds->success_msg);
      ds->success_msg = str_dup("Hotkey reset to default");
    }
    return true;
  }

  return false;
}

static bool handle_button_input(DialogState *ds, const UiEvent *event,
                                bool *should_exit, ConfigResult *result) {
  int key_char = render_event_get_char(event);

  if (render_event_is_special(event, UI_KEY_LEFT) || key_char == 'h') {
    if (ds->selected_button > 0)
      ds->selected_button--;
    return true;
  }

  if (render_event_is_special(event, UI_KEY_RIGHT) || key_char == 'l') {
    if (ds->selected_button < BTN_COUNT - 1)
      ds->selected_button++;
    return true;
  }

  if (render_event_is_special(event, UI_KEY_UP) || key_char == 'k') {
    ds->focus = FOCUS_SETTINGS;
    return true;
  }

  if (render_event_is_special(event, UI_KEY_ENTER)) {
    free(ds->error_msg);
    ds->error_msg = NULL;
    free(ds->success_msg);
    ds->success_msg = NULL;

    switch (ds->selected_button) {
    case BTN_SAVE: {
      /* Validate before saving */
      char *validate_err = NULL;
      if (!config_validate(ds->config, &validate_err)) {
        ds->error_msg = validate_err ? validate_err : str_dup("Invalid config");
        return true;
      }

      /* Save to disk */
      char *save_err = NULL;
      if (config_save(ds->config, &save_err)) {
        *result = CONFIG_RESULT_SAVED;
        *should_exit = false;
      } else {
        ds->error_msg = save_err ? save_err : str_dup("Save failed");
      }
      break;
    }

    case BTN_CANCEL:
      *result = CONFIG_RESULT_CANCELLED;
      *should_exit = false;
      break;

    case BTN_RESET:
      /* Reset to defaults */
      config_free(ds->config);
      ds->config = config_get_defaults();
      ds->success_msg = str_dup("Reset to defaults (not saved)");
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

ConfigResult config_view_show(TuiState *state) {
  return config_view_show_tab(state, CONFIG_TAB_GENERAL);
}

ConfigResult config_view_show_tab(TuiState *state, ConfigStartTab start_tab) {
  ConfigResult result = CONFIG_RESULT_CANCELLED;

  if (!state || !state->app || !state->app->config) {
    return result;
  }

  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

  /* Make a working copy of the config */
  Config *working_config = config_copy(state->app->config);
  if (!working_config) {
    return result;
  }

  /* Calculate dialog size */
  int width = term_cols - 10;
  if (width < MIN_DIALOG_WIDTH)
    width = MIN_DIALOG_WIDTH;
  if (width > MAX_DIALOG_WIDTH)
    width = MAX_DIALOG_WIDTH;

  int height = term_rows - 6;
  if (height < MIN_DIALOG_HEIGHT)
    height = MIN_DIALOG_HEIGHT;
  if (height > MAX_DIALOG_HEIGHT)
    height = MAX_DIALOG_HEIGHT;

  int starty = (term_rows - height) / 2;
  int startx = (term_cols - width) / 2;
  if (starty < 0)
    starty = 0;
  if (startx < 0)
    startx = 0;

  WINDOW *dialog = newwin(height, width, starty, startx);
  if (!dialog) {
    config_free(working_config);
    return result;
  }

  keypad(dialog, TRUE);

  /* Initialize dialog state */
  DialogState ds = {0};
  ds.config = working_config;
  ds.original = state->app->config;
  ds.current_tab = (start_tab == CONFIG_TAB_HOTKEYS) ? TAB_HOTKEYS : TAB_GENERAL;
  ds.focus = FOCUS_SETTINGS;
  ds.selected_field = 0;
  ds.selected_button = BTN_SAVE;
  ds.height = height;
  ds.width = width;
  ds.dialog_win = dialog;

  bool running = true;
  while (running) {
    curs_set(ds.editing_number ? 1 : 0);

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
      if (ds.editing_number) {
        /* Finish number editing first */
        int value = number_input_value(&ds.num_input);
        if (ds.selected_field == FIELD_PAGE_SIZE) {
          ds.config->general.page_size = value;
        } else if (ds.selected_field == FIELD_PREFETCH_PAGES) {
          ds.config->general.prefetch_pages = value;
        } else if (ds.selected_field == FIELD_MAX_RESULT_ROWS) {
          ds.config->general.max_result_rows = value;
        } else if (ds.selected_field == FIELD_HISTORY_MAX_SIZE) {
          ds.config->general.history_max_size = value;
        }
        ds.editing_number = false;
      }

      if (ds.focus == FOCUS_TABS) {
        ds.focus = FOCUS_SETTINGS;
      } else if (ds.focus == FOCUS_SETTINGS) {
        ds.focus = FOCUS_BUTTONS;
      } else {
        ds.focus = FOCUS_TABS;
      }
      continue;
    }

    /* Escape closes */
    if (render_event_is_special(&event, UI_KEY_ESCAPE) && !ds.editing_number) {
      running = false;
      continue;
    }

    /* Tab switch hotkeys (work from any focus) */
    if (!ds.editing_number) {
      if (hotkey_matches(ds.config, &event, HOTKEY_PREV_TAB)) {
        if (ds.current_tab > 0) {
          ds.current_tab--;
          ds.selected_field = 0;
          ds.hotkey_highlight = 1;
          ds.hotkey_scroll = 0;
        }
        continue;
      }
      if (hotkey_matches(ds.config, &event, HOTKEY_NEXT_TAB)) {
        if (ds.current_tab < TAB_COUNT - 1) {
          ds.current_tab++;
          ds.selected_field = 0;
          ds.hotkey_highlight = 1;
          ds.hotkey_scroll = 0;
        }
        continue;
      }
    }

    /* Handle focus-specific input */
    switch (ds.focus) {
    case FOCUS_TABS:
      if (render_event_is_special(&event, UI_KEY_LEFT)) {
        if (ds.current_tab > 0)
          ds.current_tab--;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (ds.current_tab < TAB_COUNT - 1)
          ds.current_tab++;
      } else if (render_event_is_special(&event, UI_KEY_DOWN) ||
                 render_event_is_special(&event, UI_KEY_ENTER)) {
        ds.focus = FOCUS_SETTINGS;
        /* Reset field selection when switching tabs */
        ds.selected_field = 0;
        ds.hotkey_highlight = 1; /* Skip first header */
        ds.hotkey_scroll = 0;
      }
      break;

    case FOCUS_SETTINGS:
      if (ds.current_tab == TAB_GENERAL) {
        handle_general_input(&ds, &event);
      } else if (ds.current_tab == TAB_HOTKEYS) {
        handle_hotkeys_input(&ds, &event);
      }
      break;

    case FOCUS_BUTTONS:
      handle_button_input(&ds, &event, &running, &result);
      break;
    }
  }

  /* If saved, update the app's config */
  if (result == CONFIG_RESULT_SAVED) {
    /* Reload config from disk to get the saved version */
    Config *new_config = config_load(NULL);
    if (new_config) {
      config_free(state->app->config);
      state->app->config = new_config;

      /* Apply runtime settings */
      state->app->page_size = (size_t)new_config->general.page_size;
      state->app->header_visible = new_config->general.show_header;
      state->app->status_visible = new_config->general.show_status_bar;
    }
  }

  curs_set(0);
  delwin(dialog);
  config_free(working_config);
  free(ds.error_msg);
  free(ds.success_msg);

  /* Redraw main screen */
  touchwin(stdscr);
  if (state) {
    tui_refresh(state);
  }

  return result;
}
