#include "ui/dialogs/connect.h"
#include "ui/conn_picker.h"
#include "ui/input.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * The picker overlay
 * ========================================================================== */

typedef struct { ConnPicker p; } PickerOv;

static void picker_layout(Overlay *o, NcApp *app) {
  o->cols = app->cols - 8 < 80 ? app->cols - 8 : 80;
  o->rows = app->rows - 6 < 22 ? app->rows - 6 : 22;
}

static void picker_draw(Overlay *o, NcApp *app) {
  PickerOv *po = o->data;
  int cy = 1, cx = 1;
  cp_draw(app, &po->p, o->plane, 1, 1, o->rows, o->cols, &cy, &cx);
  o->wants_cursor = po->p.input_focused;
  o->cursor_y = cy - 1;
  o->cursor_x = cx - 1;
}

static bool picker_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  PickerOv *po = o->data;
  if (ev->type == UI_EVENT_MOUSE) {
    if (list_mouse(&po->p.list, ev, 2, o->rows) == LIST_ACTIVATE) {
      po->p.input_focused = false;
      cp_perform(app, &po->p, CP_ROW, o);
    }
    return true;
  }
  CpResult r = cp_key(app, &po->p, ev, true);
  if (r.act != CP_NONE) {
    cp_perform(app, &po->p, r.act, o); /* may pop and free o */
    return true;
  }
  return !(ev->type == UI_EVENT_KEY && ev->key.is_special && ev->key.key == UI_KEY_ESCAPE);
}

static void picker_free(void *data) {
  PickerOv *po = data;
  cp_free(&po->p);
  free(po);
}

void connect_open(NcApp *app) {
  PickerOv *po = safe_calloc(1, sizeof *po);
  cp_init(app, &po->p, false);
  if (!list_selected(&po->p.list)) po->p.input_focused = true;
  Overlay *o = overlay_new("Connect");
  o->footer = "Enter connect  t tab  w workspace  n new  e edit  x delete  / URL  Esc close";
  o->data = po;
  o->free_data = picker_free;
  o->layout = picker_layout;
  o->draw = picker_draw;
  o->key = picker_key;
  overlay_push(app, o);
}

/* ==========================================================================
 * The connection form
 * ========================================================================== */

enum { F_NAME, F_DRIVER, F_FILE, F_HOST, F_PORT, F_DB, F_USER, F_PASS, F_SAVEPW, F_SSH, F_SSHHOST,
       F_SSHPORT, F_SSHUSER, F_SSHKEY, F_SSHPASS, F_SAVESSHPW, F_COUNT };
static const char *const labels[F_COUNT] = {
    "Name", "Driver", "File", "Host", "Port", "Database", "User", "Password", "Save password",
    "SSH tunnel", "SSH host", "SSH port", "SSH user", "SSH key", "SSH password", "Save SSH password"};
static const char *const drivers[] = {"sqlite", "postgres", "mysql", "mariadb"};
#define NUM_DRIVERS 4

static bool is_check(int f) { return f == F_SAVEPW || f == F_SSH || f == F_SAVESSHPW; }

typedef struct {
  TextInput in[F_COUNT];
  bool chk[F_COUNT];
  int driver, focus;
  char *custom;   /* a saved driver this build does not know: kept as a fifth choice */
  char *id;       /* saved id being edited; NULL: a new connection */
  char *message;
  bool message_error;
} Form;

static int num_drivers(const Form *f) { return f->custom ? NUM_DRIVERS + 1 : NUM_DRIVERS; }
static const char *driver_name(const Form *f, int i) { return i < NUM_DRIVERS ? drivers[i] : f->custom; }

static bool visible(const Form *f, int fld) {
  bool sqlite = f->driver == 0;
  switch (fld) {
  case F_NAME: case F_DRIVER: return true;
  case F_FILE: return sqlite;
  case F_HOST: case F_PORT: case F_DB: case F_USER: case F_PASS: case F_SAVEPW: case F_SSH: return !sqlite;
  default: return !sqlite && f->chk[F_SSH];
  }
}

static int next_field(const Form *f, int from, int dir) {
  int n = from;
  for (int i = 0; i < F_COUNT; i++) {
    n = (n + dir + F_COUNT) % F_COUNT;
    if (visible(f, n)) return n;
  }
  return from;
}

static size_t visible_count(const Form *f) {
  size_t n = 0;
  for (int i = 0; i < F_COUNT; i++) n += visible(f, i);
  return n;
}

