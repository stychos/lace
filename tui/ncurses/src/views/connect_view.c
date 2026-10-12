/*
 * Lace
 * Connection Manager dialog - Combined saved connections tree + quick connect
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connect_internal.h"

/* ============================================================================
 * URL Panel
 * ============================================================================
 */

static void draw_url_panel(WINDOW *win, DialogState *ds, int start_y,
                           int start_x, int width, int *cursor_y,
                           int *cursor_x) {
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
  attr_t url_attr =
      ds->focus == FOCUS_URL ? COLOR_PAIR(COLOR_SELECTED) : A_NORMAL;
  int url_cursor_x =
      text_input_draw(win, &ds->url_input, y, start_x, ds->url_input_width,
                      url_attr, ds->focus == FOCUS_URL);
  if (url_cursor_x >= 0) {
    *cursor_y = y;
    *cursor_x = url_cursor_x;
  }
  wattron(win, A_DIM);
  mvwhline(win, y + 1, start_x, ACS_HLINE, ds->url_input_width);
  wattroff(win, A_DIM);
  y += 3;

  /* Examples */
  wattron(win, A_DIM);
  mvwprintw(win, y++, start_x, "Examples:");
  mvwprintw(win, y++, start_x + 2, "sqlite:///path/to/db.sqlite");
  mvwprintw(win, y++, start_x + 2, "postgres://user:pass@host/db");
  mvwprintw(win, y++, start_x + 2, "mysql://user@host:3306/db");
  y++;
  mvwprintw(win, y++, start_x, "SSH tunnel:");
  mvwprintw(win, y++, start_x + 2, "postgres+ssh://sshhost/user:pass@localhost/db");
  mvwprintw(win, y++, start_x + 2, "mysql+ssh://user@sshhost/dbuser@localhost/db");
  wattroff(win, A_DIM);
}

/* ============================================================================
 * Buttons
 * ============================================================================
 */

