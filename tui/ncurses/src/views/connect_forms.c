/*
 * Lace
 * Workspaces window: the save and new/edit connection forms, drawn in the
 * details pane
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

static void collect_folders_recursive(ConnectionItem *item, FolderList *list,
                                      int depth) {
  if (item->type != CONN_ITEM_FOLDER || depth > 100)
    return;
  if (list->num_folders >= list->capacity) {
    size_t new_cap = list->capacity == 0 ? 8 : list->capacity * 2;
    list->folders =
        safe_reallocarray(list->folders, new_cap, sizeof(ConnectionItem *));
    list->capacity = new_cap;
  }
  list->folders[list->num_folders++] = item;
  for (size_t i = 0; i < item->folder.num_children; i++) {
    collect_folders_recursive(&item->folder.children[i], list, depth + 1);
  }
}

static FolderList collect_all_folders(ConnectionManager *mgr) {
  FolderList list = {NULL, 0, 0};
  collect_folders_recursive(&mgr->root, &list, 0);
  return list;
}

static void free_folder_list(FolderList *list) {
  free(list->folders);
  memset(list, 0, sizeof(*list));
}

/* "(root)", or the folder's name indented by its depth */
static char *folder_display_name(ConnectionItem *folder) {
  if (!folder->parent)
    return str_dup("(root)");
  int depth = connmgr_get_item_depth(folder);
  if (depth < 0 || depth > 100)
    depth = 0;
  return str_printf("%*s%s", depth * 2, "", connmgr_item_name(folder));
}

/* ============================================================================
 * Shared form drawing
 * ============================================================================
 */

static void draw_form_frame(WINDOW *win, int h, int w, const char *title) {
  werase(win);
  DRAW_BOX(win, COLOR_BORDER);
  char *t = str_printf(" %s ", title);
  int tw = utf8_width(t, strlen(t));
  if (tw > w - 4)
    tw = w - 4;
  WITH_ATTR(win, A_BOLD, tui_print_clip(win, 0, (w - tw) / 2, tw, t));
  free(t);
  (void)h;
}

/* A row of buttons, centered; selected is the focused one (-1: none) */
static void draw_button_row(WINDOW *win, int y, int w, const char *const *labels,
                            int count, int selected) {
  int x[8], bw[8];
  connect_button_layout(labels, count, w, 2, true, x, bw);
  for (int i = 0; i < count; i++) {
    char *b = str_printf("[ %s ]", labels[i]);
    if (i == selected)
      wattron(win, A_REVERSE | A_BOLD);
    tui_print_clip(win, y, x[i], w - 1 - x[i] > 0 ? w - 1 - x[i] : 0, b);
    if (i == selected)
      wattroff(win, A_REVERSE | A_BOLD);
    free(b);
  }
}

/* The button of a row drawn by draw_button_row at the mouse, or -1 */
static int button_clicked(WINDOW *win, int row_y, int w, const char *const *labels,
                          int count, const UiEvent *event) {
  int by, bx;
  getbegyx(win, by, bx);
  if (event->mouse.y - by != row_y)
    return -1;
  int x[8], bw[8];
  connect_button_layout(labels, count, w, 2, true, x, bw);
  return connect_button_at(x, bw, count, event->mouse.x - bx);
}

/* The form's one-line message: errors in the error color */
static void draw_form_message(WINDOW *win, int y, int w, const char *msg,
                              bool error) {
  if (!msg || !msg[0])
    return;
  char *flat = connect_flatten_message(msg);
  wattron(win, COLOR_PAIR(error ? COLOR_ERROR : COLOR_NUMBER));
  tui_print_clip(win, y, 2, w - 4, flat ? flat : msg);
  wattroff(win, COLOR_PAIR(error ? COLOR_ERROR : COLOR_NUMBER));
  free(flat);
}

/* ============================================================================
 * Save Connection Dialog
 * ============================================================================
 */