static const char *str_or(const char *s) { return s ? s : ""; }

static void fill(Form *f, const LaceSavedConnection *c) {
  text_input_set(&f->in[F_NAME], str_or(c->name));
  const char *drv = c->driver && *c->driver ? lace_driver_normalize(c->driver) : "sqlite";
  bool known = false;
  for (int i = 0; i < NUM_DRIVERS; i++)
    if (drv && !strcmp(drv, drivers[i])) f->driver = i, known = true;
  if (!known) { /* show it rather than turn it into postgres */
    f->custom = str_dup(c->driver);
    f->driver = NUM_DRIVERS;
  }
  if (f->driver == 0) text_input_set(&f->in[F_FILE], str_or(c->database));
  else text_input_set(&f->in[F_DB], str_or(c->database));
  text_input_set(&f->in[F_HOST], str_or(c->host));
  if (c->port > 0) {
    char *p = str_printf("%d", c->port);
    text_input_set(&f->in[F_PORT], p);
    free(p);
  }
  text_input_set(&f->in[F_USER], str_or(c->user));
  f->chk[F_SAVEPW] = c->save_password;
  if (c->save_password) text_input_set(&f->in[F_PASS], str_or(c->password));
  f->chk[F_SSH] = c->ssh;
  text_input_set(&f->in[F_SSHHOST], str_or(c->ssh_host));
  if (c->ssh_port > 0) {
    char *p = str_printf("%d", c->ssh_port);
    text_input_set(&f->in[F_SSHPORT], p);
    free(p);
  }
  text_input_set(&f->in[F_SSHUSER], str_or(c->ssh_user));
  text_input_set(&f->in[F_SSHKEY], str_or(c->ssh_key));
  f->chk[F_SAVESSHPW] = c->save_ssh_password;
  if (c->save_ssh_password) text_input_set(&f->in[F_SSHPASS], str_or(c->ssh_password));
}

static const char *val(const Form *f, int fld) { return f->in[fld].buf ? f->in[fld].buf : ""; }

char *connect_check_port(const char *text, const char *what) {
  if (!text || !*text) return NULL;
  char *end;
  errno = 0;
  long v = strtol(text, &end, 10);
  if (errno || *end || v < 1 || v > 65535) return str_printf("%s: 1 to 65535, or empty for the default", what);
  return NULL;
}

char *connect_check_host(const char *text, const char *what) {
  for (const char *c = text; c && *c; c++)
    if (strchr("/?#@", *c) || isspace((unsigned char)*c))
      return str_printf("%s: no spaces or any of / ? # @", what);
  return NULL;
}