static void draw_buttons(WINDOW *win, DialogState *ds, int y, int width) {
  const char *buttons[] = {"Connect", "New WS", "Test", "Save",
                           "Delete",  "Close",  "Quit"};
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

static void draw_dialog(WINDOW *win, DialogState *ds, int *cursor_y,
                        int *cursor_x) {
  werase(win);
  DRAW_BOX(win, COLOR_BORDER);
  WITH_ATTR(win, A_BOLD,
            mvwprintw(win, 0, (ds->width - 20) / 2, " Connection Manager "));

  /* Horizontal line above buttons (calculate first for divider) */
  int btn_line_y = ds->height - 4;

  /* Vertical divider between panels - stop at button bar */
  int divider_x = TREE_PANEL_WIDTH + 2;
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  for (int i = 1; i < btn_line_y; i++) {
    mvwaddch(win, i, divider_x, ACS_VLINE);
  }
  mvwaddch(win, 0, divider_x, ACS_TTEE);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));

  /* Left panel: Saved connections tree */
  connect_draw_tree(win, ds, 2, 2, ds->tree_height, TREE_PANEL_WIDTH);

  /* Right panel: Quick connect URL */
  int url_panel_x = divider_x + 2;
  int url_panel_width = ds->width - url_panel_x - 2;
  draw_url_panel(win, ds, 2, url_panel_x, url_panel_width, cursor_y, cursor_x);

  /* Horizontal line above buttons */
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  mvwaddch(win, btn_line_y, 0, ACS_LTEE);
  mvwhline(win, btn_line_y, 1, ACS_HLINE, ds->width - 2);
  mvwaddch(win, btn_line_y, ds->width - 1, ACS_RTEE);
  mvwaddch(win, btn_line_y, divider_x, ACS_BTEE);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));

  /* Error/success messages */
  int msg_y = ds->height - 3;
  int msg_max_len = ds->width - 4; /* Leave margin on both sides */
  if (msg_max_len < 10)
    msg_max_len = 10;
  if (ds->error_msg && ds->error_msg[0]) {
    wattron(win, COLOR_PAIR(COLOR_ERROR));
    tui_print_clip(win, msg_y, 2, msg_max_len, ds->error_msg);
    wattroff(win, COLOR_PAIR(COLOR_ERROR));
  } else if (ds->success_msg && ds->success_msg[0]) {
    wattron(win, COLOR_PAIR(COLOR_NUMBER));
    tui_print_clip(win, msg_y, 2, msg_max_len, ds->success_msg);
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

char *connect_prompt_text(WINDOW *parent, const char *title,
                               const char *label, const char *initial_value) {
  (void)parent; /* Centered on the screen, over the connect dialog */
  PromptOptions opts = {title, label, initial_value, NULL, false, false};
  return tui_prompt(&opts);
}

/* Confirmation dialog - returns true if user confirms */
bool connect_confirm(WINDOW *parent, const char *title,
                                const char *message) {
  int parent_h, parent_w;
  getmaxyx(parent, parent_h, parent_w);
  (void)parent_h;

  int dlg_height = 7;
  int dlg_width = 50;
  if (dlg_width > parent_w - 10)
    dlg_width = parent_w - 10;

  int dlg_y = 5;
  int dlg_x = (parent_w - dlg_width) / 2;

  WINDOW *dlg = derwin(parent, dlg_height, dlg_width, dlg_y, dlg_x);
  if (!dlg)
    return false;

  keypad(dlg, TRUE);

  /* Focus: 0 = Yes, 1 = No */
  int focus = 1; /* Default to No for safety */

  bool result = false;
  bool running = true;

  while (running) {
    werase(dlg);
    DRAW_BOX(dlg, COLOR_BORDER);

    int title_len = (int)strlen(title) + 2;
    WITH_ATTR(dlg, A_BOLD,
              mvwprintw(dlg, 0, (dlg_width - title_len) / 2, " %s ", title));

    /* Message */
    int msg_x = (dlg_width - (int)strlen(message)) / 2;
    if (msg_x < 2)
      msg_x = 2;
    mvwprintw(dlg, 2, msg_x, "%s", message);

    /* Buttons */
    int btn_x = dlg_width / 2 - 10;
    if (focus == 0)
      wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 5, btn_x, "[ Yes ]");
    if (focus == 0)
      wattroff(dlg, A_REVERSE);
    if (focus == 1)
      wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 5, btn_x + 9, "[ No ]");
    if (focus == 1)
      wattroff(dlg, A_REVERSE);

    curs_set(0);
    wrefresh(dlg);

    int ch = wgetch(dlg);
    UiEvent event;
    render_translate_key(ch, &event);

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_BTAB) ||
               render_event_is_special(&event, UI_KEY_LEFT) ||
               render_event_is_special(&event, UI_KEY_RIGHT) ||
               render_event_get_char(&event) == 'h' ||
               render_event_get_char(&event) == 'l') {
      focus = 1 - focus; /* Toggle between 0 and 1 */
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
static char *connect_prompt_password(WINDOW *parent, const char *title,
                                  const char *label) {
  (void)parent;
  PromptOptions opts = {title, label, NULL, NULL, true, true};
  return tui_prompt(&opts); /* An empty password is a valid answer */
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

/* Connect (or just test). On success returns the connection string and, when
 * it came from a saved connection, sets *saved_id to a copy of its id so the
 * session can restore it later (NULL for a typed URL). */
static char *try_connect(DialogState *ds, bool test_only, ConnectMode *mode,
                         char **saved_id) {
  char *connstr = NULL;
  char *err = NULL;
  SavedConnection *saved_conn = NULL;

  /* Determine source: tree selection or URL */
  if (ds->focus == FOCUS_TREE ||
      (ds->focus == FOCUS_BUTTONS && ds->url_input.len == 0)) {
    /* Use selected saved connection */
    ConnectionItem *item =
        connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
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
    if (!strstr(ds->url_input.buf, "://")) {
      connstr = connstr_from_path(ds->url_input.buf, &err);
      if (!connstr) {
        ds->error_msg = err ? err : str_dup("Invalid file path");
        return NULL;
      }
    } else {
      connstr = str_dup(ds->url_input.buf);
    }
  }

  /* Test connection */
  DbConnection *conn = db_connect(ds->client, connstr, &err);
  if (!conn) {
    /* Connection failed - only prompt for password if it's an auth error */
    if (saved_conn && saved_conn->driver &&
        strcmp(saved_conn->driver, "sqlite") != 0 && is_auth_error(err)) {
      /* Prompt for password */
      char *password = connect_prompt_password(ds->dialog_win, "Password Required",
                                            "Enter password:");
      if (password) {
        /* Rebuild connection string with password */
        str_secure_free(connstr); /* Connection string may contain password */
        free(err);
        err = NULL;

        /* Same builder as the first attempt, so an SSH tunnel is kept (a
         * plain rebuild connected straight to the database host) */
        connstr = lace_saved_conn_build_connstr(saved_conn, password, NULL);

        str_secure_free(password);

        if (connstr) {
          /* Retry connection */
          conn = db_connect(ds->client, connstr, &err);
        }
      }
    }

    if (!conn) {
      ds->error_msg = err ? err : str_dup("Connection failed");
      str_secure_free(connstr); /* Connection string may contain password */
      return NULL;
    }
  }

  db_disconnect(conn);

  if (test_only) {
    ds->success_msg = str_dup("Connection successful!");
    str_secure_free(connstr); /* Connection string may contain password */
    return NULL;
  }

  /* Determine mode */
  if (mode) {
    *mode = ds->has_existing_tabs ? CONNECT_MODE_NEW_TAB
                                  : CONNECT_MODE_NEW_WORKSPACE;
  }
  if (saved_id) {
    *saved_id = saved_conn && saved_conn->id ? str_dup(saved_conn->id) : NULL;
  }

  return connstr;
}

/* ============================================================================
 * Input Handling
 * ============================================================================
 */

static bool handle_button_input(DialogState *ds, const UiEvent *event,
                                bool *should_exit, ConnectResult *result) {
  if (hotkey_matches(ds->config, event, HOTKEY_MOVE_LEFT)) {
    if (ds->selected_button > 0) {
      ds->selected_button--;
    }
    return true;
  }

  if (hotkey_matches(ds->config, event, HOTKEY_MOVE_RIGHT)) {
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
      char *connstr = try_connect(ds, false, &mode, &result->saved_conn_id);
      if (connstr) {
        result->connstr = connstr;
        result->mode = CONNECT_MODE_NEW_TAB;
        *should_exit = false; /* false = stop running */
      }
      break;
    }

    case BTN_NEW_WS: {
      ConnectMode mode = CONNECT_MODE_NEW_WORKSPACE;
      char *connstr = try_connect(ds, false, &mode, &result->saved_conn_id);
      if (connstr) {
        result->connstr = connstr;
        result->mode = CONNECT_MODE_NEW_WORKSPACE;
        *should_exit = false; /* false = stop running */
      }
      break;
    }

    case BTN_TEST:
      try_connect(ds, true, NULL, NULL);
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
      if (!strstr(ds->url_input.buf, "://")) {
        url_to_save = connstr_from_path(ds->url_input.buf, &err);
        if (!url_to_save) {
          ds->error_msg = err ? err : str_dup("Invalid file path");
          break;
        }
      } else {
        url_to_save = str_dup(ds->url_input.buf);
      }

      char *save_err = NULL;
      if (connect_save_dialog(ds->dialog_win, ds->mgr, url_to_save, &save_err)) {
        ds->success_msg = str_dup("Connection saved!");
      } else if (save_err) {
        ds->error_msg = save_err;
      }

      free(url_to_save);
      break;
    }

    case BTN_DELETE: {
      ConnectionItem *item =
          connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
      if (item && item->parent) {
        const char *name = connmgr_item_name(item);
        char msg[128];
        snprintf(msg, sizeof(msg), "Delete '%s'?", name ? name : "item");

        if (connect_confirm(ds->dialog_win, "Confirm Delete", msg)) {
          connmgr_remove_item(ds->mgr, item);
          if (ds->tree_highlight > 0) {
            ds->tree_highlight--;
          }
        }
      }
      break;
    }

    case BTN_CLOSE:
      *should_exit = false; /* false = stop running */
      break;

    case BTN_QUIT:
      result->mode = CONNECT_MODE_QUIT;
      *should_exit = false; /* false = stop running */
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
  if (width < MIN_DIALOG_WIDTH)
    width = MIN_DIALOG_WIDTH;
  if (width > 100)
    width = 100;

  int height = term_rows - 6;
  if (height < MIN_DIALOG_HEIGHT)
    height = MIN_DIALOG_HEIGHT;
  if (height > 30)
    height = 30;

  WINDOW *dialog = dialog_create(height, width, term_rows, term_cols);
  if (!dialog) {
    connmgr_free(mgr);
    return result;
  }

  /* Initialize dialog state */
  DialogState ds = {0};
  ds.mgr = mgr;
  ds.config = state && state->app ? state->app->config : NULL;
  ds.client = state && state->app ? state->app->client : NULL;
  ds.focus = connmgr_count_visible(mgr) > 0 ? FOCUS_TREE : FOCUS_URL;
  ds.prev_panel_focus = ds.focus; /* Initialize to same as initial focus */
  ds.tree_highlight = 0;
  ds.tree_scroll = 0;
  ds.selected_button = BTN_CONNECT;
  ds.has_existing_tabs = has_existing;
  ds.height = height;
  ds.width = width;
  ds.tree_height = height - 6;
  ds.dialog_win = dialog;

  int url_input_width = width - TREE_PANEL_WIDTH - 8;
  text_input_init(&ds.url_input, NULL, false);
  ds.url_input_width = url_input_width < 3 ? 3 : url_input_width;

  bool running = true;
  while (running) {
    /* A move belongs to the tree: leaving it (Tab, 'l') ends the move, so
     * the URL panel and buttons can never change the tree under it */
    if (ds.move.active && ds.focus != FOCUS_TREE) {
      memset(&ds.move, 0, sizeof(ds.move));
    }
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

    /* Tab / Shift+Tab cycle focus in screen order: tree, URL, buttons */
    if (render_event_is_special(&event, UI_KEY_TAB) ||
        render_event_is_special(&event, UI_KEY_BTAB)) {
      bool back = render_event_is_special(&event, UI_KEY_BTAB);
      if (ds.focus == FOCUS_TREE || ds.focus == FOCUS_URL)
        ds.prev_panel_focus = ds.focus;
      if (ds.focus == FOCUS_TREE) {
        ds.focus = back ? FOCUS_BUTTONS : FOCUS_URL;
      } else if (ds.focus == FOCUS_URL) {
        ds.focus = back ? FOCUS_TREE : FOCUS_BUTTONS;
      } else {
        ds.focus = back ? FOCUS_URL : FOCUS_TREE;
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
      try_connect(&ds, true, NULL, NULL);
      continue;
    }

    /* Save URL to list hotkey */
    if (ds.config && hotkey_matches(ds.config, &event, HOTKEY_CONN_SAVE) &&
        ds.url_input.len > 0) {
      char *url_to_save = NULL;
      char *err = NULL;

      if (!strstr(ds.url_input.buf, "://")) {
        url_to_save = connstr_from_path(ds.url_input.buf, &err);
        if (!url_to_save) {
          ds.error_msg = err ? err : str_dup("Invalid file path");
          continue;
        }
      } else {
        url_to_save = str_dup(ds.url_input.buf);
      }

      char *save_err = NULL;
      if (connect_save_dialog(dialog, mgr, url_to_save, &save_err)) {
        ds.success_msg = str_dup("Connection saved!");
      } else if (save_err) {
        ds.error_msg = save_err;
      }

      free(url_to_save);
      continue;
    }

    /* Global shortcuts (work regardless of focus) */

    /* New folder hotkey (global) */
    if (ds.config &&
        hotkey_matches(ds.config, &event, HOTKEY_CONN_NEW_FOLDER) &&
        !ds.move.active && ds.focus != FOCUS_URL) {
      char *name = connect_prompt_text(dialog, "New Folder", "Name:", "");
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
      if (connect_connection_form(dialog, mgr, NULL, &mgr->root, &err)) {
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
            char *connstr =
                try_connect(&ds, false, &mode, &result.saved_conn_id);
            if (connstr) {
              result.connstr = connstr;
              result.mode = mode;
              running = false;
            }
          }
        }
      } else {
        connect_tree_handle_key(&ds, &event);
      }
      break;

    case FOCUS_URL:
      if (render_event_is_special(&event, UI_KEY_ENTER)) {
        if (ds.url_input.len > 0) {
          ConnectMode mode;
          char *connstr = try_connect(&ds, false, &mode, &result.saved_conn_id);
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
        text_input_handle_key(&ds.url_input, &event);
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

  text_input_free(&ds.url_input); /* Wiped: it may hold a password */

  /* Redraw main screen */
  touchwin(stdscr);
  if (state) {
    tui_refresh(state);
  }

  return result;
}