bool connect_save_dialog(DialogState *ds, const char *url, char **error) {
  ConnectionManager *mgr = ds->mgr;
  char *parse_err = NULL;
  SavedConnection *conn = connmgr_parse_connstr(url, &parse_err);
  if (!conn) {
    *error = parse_err ? parse_err : str_dup("Invalid connection URL");
    return false;
  }
  bool has_password = (conn->password && conn->password[0]) ||
                      (conn->ssh_password && conn->ssh_password[0]);

  FolderList folders = collect_all_folders(mgr);
  size_t selected_folder = 0;
  TextInput name;
  text_input_init(&name, conn->name, false);
  bool save_passwords = false; /* Never stored without being asked */

  /* Focus: 0 name, 1 folder, 2 save passwords (if any), then the buttons */
  enum { F_NAME, F_FOLDER, F_SAVEPW, F_SAVE, F_CANCEL, F_COUNT };
  int focus = F_NAME;
  char *msg = NULL;
  bool result = false, running = true;
  int h = 0, w = 0;
  WINDOW *dlg = NULL;

  while (running) {
    if (!dlg) {
      dlg = connect_pane_window(ds, 12, &h, &w);
      if (!dlg) {
        *error = str_dup("The terminal is too small for this dialog");
        break;
      }
    }
    draw_form_frame(dlg, h, w, "Save Connection");
    int fx = 14, fw = w - fx - 2;
    mvwprintw(dlg, 2, 2, "Name:");
    int cx = text_input_draw(dlg, &name, 2, fx, fw,
                             focus == F_NAME ? COLOR_PAIR(COLOR_SELECTED) : A_NORMAL,
                             focus == F_NAME);
    mvwprintw(dlg, 4, 2, "Folder:");
    char *folder_name = selected_folder < folders.num_folders
                            ? folder_display_name(folders.folders[selected_folder])
                            : str_dup("(root)");
    char *picker = str_printf("< %s >", folder_name);
    if (focus == F_FOLDER)
      wattron(dlg, COLOR_PAIR(COLOR_SELECTED));
    tui_print_fit(dlg, 4, fx, fw, picker);
    if (focus == F_FOLDER)
      wattroff(dlg, COLOR_PAIR(COLOR_SELECTED));
    free(picker);
    free(folder_name);
    if (has_password) {
      if (focus == F_SAVEPW)
        wattron(dlg, A_REVERSE);
      mvwprintw(dlg, 6, fx, "[%c] Save the password(s) too", save_passwords ? 'X' : ' ');
      if (focus == F_SAVEPW)
        wattroff(dlg, A_REVERSE);
    }
    draw_form_message(dlg, h - 4, w, msg, true);
    WITH_ATTR(dlg, COLOR_PAIR(COLOR_BORDER), mvwaddch(dlg, h - 3, 0, ACS_LTEE);
              mvwhline(dlg, h - 3, 1, ACS_HLINE, w - 2);
              mvwaddch(dlg, h - 3, w - 1, ACS_RTEE));
    const char *labels[] = {"Save", "Cancel"};
    draw_button_row(dlg, h - 2, w, labels, 2,
                    focus == F_SAVE ? 0 : focus == F_CANCEL ? 1 : -1);
    if (focus == F_NAME && cx >= 0) {
      wmove(dlg, 2, cx);
      curs_set(1);
    } else {
      curs_set(0);
    }
    wrefresh(dlg);

    int ch = wgetch(dlg);
    if (ch == KEY_RESIZE) {
      delwin(dlg);
      dlg = NULL;
      connect_redraw(ds);
      continue;
    }
    UiEvent event;
    render_translate_key(ch, &event);
    free(msg);
    msg = NULL;
    if (event.type == UI_EVENT_MOUSE && event.mouse.button == UI_MOUSE_LEFT) {
      /* A click focuses a field or presses a button (as Enter) */
      int by, bx;
      getbegyx(dlg, by, bx);
      (void)bx;
      int ry = event.mouse.y - by;
      int b = button_clicked(dlg, h - 2, w, labels, 2, &event);
      if (b >= 0) {
        focus = b == 0 ? F_SAVE : F_CANCEL;
        event.type = UI_EVENT_KEY;
        event.key.is_special = true;
        event.key.key = UI_KEY_ENTER;
        event.key.mods = UI_MOD_NONE;
      } else {
        if (ry == 2)
          focus = F_NAME;
        else if (ry == 4)
          focus = F_FOLDER;
        else if (ry == 6 && has_password) {
          focus = F_SAVEPW;
          save_passwords = !save_passwords;
        }
        continue;
      }
    }

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_DOWN)) {
      do {
        focus = FOCUS_NEXT(focus, F_COUNT);
      } while (focus == F_SAVEPW && !has_password);
    } else if (render_event_is_special(&event, UI_KEY_BTAB) ||
               render_event_is_special(&event, UI_KEY_UP)) {
      do {
        focus = FOCUS_PREV(focus, F_COUNT);
      } while (focus == F_SAVEPW && !has_password);
    } else if (focus == F_SAVEPW && (render_event_get_char(&event) == ' ' ||
                                     render_event_is_special(&event, UI_KEY_ENTER))) {
      save_passwords = !save_passwords;
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (focus == F_CANCEL) {
        running = false;
      } else if (name.len == 0) {
        msg = str_dup("Enter a name for the connection");
        focus = F_NAME;
      } else {
        free(conn->name);
        conn->name = str_dup(name.buf);
        conn->save_password = save_passwords && conn->password && conn->password[0];
        conn->save_ssh_password =
            save_passwords && conn->ssh_password && conn->ssh_password[0];
        ConnectionItem *target = selected_folder < folders.num_folders
                                     ? folders.folders[selected_folder]
                                     : &mgr->root;
        if (connmgr_add_connection(target, conn)) { /* Takes conn */
          conn = NULL;
          mgr->modified = true;
          result = true;
        } else {
          *error = str_dup("Failed to add connection");
        }
        running = false;
      }
    } else if (focus == F_NAME) {
      text_input_handle_key(&name, &event);
    } else if (focus == F_FOLDER && folders.num_folders > 0) {
      if (render_event_is_special(&event, UI_KEY_LEFT))
        selected_folder = (selected_folder + folders.num_folders - 1) % folders.num_folders;
      else if (render_event_is_special(&event, UI_KEY_RIGHT))
        selected_folder = (selected_folder + 1) % folders.num_folders;
    } else if (focus == F_SAVE && render_event_is_special(&event, UI_KEY_RIGHT)) {
      focus = F_CANCEL;
    } else if (focus == F_CANCEL && render_event_is_special(&event, UI_KEY_LEFT)) {
      focus = F_SAVE;
    }
  }

  curs_set(0);
  if (dlg)
    delwin(dlg);
  text_input_free(&name);
  free_folder_list(&folders);
  free(msg);
  if (conn) {
    connmgr_free_connection(conn);
    free(conn);
  }
  return result;
}