static void set_message(Form *f, bool error, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_message(Form *f, bool error, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  free(f->message);
  f->message = str_vprintf(fmt, ap);
  va_end(ap);
  f->message_error = error;
}

/* The connection the fields describe; NULL (and the message set) when they
 * do not validate. with_secrets: keep the typed passwords whatever the
 * "save" boxes say (a test connects with them). */
static LaceSavedConnection *collect(Form *f, bool with_secrets) {
  bool sqlite = f->driver == 0;
  char *err = NULL;
  if (sqlite && !*val(f, F_FILE)) { set_message(f, true, "File: a path is required"); return NULL; }
  if (!sqlite) {
    if ((err = connect_check_host(val(f, F_HOST), "Host")) || (err = connect_check_port(val(f, F_PORT), "Port")))
      goto bad;
    if (f->chk[F_SSH]) {
      if (!*val(f, F_SSHHOST)) { set_message(f, true, "SSH host is required"); return NULL; }
      if ((err = connect_check_host(val(f, F_SSHHOST), "SSH host")) ||
          (err = connect_check_port(val(f, F_SSHPORT), "SSH port")))
        goto bad;
    }
  }
  LaceSavedConnection *c = lace_saved_conn_new();
  free(c->id);
  c->id = NULL;
  c->driver = str_dup(driver_name(f, f->driver));
  c->database = str_dup(sqlite ? val(f, F_FILE) : val(f, F_DB));
  c->host = str_dup(sqlite ? "" : val(f, F_HOST));
  c->user = str_dup(sqlite ? "" : val(f, F_USER));
  c->port = sqlite || !*val(f, F_PORT) ? 0 : atoi(val(f, F_PORT));
  c->save_password = !sqlite && f->chk[F_SAVEPW];
  c->password = str_dup(!sqlite && (c->save_password || with_secrets) ? val(f, F_PASS) : "");
  c->ssh = !sqlite && f->chk[F_SSH];
  c->ssh_host = str_dup(c->ssh ? val(f, F_SSHHOST) : "");
  c->ssh_user = str_dup(c->ssh ? val(f, F_SSHUSER) : "");
  c->ssh_key = str_dup(c->ssh ? val(f, F_SSHKEY) : "");
  c->ssh_port = c->ssh && *val(f, F_SSHPORT) ? atoi(val(f, F_SSHPORT)) : 0;
  c->save_ssh_password = c->ssh && f->chk[F_SAVESSHPW];
  c->ssh_password = str_dup(c->ssh && (c->save_ssh_password || with_secrets) ? val(f, F_SSHPASS) : "");
  const char *name = val(f, F_NAME);
  if (*name) {
    c->name = str_dup(name);
  } else if (sqlite) {
    const char *slash = strrchr(c->database, '/');
    c->name = str_dup(slash && slash[1] ? slash + 1 : c->database);
  } else {
    c->name = str_printf("%s on %s", *c->database ? c->database : driver_name(f, f->driver),
                         *c->host ? c->host : "localhost");
  }
  return c;
bad:
  set_message(f, true, "%s", err);
  free(err);
  return NULL;
}

static void apply(LaceSavedConnection *dst, LaceSavedConnection *src) {
  char *id = dst->id;
  dst->id = NULL;
  connmgr_free_connection(dst);
  *dst = *src;
  dst->id = id;
  free(src);
}

static bool form_save(Form *f, NcApp *app) {
  LaceSavedConnection *c = collect(f, false);
  if (!c) return false;
  if (!app->connmgr) { lace_saved_conn_free(c); set_message(f, true, "connections.json is not available"); return false; }
  ConnectionItem *it = f->id ? connmgr_find_by_id(app->connmgr, f->id) : NULL;
  if (it && connmgr_is_connection(it)) {
    apply(&it->connection, c);
  } else {
    free(c->id);
    c->id = f->id ? str_dup(f->id) : lace_generate_uuid();
    if (!f->id) f->id = str_dup(c->id); /* a retry after a failed save edits this entry */
    if (!connmgr_add_connection(&app->connmgr->root, c)) {
      lace_saved_conn_free(c);
      set_message(f, true, "cannot add the connection");
      return false;
    }
  }
  char *err = NULL;
  if (!connmgr_save(app->connmgr, &err)) {
    set_message(f, true, "cannot save: %s", err ? err : "unknown error");
    free(err);
    return false;
  }
  cp_changed();
  return true;
}

static void test_done(NcApp *app, NcConnection *c, const char *error, void *data) {
  (void)app; (void)c; (void)error; (void)data;
}

static void form_test(Form *f, NcApp *app) {
  LaceSavedConnection *c = collect(f, true);
  if (!c) return;
  char *cs = lace_saved_conn_build_connstr(c, NULL, NULL);
  if (cs) app_test_connection(app, cs, c, test_done, NULL);
  else set_message(f, true, "cannot build the connection string");
  str_secure_free(cs);
  lace_saved_conn_free(c);
}

static void form_layout(Overlay *o, NcApp *app) {
  Form *f = o->data;
  o->cols = app->cols - 8 < 70 ? app->cols - 8 : 70;
  o->rows = (int)visible_count(f) + 2;
}

static bool form_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  Form *f = o->data;
  if (ev->type != UI_EVENT_KEY) return true;
  bool plain = !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT));
  if (ev->key.is_special && plain) {
    switch (ev->key.key) {
    case UI_KEY_ESCAPE: return false;
    case UI_KEY_TAB: case UI_KEY_DOWN: f->focus = next_field(f, f->focus, 1); return true;
    case UI_KEY_BTAB: case UI_KEY_UP: f->focus = next_field(f, f->focus, -1); return true;
    case UI_KEY_ENTER:
      if (form_save(f, app)) overlay_pop_this(app, o);
      return true;
    case UI_KEY_LEFT: case UI_KEY_RIGHT:
      if (f->focus == F_DRIVER) {
        f->driver = (f->driver + (ev->key.key == UI_KEY_RIGHT ? 1 : num_drivers(f) - 1)) % num_drivers(f);
        if (!visible(f, f->focus)) f->focus = F_DRIVER;
        overlay_layout_all(app);
        return true;
      }
      break;
    default: break;
    }
  }
  if (!ev->key.is_special && (ev->key.mods & UI_MOD_CTRL) && toupper(ev->key.key) == 'T') {
    form_test(f, app);
    return true;
  }
  if (is_check(f->focus)) {
    if (!ev->key.is_special && plain && ev->key.key == ' ') {
      f->chk[f->focus] = !f->chk[f->focus];
      overlay_layout_all(app);
    }
    return true;
  }
  if (f->focus == F_DRIVER) return true;
  nc_text_input_key(&f->in[f->focus], ev);
  return true;
}

