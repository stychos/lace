/*
 * Lace
 * Connection Manager dialog: save dialog and new/edit connection form
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connect_internal.h"

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
    list->folders =
        safe_reallocarray(list->folders, new_cap, sizeof(ConnectionItem *));
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

  /* Validate depth to prevent overflow in size calculation */
  if (depth < 0 || depth > 1000) {
    return str_dup(name); /* Fallback for pathological nesting */
  }

  size_t indent = (size_t)depth * 2;
  size_t name_len = strlen(name);

  char *result = safe_malloc(indent + name_len + 1);

  for (size_t i = 0; i < indent; i++) {
    result[i] = ' ';
  }
  memcpy(result + indent, name, name_len + 1); /* Include null terminator */
  return result;
}

/* ============================================================================
 * Save Connection Dialog
 * ============================================================================
 */

bool connect_save_dialog(WINDOW *parent, ConnectionManager *mgr,
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
  size_t selected_folder = 0; /* Default to root */

  int parent_h, parent_w;
  getmaxyx(parent, parent_h, parent_w);
  (void)parent_h;

  /* Dialog with name input and folder picker */
  int dlg_height = 12;
  int dlg_width = 50;
  if (dlg_width > parent_w - 10)
    dlg_width = parent_w - 10;

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

  TextInput name; /* Pre-filled with the parsed name */
  text_input_init(&name, conn->name, false);

  /* Focus: 0 = name, 1 = folder, 2 = Save button, 3 = Cancel button */
  int focus = 0;

  bool result = false;
  bool running = true;

  while (running) {
    werase(dlg);
    DRAW_BOX(dlg, COLOR_BORDER);
    WITH_ATTR(dlg, A_BOLD,
              mvwprintw(dlg, 0, (dlg_width - 18) / 2, " Save Connection "));

    /* Name input */
    mvwprintw(dlg, 2, 2, "Name:");
    int name_cursor_x =
        text_input_draw(dlg, &name, 3, 2, dlg_width - 4,
                        focus == 0 ? COLOR_PAIR(COLOR_SELECTED) : A_NORMAL,
                        focus == 0);

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
    if (focus == 2)
      wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 10, btn_x, "[ Save ]");
    if (focus == 2)
      wattroff(dlg, A_REVERSE);
    if (focus == 3)
      wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 10, btn_x + 10, "[ Cancel ]");
    if (focus == 3)
      wattroff(dlg, A_REVERSE);

    /* Position cursor */
    if (focus == 0 && name_cursor_x >= 0) {
      wmove(dlg, 3, name_cursor_x);
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
      focus = FOCUS_NEXT(focus, 4);
    } else if (render_event_is_special(&event, UI_KEY_UP) ||
               render_event_is_special(&event, UI_KEY_BTAB)) {
      focus = FOCUS_PREV(focus, 4);
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (focus == 3) {
        /* Cancel */
        running = false;
      } else if (name.len > 0) {
        /* Save the connection */
        free(conn->name);
        conn->name = str_dup(name.buf);

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
      text_input_handle_key(&name, &event);
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
        if (focus == 3)
          focus = 2;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (focus == 2)
          focus = 3;
      }
    }
  }

  curs_set(0);
  delwin(dlg);
  text_input_free(&name);
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
  char driver[32]; /* sqlite, postgres, mysql, mariadb */
  char host[128];
  char port[8];
  char database[256];
  char user[64];
  char password[64];
  bool save_password;
} ConnectionFormData;