/* ============================================================================
 * New/Edit Connection Form
 * ============================================================================
 */

/* What can have focus, in screen order */
typedef enum {
  CF_NAME,
  CF_DRIVER,
  CF_FILE,
  CF_HOST,
  CF_PORT,
  CF_DATABASE,
  CF_USER,
  CF_PASSWORD,
  CF_SAVE_PW,
  CF_SSH,
  CF_SSH_HOST,
  CF_SSH_PORT,
  CF_SSH_USER,
  CF_SSH_KEY,
  CF_SSH_PASSWORD,
  CF_SAVE_SSH_PW,
  CF_BTN_TEST,
  CF_BTN_SAVE,
  CF_BTN_CANCEL,
  CF_COUNT
} FormField;

static const char *const k_labels[CF_COUNT] = {
    "Name",     "Driver",   "File",       "Host",     "Port",
    "Database", "User",     "Password",   "",         "",
    "SSH host", "SSH port", "SSH user",   "SSH key",  "SSH pass", "",
    "",         "",         ""};

typedef struct {
  TextInput in[CF_COUNT]; /* The text fields (others unused) */
  const char *drivers[6];
  int num_drivers;
  int driver;
  char *custom_driver; /* A saved driver name not in the list, kept */
  bool save_pw, ssh, save_ssh_pw;
} ConnForm;