static void form_draw(Overlay *o, NcApp *app) {
  Form *f = o->data;
  const Theme *t = &app->theme;
  struct ncplane *pl = o->plane;
  int lw = 18, fw = o->cols - lw - 3;
  int body = o->rows - 1; /* the last row is the message */
  int row_of_focus = 0, n = 0;
  for (int i = 0; i < F_COUNT; i++) {
    if (!visible(f, i)) continue;
    if (i == f->focus) row_of_focus = n;
    n++;
  }
  int first = row_of_focus >= body ? row_of_focus - body + 1 : 0;
  n = 0;
  o->wants_cursor = false;
  for (int i = 0; i < F_COUNT; i++) {
    if (!visible(f, i)) continue;
    int r = n++ - first;
    if (r < 0 || r >= body) continue;
    int y = 1 + r;
    bool foc = i == f->focus;
    nc_style(pl, t, foc ? TC_ACCENT : TC_OVERLAY_FG, TC_OVERLAY_BG, foc ? NCSTYLE_BOLD : 0);
    nc_put(pl, y, 2, lw, labels[i]);
    int x = 2 + lw;
    if (i == F_DRIVER) {
      char *d = str_printf("\xe2\x80\xb9 %s \xe2\x80\xba", driver_name(f, f->driver));
      nc_style(pl, t, TC_OVERLAY_FG, TC_EDIT_BG, 0);
      nc_put(pl, y, x, fw, d);
      free(d);
    } else if (is_check(i)) {
      nc_style(pl, t, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
      nc_put(pl, y, x, fw, f->chk[i] ? "[x]" : "[ ]");
    } else {
      int cx = x;
      if (!f->in[i].len && i == F_PORT) {
        char *d = str_printf("%d", lace_driver_default_port(driver_name(f, f->driver)));
        nc_style(pl, t, TC_DIM, TC_EDIT_BG, 0);
        nc_put(pl, y, x, fw, d);
        free(d);
      } else {
        nc_draw_input(pl, t, y, x, fw, &f->in[i], TC_OVERLAY_FG, TC_EDIT_BG, &cx);
      }
      if (foc) {
        o->wants_cursor = true;
        o->cursor_y = y - 1; /* content coordinates: the frame adds its origin plus one */
        o->cursor_x = cx - 1;
      }
    }
  }
  if (f->message) {
    nc_style(pl, t, f->message_error ? TC_ERROR_FG : TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
    nc_put(pl, o->rows, 2, o->cols - 2, f->message);
  }
}

static void form_free(void *data) {
  Form *f = data;
  for (int i = 0; i < F_COUNT; i++) text_input_free(&f->in[i]);
  free(f->id);
  free(f->custom);
  free(f->message);
  free(f);
}

void connect_form_open(NcApp *app, LaceSavedConnection *edit) {
  Form *f = safe_calloc(1, sizeof *f);
  for (int i = 0; i < F_COUNT; i++) text_input_init(&f->in[i], NULL, i == F_PASS || i == F_SSHPASS);
  f->driver = 1; /* a new connection is most often a server */
  f->focus = F_NAME;
  if (edit) {
    f->id = edit->id && *edit->id ? str_dup(edit->id) : NULL;
    fill(f, edit);
    lace_saved_conn_free(edit);
  }
  Overlay *o = overlay_new(f->id && app->connmgr && connmgr_find_by_id(app->connmgr, f->id) ? "Edit connection"
                                                                                          : "New connection");
  o->footer = "Tab next  Space toggle  \xe2\x86\x90\xe2\x86\x92 driver  Enter save  ^T test  Esc cancel";
  o->data = f;
  o->free_data = form_free;
  o->layout = form_layout;
  o->draw = form_draw;
  o->key = form_key;
  overlay_push(app, o);
}