bool connect_connection_form(WINDOW *parent, ConnectionManager *mgr,
                                 ConnectionItem *edit_item, /* NULL for new */
                                 ConnectionItem *parent_folder, char **error) {
  int parent_h, parent_w;
  getmaxyx(parent, parent_h, parent_w);
  (void)parent_h;

  int dlg_height = 20;
  int dlg_width = 60;
  if (dlg_width > parent_w - 6)
    dlg_width = parent_w - 6;

  int dlg_y = 2;
  int dlg_x = (parent_w - dlg_width) / 2;

  WINDOW *dlg = derwin(parent, dlg_height, dlg_width, dlg_y, dlg_x);
  if (!dlg) {
    *error = str_dup("Failed to create dialog");
    return false;
  }

  keypad(dlg, TRUE);

  ConnectionFormData form = {0};
  strncpy(form.driver, "postgres",
          sizeof(form.driver) - 1); /* Default driver */

  /* Pre-fill if editing */
  if (edit_item && connmgr_is_connection(edit_item)) {
    SavedConnection *conn = &edit_item->connection;
    if (conn->name)
      strncpy(form.name, conn->name, sizeof(form.name) - 1);
    if (conn->driver)
      strncpy(form.driver, conn->driver, sizeof(form.driver) - 1);
    if (conn->host)
      strncpy(form.host, conn->host, sizeof(form.host) - 1);
    if (conn->port > 0)
      snprintf(form.port, sizeof(form.port), "%d", conn->port);
    if (conn->database)
      strncpy(form.database, conn->database, sizeof(form.database) - 1);
    if (conn->user)
      strncpy(form.user, conn->user, sizeof(form.user) - 1);
    if (conn->password)
      strncpy(form.password, conn->password, sizeof(form.password) - 1);
    form.save_password = conn->save_password;
  }

  const char *drivers[] = {"sqlite", "postgres", "mysql", "mariadb"};
  int num_drivers = 4;
  int current_driver = 1; /* postgres default */

  /* Find current driver index */
  for (int i = 0; i < num_drivers; i++) {
    if (str_eq(form.driver, drivers[i])) {
      current_driver = i;
      break;
    }
  }

  /* Field definitions */
  enum {
    FLD_NAME,
    FLD_DRIVER,
    FLD_HOST,
    FLD_PORT,
    FLD_DATABASE,
    FLD_USER,
    FLD_PASSWORD,
    FLD_SAVE_PWD,
    FLD_SAVE_BTN,
    FLD_CANCEL_BTN,
    FLD_COUNT
  };
  int focus = FLD_NAME;
  /* The text fields edit the form's arrays in place */
  TextInput inputs[FLD_COUNT];
  memset(inputs, 0, sizeof(inputs));
  text_input_bind(&inputs[FLD_NAME], form.name, sizeof(form.name), false);
  text_input_bind(&inputs[FLD_HOST], form.host, sizeof(form.host), false);
  text_input_bind(&inputs[FLD_PORT], form.port, sizeof(form.port), false);
  text_input_bind(&inputs[FLD_DATABASE], form.database, sizeof(form.database),
                  false);
  text_input_bind(&inputs[FLD_USER], form.user, sizeof(form.user), false);
  text_input_bind(&inputs[FLD_PASSWORD], form.password, sizeof(form.password),
                  true);

  bool result = false;
  bool running = true;

  while (running) {
    werase(dlg);
    DRAW_BOX(dlg, COLOR_BORDER);

    const char *title = edit_item ? " Edit Connection " : " New Connection ";
    WITH_ATTR(dlg, A_BOLD,
              mvwprintw(dlg, 0, (dlg_width - (int)strlen(title)) / 2, "%s", title));

    int y = 2;
    int label_w = 12;
    int field_w = dlg_width - label_w - 4;
    int cursor_y = -1, cursor_x = -1;

/* Helper macro to draw a text field; remembers where the cursor goes */
#define DRAW_FIELD(fld, label)                                                 \
  do {                                                                         \
    mvwprintw(dlg, y, 2, "%s:", label);                                        \
    int cx_ = text_input_draw(                                                 \
        dlg, &inputs[fld], y, label_w + 2, field_w,                            \
        focus == fld ? COLOR_PAIR(COLOR_SELECTED) : A_NORMAL, focus == fld);   \
    if (focus == fld && cx_ >= 0) {                                            \
      cursor_y = y;                                                            \
      cursor_x = cx_;                                                          \
    }                                                                          \
    y++;                                                                       \
  } while (0)

    DRAW_FIELD(FLD_NAME, "Name");
    y++; /* Spacer */

    /* Driver selector */
    mvwprintw(dlg, y, 2, "Driver:");
    if (focus == FLD_DRIVER)
      wattron(dlg, COLOR_PAIR(COLOR_SELECTED));
    mvwprintw(dlg, y, label_w + 2, "< %s >", drivers[current_driver]);
    if (focus == FLD_DRIVER)
      wattroff(dlg, COLOR_PAIR(COLOR_SELECTED));
    y++;

    DRAW_FIELD(FLD_HOST, "Host");
    DRAW_FIELD(FLD_PORT, "Port");
    DRAW_FIELD(FLD_DATABASE, "Database");
    y++; /* Spacer */
    DRAW_FIELD(FLD_USER, "User");
    DRAW_FIELD(FLD_PASSWORD, "Password");

    /* Save password checkbox */
    mvwprintw(dlg, y, label_w + 2, "[%c] Save password",
              form.save_password ? 'X' : ' ');
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
    if (focus == FLD_SAVE_BTN)
      wattron(dlg, A_REVERSE);
    mvwprintw(dlg, y, btn_x, "[ Save ]");
    if (focus == FLD_SAVE_BTN)
      wattroff(dlg, A_REVERSE);
    if (focus == FLD_CANCEL_BTN)
      wattron(dlg, A_REVERSE);
    mvwprintw(dlg, y, btn_x + 10, "[ Cancel ]");
    if (focus == FLD_CANCEL_BTN)
      wattroff(dlg, A_REVERSE);

    /* Position cursor */
    if (cursor_x >= 0) {
      wmove(dlg, cursor_y, cursor_x);
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
      focus = FOCUS_NEXT(focus, FLD_COUNT);
    } else if (render_event_is_special(&event, UI_KEY_UP) ||
               render_event_is_special(&event, UI_KEY_BTAB)) {
      focus = FOCUS_PREV(focus, FLD_COUNT);
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (focus == FLD_CANCEL_BTN) {
        /* Cancel */
        running = false;
      } else if (focus == FLD_SAVE_PWD) {
        form.save_password = !form.save_password;
      } else if (strlen(form.name) > 0) {
        /* Create/update connection */
        SavedConnection *conn =
            edit_item ? &edit_item->connection : connmgr_new_connection();
        if (!conn) {
          *error = str_dup("Failed to create connection");
          running = false;
          continue;
        }

        /* Update fields */
        if (!edit_item) {
          /* New connection - need to allocate */
          free(conn->name);
          conn->name = str_dup(form.name);
          free(conn->driver);
          conn->driver = str_dup(drivers[current_driver]);
          free(conn->host);
          conn->host = str_dup(form.host);
          free(conn->database);
          conn->database = str_dup(form.database);
          free(conn->user);
          conn->user = str_dup(form.user);
          str_secure_free(conn->password);
          conn->password = str_dup(form.password);
        } else {
          /* Edit - update in place */
          free(conn->name);
          conn->name = str_dup(form.name);
          free(conn->driver);
          conn->driver = str_dup(drivers[current_driver]);
          free(conn->host);
          conn->host = str_dup(form.host);
          free(conn->database);
          conn->database = str_dup(form.database);
          free(conn->user);
          conn->user = str_dup(form.user);
          str_secure_free(conn->password);
          conn->password = str_dup(form.password);
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
        strncpy(form.driver, drivers[current_driver], sizeof(form.driver) - 1);
      } else if (render_event_is_special(&event, UI_KEY_RIGHT) ||
                 render_event_get_char(&event) == 'l') {
        current_driver = (current_driver + 1) % num_drivers;
        strncpy(form.driver, drivers[current_driver], sizeof(form.driver) - 1);
      }
    } else if (focus == FLD_SAVE_PWD) {
      if (render_event_get_char(&event) == ' ') {
        form.save_password = !form.save_password;
      }
    } else if (focus == FLD_SAVE_BTN || focus == FLD_CANCEL_BTN) {
      /* Button navigation with left/right */
      if (render_event_is_special(&event, UI_KEY_LEFT)) {
        if (focus == FLD_CANCEL_BTN)
          focus = FLD_SAVE_BTN;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (focus == FLD_SAVE_BTN)
          focus = FLD_CANCEL_BTN;
      }
    } else if (focus >= FLD_NAME && focus <= FLD_PASSWORD) {
      /* Text fields; the port takes digits only */
      int key_char = render_event_get_char(&event);
      if (!(focus == FLD_PORT && render_event_is_char(&event) &&
            (key_char < '0' || key_char > '9'))) {
        text_input_handle_key(&inputs[focus], &event);
      }
    }
  }

  curs_set(0);
  delwin(dlg);

  /* Securely zero password buffer before returning to prevent stack leakage */
  volatile char *vpass = form.password;
  for (size_t i = 0; i < sizeof(form.password); i++) {
    vpass[i] = 0;
  }

  return result;
}