static bool form_is_text(FormField f) {
  return f == CF_NAME || f == CF_FILE || f == CF_HOST || f == CF_PORT ||
         f == CF_DATABASE || f == CF_USER || f == CF_PASSWORD ||
         f == CF_SSH_HOST || f == CF_SSH_PORT || f == CF_SSH_USER ||
         f == CF_SSH_KEY || f == CF_SSH_PASSWORD;
}

static bool form_sqlite(const ConnForm *f) {
  return str_eq(f->drivers[f->driver], "sqlite");
}

/* Whether a field is shown for the current driver and SSH choice */
static bool form_visible(const ConnForm *f, FormField fld) {
  bool sqlite = form_sqlite(f);
  switch (fld) {
  case CF_FILE:
    return sqlite;
  case CF_HOST:
  case CF_PORT:
  case CF_DATABASE:
  case CF_USER:
  case CF_PASSWORD:
  case CF_SAVE_PW:
  case CF_SSH:
    return !sqlite;
  case CF_SSH_HOST:
  case CF_SSH_PORT:
  case CF_SSH_USER:
  case CF_SSH_KEY:
  case CF_SSH_PASSWORD:
  case CF_SAVE_SSH_PW:
    return !sqlite && f->ssh;
  default:
    return true;
  }
}

static void form_init(ConnForm *f, const SavedConnection *c) {
  memset(f, 0, sizeof(*f));
  static const char *const known[] = {"postgres", "mysql", "mariadb", "sqlite"};
  for (int i = 0; i < 4; i++)
    f->drivers[f->num_drivers++] = known[i];
  f->driver = 0;
  for (int i = 0; i < CF_COUNT; i++) {
    if (form_is_text((FormField)i))
      text_input_init(&f->in[i], NULL, i == CF_PASSWORD || i == CF_SSH_PASSWORD);
  }
  if (!c)
    return;
  const char *drv = c->driver ? c->driver : "";
  if (str_eq(drv, "postgresql") || str_eq(drv, "pg"))
    drv = "postgres";
  int found = -1;
  for (int i = 0; i < f->num_drivers; i++)
    if (str_eq(drv, f->drivers[i]))
      found = i;
  if (found < 0 && drv[0]) {
    f->custom_driver = str_dup(drv); /* Kept, not silently changed */
    f->drivers[f->num_drivers] = f->custom_driver;
    found = f->num_drivers++;
  }
  f->driver = found < 0 ? 0 : found;
  text_input_set(&f->in[CF_NAME], c->name);
  if (form_sqlite(f))
    text_input_set(&f->in[CF_FILE], c->database);
  else
    text_input_set(&f->in[CF_DATABASE], c->database);
  text_input_set(&f->in[CF_HOST], c->host);
  if (c->port > 0) {
    char p[16];
    snprintf(p, sizeof(p), "%d", c->port);
    text_input_set(&f->in[CF_PORT], p);
  }
  text_input_set(&f->in[CF_USER], c->user);
  text_input_set(&f->in[CF_PASSWORD], c->password);
  f->save_pw = c->save_password;
  f->ssh = c->ssh;
  text_input_set(&f->in[CF_SSH_HOST], c->ssh_host);
  if (c->ssh_port > 0) {
    char p[16];
    snprintf(p, sizeof(p), "%d", c->ssh_port);
    text_input_set(&f->in[CF_SSH_PORT], p);
  }
  text_input_set(&f->in[CF_SSH_USER], c->ssh_user);
  text_input_set(&f->in[CF_SSH_KEY], c->ssh_key);
  text_input_set(&f->in[CF_SSH_PASSWORD], c->ssh_password);
  f->save_ssh_pw = c->save_ssh_password;
}

static void form_free(ConnForm *f) {
  for (int i = 0; i < CF_COUNT; i++) {
    if (form_is_text((FormField)i))
      text_input_free(&f->in[i]); /* Owned buffers are wiped */
  }
  free(f->custom_driver);
}

/* A port field: empty (default) or 1..65535 */
static bool form_port(const TextInput *in, int *port) {
  *port = 0;
  if (in->len == 0)
    return true;
  long v = strtol(in->buf, NULL, 10);
  if (v < 1 || v > 65535)
    return false;
  *port = (int)v;
  return true;
}

/* A host may not hold what would change the URL's meaning */
static bool form_host_ok(const TextInput *in) {
  for (size_t i = 0; i < in->len; i++) {
    if (strchr("/?#@ \t", in->buf[i]))
      return false;
  }
  return true;
}

/* The form as a connection (fields replaced in *c); NULL, or why not */
static char *form_apply(const ConnForm *f, SavedConnection *c, bool need_name) {
  int port = 0, ssh_port = 0;
  if (need_name && f->in[CF_NAME].len == 0)
    return str_dup("Enter a name for the connection");
  bool sqlite = form_sqlite(f);
  if (sqlite) {
    if (f->in[CF_FILE].len == 0)
      return str_dup("Enter the database file");
  } else {
    if (!form_host_ok(&f->in[CF_HOST]))
      return str_dup("Host: a name or address only (no / ? # @ or spaces)");
    if (!form_port(&f->in[CF_PORT], &port))
      return str_dup("Port: 1 to 65535, or empty for the default");
    if (f->ssh) {
      if (f->in[CF_SSH_HOST].len == 0)
        return str_dup("Enter the SSH host");
      if (!form_host_ok(&f->in[CF_SSH_HOST]))
        return str_dup("SSH host: a name or address only (no / ? # @ or spaces)");
      if (!form_port(&f->in[CF_SSH_PORT], &ssh_port))
        return str_dup("SSH port: 1 to 65535, or empty for the default");
    }
  }
#define SET_STR(field, value)                                                  \
  do {                                                                         \
    free(c->field);                                                            \
    c->field = str_dup(value);                                                 \
  } while (0)
#define SET_SECRET(field, value)                                               \
  do {                                                                         \
    str_secure_free(c->field);                                                 \
    c->field = str_dup(value);                                                 \
  } while (0)
  SET_STR(name, f->in[CF_NAME].buf ? f->in[CF_NAME].buf : "");
  SET_STR(driver, f->drivers[f->driver]);
  /* Fields of the other kind of database are cleared: a user left from a
   * network driver would otherwise become part of a SQLite path */
  SET_STR(host, sqlite ? "" : (f->in[CF_HOST].buf ? f->in[CF_HOST].buf : ""));
  SET_STR(database, sqlite ? (f->in[CF_FILE].buf ? f->in[CF_FILE].buf : "")
                           : (f->in[CF_DATABASE].buf ? f->in[CF_DATABASE].buf : ""));
  SET_STR(user, sqlite ? "" : (f->in[CF_USER].buf ? f->in[CF_USER].buf : ""));
  SET_SECRET(password, sqlite ? "" : (f->in[CF_PASSWORD].buf ? f->in[CF_PASSWORD].buf : ""));
  c->port = sqlite ? 0 : port;
  c->save_password = !sqlite && f->save_pw;
  c->ssh = !sqlite && f->ssh;
  SET_STR(ssh_host, c->ssh && f->in[CF_SSH_HOST].buf ? f->in[CF_SSH_HOST].buf : "");
  SET_STR(ssh_user, c->ssh && f->in[CF_SSH_USER].buf ? f->in[CF_SSH_USER].buf : "");
  SET_STR(ssh_key, c->ssh && f->in[CF_SSH_KEY].buf ? f->in[CF_SSH_KEY].buf : "");
  SET_SECRET(ssh_password,
             c->ssh && f->in[CF_SSH_PASSWORD].buf ? f->in[CF_SSH_PASSWORD].buf : "");
  c->ssh_port = c->ssh ? ssh_port : 0;
  c->save_ssh_password = c->ssh && f->save_ssh_pw;
#undef SET_STR
#undef SET_SECRET
  return NULL;
}

static FormField form_step(const ConnForm *f, FormField from, int dir) {
  int x = (int)from;
  for (int n = 0; n < CF_COUNT; n++) {
    x = (x + dir + CF_COUNT) % CF_COUNT;
    if (form_visible(f, (FormField)x))
      return (FormField)x;
  }
  return from;
}

bool connect_connection_form(DialogState *ds, ConnectionItem *edit_item,
                             ConnectionItem *parent_folder, char **error) {
  ConnectionManager *mgr = ds->mgr;
  ConnForm form;
  form_init(&form, edit_item ? &edit_item->connection : NULL);
  FormField focus = CF_NAME;
  char *msg = NULL;
  bool msg_error = true;
  bool result = false, running = true;
  int scroll = 0; /* First field row shown, when they do not all fit */
  int h = 0, w = 0;
  WINDOW *dlg = NULL;
  const char *title = edit_item ? "Edit Connection" : "New Connection";

  while (running) {
    /* Rows: the visible fields, then message, separator, buttons */
    int rows = 0;
    for (int i = 0; i < CF_BTN_TEST; i++)
      rows += form_visible(&form, (FormField)i);
    /* Sized to the fields shown: the driver and the SSH box change them */
    int want_h = rows + 6;
    if (dlg) {
      if (h != connect_pane_height(ds, want_h)) {
        delwin(dlg);
        dlg = NULL;
        connect_redraw(ds);
      }
    }
    if (!dlg) {
      dlg = connect_pane_window(ds, want_h, &h, &w);
      if (!dlg) {
        *error = str_dup("The terminal is too small for this dialog");
        break;
      }
    }
    int area = h - 6; /* Field rows that fit between title and message */
    if (area < 1)
      area = 1;

    /* Keep the focused field on screen */
    int focus_row = 0;
    for (int i = 0; i < (int)focus && i < CF_BTN_TEST; i++)
      focus_row += form_visible(&form, (FormField)i);
    if (focus < CF_BTN_TEST) {
      if (focus_row < scroll)
        scroll = focus_row;
      else if (focus_row >= scroll + area)
        scroll = focus_row - area + 1;
    }
    if (scroll > rows - area)
      scroll = rows - area > 0 ? rows - area : 0;

    draw_form_frame(dlg, h, w, title);
    int lx = 2, fx = 13, fw = w - fx - 2;
    int cy = -1, cx = -1, row = 0;
    for (int i = 0; i < CF_BTN_TEST; i++) {
      FormField fld = (FormField)i;
      if (!form_visible(&form, fld))
        continue;
      int y = 2 + row - scroll;
      row++;
      if (y < 2 || y >= 2 + area)
        continue;
      bool on = focus == fld;
      attr_t sel = on ? COLOR_PAIR(COLOR_SELECTED) : A_NORMAL;
      if (k_labels[fld][0]) {
        char *l = str_printf("%s:", k_labels[fld]);
        tui_print_clip(dlg, y, lx, fx - lx - 1, l);
        free(l);
      }
      if (form_is_text(fld)) {
        int c = text_input_draw(dlg, &form.in[fld], y, fx, fw, sel, on);
        if (on && c >= 0) {
          cy = y;
          cx = c;
        }
      } else if (fld == CF_DRIVER) {
        char *d = str_printf("< %s >", form.drivers[form.driver]);
        wattron(dlg, sel);
        tui_print_clip(dlg, y, fx, fw, d);
        wattroff(dlg, sel);
        free(d);
      } else {
        bool v = fld == CF_SAVE_PW ? form.save_pw
                 : fld == CF_SSH   ? form.ssh
                                   : form.save_ssh_pw;
        const char *what = fld == CF_SAVE_PW ? "Save password"
                           : fld == CF_SSH   ? "Through an SSH tunnel"
                                             : "Save SSH password";
        char *t = str_printf("[%c] %s", v ? 'X' : ' ', what);
        if (on)
          wattron(dlg, A_REVERSE);
        tui_print_clip(dlg, y, fx, fw, t);
        if (on)
          wattroff(dlg, A_REVERSE);
        free(t);
      }
    }
    if (rows > area) { /* More fields than room: say so at the edges */
      if (scroll > 0)
        mvwaddch(dlg, 2, w - 2, ACS_UARROW);
      if (scroll + area < rows)
        mvwaddch(dlg, 1 + area, w - 2, ACS_DARROW);
    }
    draw_form_message(dlg, h - 4, w, msg, msg_error);
    WITH_ATTR(dlg, COLOR_PAIR(COLOR_BORDER), mvwaddch(dlg, h - 3, 0, ACS_LTEE);
              mvwhline(dlg, h - 3, 1, ACS_HLINE, w - 2);
              mvwaddch(dlg, h - 3, w - 1, ACS_RTEE));
    const char *labels[] = {"Test", "Save", "Cancel"};
    draw_button_row(dlg, h - 2, w, labels, 3,
                    focus >= CF_BTN_TEST ? (int)focus - CF_BTN_TEST : -1);
    if (cx >= 0) {
      wmove(dlg, cy, cx);
      curs_set(1);
    } else {
      curs_set(0);
    }
    wrefresh(dlg);

    int ch = wgetch(dlg);
    if (ch == KEY_RESIZE) {
      delwin(dlg);
      dlg = NULL;
      connect_redraw(ds);
      continue;
    }
    UiEvent event;
    render_translate_key(ch, &event);
    free(msg);
    msg = NULL;
    msg_error = true;
    if (event.type == UI_EVENT_MOUSE) {
      if (event.mouse.button == UI_MOUSE_SCROLL_UP)
        focus = form_step(&form, focus, -1);
      else if (event.mouse.button == UI_MOUSE_SCROLL_DOWN)
        focus = form_step(&form, focus, 1);
      if (event.mouse.button != UI_MOUSE_LEFT)
        continue;
      int b = button_clicked(dlg, h - 2, w, labels, 3, &event);
      if (b >= 0) {
        /* A button press is Enter on it */
        focus = (FormField)(CF_BTN_TEST + b);
        event.type = UI_EVENT_KEY;
        event.key.is_special = true;
        event.key.key = UI_KEY_ENTER;
        event.key.mods = UI_MOD_NONE;
      } else {
        /* A field row: focus it; a checkbox also toggles; the driver cycles */
        int by, bx;
        getbegyx(dlg, by, bx);
        (void)bx;
        int ry = event.mouse.y - by, r = 0;
        for (int i = 0; i < CF_BTN_TEST; i++) {
          if (!form_visible(&form, (FormField)i))
            continue;
          if (2 + r - scroll == ry) {
            focus = (FormField)i;
            if (focus == CF_SAVE_PW)
              form.save_pw = !form.save_pw;
            else if (focus == CF_SSH)
              form.ssh = !form.ssh;
            else if (focus == CF_SAVE_SSH_PW)
              form.save_ssh_pw = !form.save_ssh_pw;
            break;
          }
          r++;
        }
        continue;
      }
    }
    int key = render_event_get_char(&event);
    bool enter = render_event_is_special(&event, UI_KEY_ENTER);
    bool test_key = ds->config && hotkey_matches(ds->config, &event, HOTKEY_CONN_TEST);

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_DOWN)) {
      focus = form_step(&form, focus, 1);
    } else if (render_event_is_special(&event, UI_KEY_BTAB) ||
               render_event_is_special(&event, UI_KEY_UP)) {
      focus = form_step(&form, focus, -1);
    } else if ((focus == CF_SAVE_PW || focus == CF_SSH || focus == CF_SAVE_SSH_PW) &&
               key == ' ') { /* Space toggles; Enter saves, as everywhere */
      bool *v = focus == CF_SAVE_PW ? &form.save_pw
                : focus == CF_SSH   ? &form.ssh
                                    : &form.save_ssh_pw;
      *v = !*v;
    } else if (focus == CF_DRIVER &&
               (render_event_is_special(&event, UI_KEY_LEFT) ||
                render_event_is_special(&event, UI_KEY_RIGHT) || key == ' ')) {
      /* The file and the database name are one value in another place */
      bool was_sqlite = form_sqlite(&form);
      int dir = render_event_is_special(&event, UI_KEY_LEFT) ? -1 : 1;
      form.driver = (form.driver + dir + form.num_drivers) % form.num_drivers;
      if (was_sqlite != form_sqlite(&form)) {
        FormField from = was_sqlite ? CF_FILE : CF_DATABASE;
        FormField to = was_sqlite ? CF_DATABASE : CF_FILE;
        if (form.in[to].len == 0)
          text_input_set(&form.in[to], form.in[from].buf);
      }
    } else if ((focus == CF_BTN_TEST && enter) || test_key) {
      SavedConnection *probe = connmgr_new_connection();
      char *why = form_apply(&form, probe, false);
      if (why) {
        msg = why;
      } else {
        connect_redraw(ds);
        bool ok = connect_test_saved(ds, probe, &msg);
        msg_error = !ok;
      }
      connmgr_free_connection(probe);
      free(probe);
    } else if (focus == CF_BTN_CANCEL && enter) {
      running = false;
    } else if (enter) {
      /* Enter anywhere else saves */
      SavedConnection *conn = edit_item ? &edit_item->connection : connmgr_new_connection();
      char *why = form_apply(&form, conn, true);
      if (why) {
        msg = why;
        if (!edit_item) {
          connmgr_free_connection(conn);
          free(conn);
        }
      } else if (edit_item) {
        mgr->modified = true;
        result = true;
        running = false;
      } else {
        ConnectionItem *target = parent_folder ? parent_folder : &mgr->root;
        if (connmgr_add_connection(target, conn)) { /* Takes conn */
          mgr->modified = true;
          result = true;
        } else {
          *error = str_dup("Failed to add connection");
          connmgr_free_connection(conn);
          free(conn);
        }
        running = false;
      }
    } else if (focus >= CF_BTN_TEST) {
      if (render_event_is_special(&event, UI_KEY_LEFT) && focus > CF_BTN_TEST)
        focus = (FormField)(focus - 1);
      else if (render_event_is_special(&event, UI_KEY_RIGHT) && focus < CF_BTN_CANCEL)
        focus = (FormField)(focus + 1);
    } else if (form_is_text(focus)) {
      bool port = focus == CF_PORT || focus == CF_SSH_PORT;
      bool digit_ok = !render_event_is_char(&event) ||
                      (key >= '0' && key <= '9' && form.in[focus].len < 5);
      if (!port || digit_ok)
        text_input_handle_key(&form.in[focus], &event);
    }
  }

  curs_set(0);
  if (dlg)
    delwin(dlg);
  form_free(&form);
  free(msg);
  return result;
}
