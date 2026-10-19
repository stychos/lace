#include "ui/conn_form.h"
#include "app.h"
#include "util/mem.h"
#include "util/str.h"
#include <ctype.h>
#include <errno.h>
#include <lace.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

const char *const conn_fields_labels[CF_COUNT] = {
    "Name", "Driver", "File", "Host", "Port", "Database", "User", "Password", "Save password",
    "SSH tunnel", "SSH host", "SSH port", "SSH user", "SSH key", "SSH password", "Save SSH password"};
static const char *const drivers[CF_NUM_DRIVERS] = {"sqlite", "postgres", "mysql", "mariadb"};

int conn_fields_num_drivers(const ConnFields *f) { return f->custom ? CF_NUM_DRIVERS + 1 : CF_NUM_DRIVERS; }
const char *conn_fields_driver_name(const ConnFields *f, int i) {
  return i < CF_NUM_DRIVERS ? drivers[i] : f->custom ? f->custom : "";
}
bool conn_fields_is_check(int fld) { return fld == CF_SAVEPW || fld == CF_SSH || fld == CF_SAVESSHPW; }

bool conn_fields_visible(const ConnFields *f, int fld) {
  bool sqlite = f->driver == 0;
  switch (fld) {
  case CF_NAME: case CF_DRIVER: return true;
  case CF_FILE: return sqlite;
  case CF_HOST: case CF_PORT: case CF_DB: case CF_USER: case CF_PASS: case CF_SAVEPW: case CF_SSH: return !sqlite;
  default: return !sqlite && f->chk[CF_SSH];
  }
}

int conn_fields_next(const ConnFields *f, int from, int dir) {
  int n = from;
  for (int i = 0; i < CF_COUNT; i++) {
    n = (n + dir + CF_COUNT) % CF_COUNT;
    if (conn_fields_visible(f, n)) return n;
  }
  return from;
}

static const char *val(const ConnFields *f, int fld) { return f->in[fld].buf ? f->in[fld].buf : ""; }

/* Every value in one string: drivers, boxes and inputs (passwords too) */
static char *snapshot(const ConnFields *f) {
  char *s = str_printf("%d\x1f%s", f->driver, f->custom ? f->custom : "");
  for (int i = 0; i < CF_COUNT; i++) {
    char *n = str_printf("%s\x1f%d%s", s, f->chk[i], val(f, i));
    str_secure_free(s);
    s = n;
  }
  return s;
}

void conn_fields_mark_clean(ConnFields *f) {
  str_secure_free(f->baseline);
  f->baseline = snapshot(f);
}

bool conn_fields_dirty(const ConnFields *f) {
  char *now = snapshot(f);
  bool dirty = !f->baseline || strcmp(now, f->baseline) != 0;
  str_secure_free(now);
  return dirty;
}

void conn_fields_set_message(ConnFields *f, bool error, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  free(f->message);
  f->message = str_vprintf(fmt, ap);
  va_end(ap);
  f->message_error = error;
}

static void reset(ConnFields *f) {
  for (int i = 0; i < CF_COUNT; i++) {
    text_input_set(&f->in[i], "");
    f->chk[i] = false;
  }
  free(f->custom);
  f->custom = NULL;
  f->driver = 1; /* a new connection is most often a server */
  free(f->message);
  f->message = NULL;
}

void conn_fields_init(ConnFields *f) {
  memset(f, 0, sizeof *f);
  for (int i = 0; i < CF_COUNT; i++) text_input_init(&f->in[i], NULL, i == CF_PASS || i == CF_SSHPASS);
  reset(f);
  conn_fields_mark_clean(f);
}

void conn_fields_free(ConnFields *f) {
  for (int i = 0; i < CF_COUNT; i++) text_input_free(&f->in[i]);
  free(f->custom);
  str_secure_free(f->baseline);
  free(f->message);
  memset(f, 0, sizeof *f);
}

void conn_fields_clear(ConnFields *f) {
  reset(f);
  conn_fields_mark_clean(f);
}

static const char *str_or(const char *s) { return s ? s : ""; }

static void set_port(TextInput *in, int port) {
  if (port <= 0) {
    text_input_set(in, "");
    return;
  }
  char *p = str_printf("%d", port);
  text_input_set(in, p);
  free(p);
}

/* Every field but the message from c; passwords: the saved ones only, or all */
static void load(ConnFields *f, const LaceSavedConnection *c, bool all_passwords) {
  char *keep = f->message;
  f->message = NULL;
  reset(f);
  f->message = keep;
  text_input_set(&f->in[CF_NAME], str_or(c->name));
  const char *drv = c->driver && *c->driver ? lace_driver_normalize(c->driver) : "sqlite";
  bool known = false;
  for (int i = 0; i < CF_NUM_DRIVERS; i++)
    if (drv && !strcmp(drv, drivers[i])) f->driver = i, known = true;
  if (!known) { /* shown rather than turned into postgres */
    f->custom = str_dup(c->driver);
    f->driver = CF_NUM_DRIVERS;
  }
  text_input_set(&f->in[f->driver == 0 ? CF_FILE : CF_DB], str_or(c->database));
  text_input_set(&f->in[CF_HOST], str_or(c->host));
  set_port(&f->in[CF_PORT], c->port);
  text_input_set(&f->in[CF_USER], str_or(c->user));
  f->chk[CF_SAVEPW] = c->save_password;
  if (c->save_password || all_passwords) text_input_set(&f->in[CF_PASS], str_or(c->password));
  f->chk[CF_SSH] = c->ssh;
  text_input_set(&f->in[CF_SSHHOST], str_or(c->ssh_host));
  set_port(&f->in[CF_SSHPORT], c->ssh_port);
  text_input_set(&f->in[CF_SSHUSER], str_or(c->ssh_user));
  text_input_set(&f->in[CF_SSHKEY], str_or(c->ssh_key));
  f->chk[CF_SAVESSHPW] = c->save_ssh_password;
  if (c->save_ssh_password || all_passwords) text_input_set(&f->in[CF_SSHPASS], str_or(c->ssh_password));
}

void conn_fields_fill(ConnFields *f, const LaceSavedConnection *c) {
  load(f, c, false);
  free(f->message);
  f->message = NULL;
  conn_fields_mark_clean(f);
}

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

/* The fields as a saved connection, unchecked; passwords: typed ones always
 * (with_secrets) or only where the save box is on */
static LaceSavedConnection *model_of(const ConnFields *f, bool with_secrets) {
  bool sqlite = f->driver == 0;
  LaceSavedConnection *c = lace_saved_conn_new();
  free(c->id);
  c->id = NULL;
  c->driver = str_dup(conn_fields_driver_name(f, f->driver));
  c->database = str_dup(sqlite ? val(f, CF_FILE) : val(f, CF_DB));
  c->host = str_dup(sqlite ? "" : val(f, CF_HOST));
  c->user = str_dup(sqlite ? "" : val(f, CF_USER));
  c->port = sqlite || !*val(f, CF_PORT) ? 0 : atoi(val(f, CF_PORT));
  c->save_password = !sqlite && f->chk[CF_SAVEPW];
  c->password = str_dup(!sqlite && (c->save_password || with_secrets) ? val(f, CF_PASS) : "");
  c->ssh = !sqlite && f->chk[CF_SSH];
  c->ssh_host = str_dup(c->ssh ? val(f, CF_SSHHOST) : "");
  c->ssh_user = str_dup(c->ssh ? val(f, CF_SSHUSER) : "");
  c->ssh_key = str_dup(c->ssh ? val(f, CF_SSHKEY) : "");
  c->ssh_port = c->ssh && *val(f, CF_SSHPORT) ? atoi(val(f, CF_SSHPORT)) : 0;
  c->save_ssh_password = c->ssh && f->chk[CF_SAVESSHPW];
  c->ssh_password = str_dup(c->ssh && (c->save_ssh_password || with_secrets) ? val(f, CF_SSHPASS) : "");
  const char *name = val(f, CF_NAME);
  if (*name) {
    c->name = str_dup(name);
  } else if (sqlite) {
    const char *slash = strrchr(c->database, '/');
    c->name = str_dup(slash && slash[1] ? slash + 1 : c->database);
  } else {
    c->name = str_printf("%s on %s", *c->database ? c->database : conn_fields_driver_name(f, f->driver),
                         *c->host ? c->host : "localhost");
  }
  return c;
}

/* The validation error of the fields, or NULL */
static char *check(const ConnFields *f) {
  char *err = NULL;
  if (f->driver == 0) return *val(f, CF_FILE) ? NULL : str_dup("File: a path is required");
  if ((err = connect_check_host(val(f, CF_HOST), "Host")) || (err = connect_check_port(val(f, CF_PORT), "Port")))
    return err;
  if (f->chk[CF_SSH]) {
    if (!*val(f, CF_SSHHOST)) return str_dup("SSH host is required");
    if ((err = connect_check_host(val(f, CF_SSHHOST), "SSH host")) ||
        (err = connect_check_port(val(f, CF_SSHPORT), "SSH port")))
      return err;
  }
  return NULL;
}

LaceSavedConnection *conn_fields_collect(ConnFields *f, bool with_secrets) {
  char *err = check(f);
  if (err) {
    conn_fields_set_message(f, true, "%s", err);
    free(err);
    return NULL;
  }
  return model_of(f, with_secrets);
}

/* The string with the given passwords (NULL: none) */
static char *build(const ConnFields *f, const char *pw, const char *ssh_pw) {
  LaceSavedConnection *m = model_of(f, false);
  m->save_password = m->save_ssh_password = false; /* only what is passed goes in */
  char *cs = lace_saved_conn_build_connstr(m, pw && *pw ? pw : NULL, ssh_pw && *ssh_pw ? ssh_pw : NULL);
  lace_saved_conn_free(m);
  return cs;
}

char *conn_fields_connstr(ConnFields *f) {
  char *err = check(f);
  if (err) {
    conn_fields_set_message(f, true, "%s", err);
    free(err);
    return NULL;
  }
  bool sqlite = f->driver == 0;
  char *cs = build(f, sqlite ? NULL : val(f, CF_PASS), f->chk[CF_SSH] && !sqlite ? val(f, CF_SSHPASS) : NULL);
  if (!cs) conn_fields_set_message(f, true, "Cannot build the connection string");
  return cs;
}

/* Replace every occurrence of from by to (allocated) */
static char *replace_all(const char *s, const char *from, const char *to) {
  size_t fl = strlen(from), tl = strlen(to), n = 0;
  for (const char *p = strstr(s, from); p; p = strstr(p + fl, from)) n++;
  char *out = safe_malloc(strlen(s) + n * tl + 1), *o = out;
  for (const char *p = s;;) {
    const char *q = strstr(p, from);
    if (!q) {
      strcpy(o, p);
      break;
    }
    memcpy(o, p, (size_t)(q - p));
    o += q - p;
    memcpy(o, to, tl);
    o += tl;
    p = q + fl;
  }
  return out;
}

char *conn_fields_uri(const ConnFields *f) {
  char *err = check(f);
  if (err) {
    free(err);
    return str_dup("");
  }
  bool sqlite = f->driver == 0;
  const char *pw = !sqlite && *val(f, CF_PASS) ? CF_PASSWORD_MASK : NULL;
  const char *spw = !sqlite && f->chk[CF_SSH] && *val(f, CF_SSHPASS) ? CF_PASSWORD_MASK : NULL;
  char *cs = build(f, pw, spw);
  if (!cs) return str_dup("");
  /* the bullets come back percent-encoded */
  char *a = replace_all(cs, "%E2%80%A2", "\xe2\x80\xa2");
  char *b = replace_all(a, "%e2%80%a2", "\xe2\x80\xa2");
  free(a);
  free(cs);
  return b;
}

bool conn_fields_set_uri(ConnFields *f, const char *text) {
  char *err = NULL;
  char *cs = app_target_to_connstr(text, &err);
  LaceSavedConnection *m = cs ? lace_saved_conn_from_connstr(cs, &err) : NULL;
  str_secure_free(cs);
  if (!m) {
    conn_fields_set_message(f, true, "%s", err ? err : "Not a connection URL or an existing SQLite file");
    free(err);
    return false;
  }
  free(err);
  char *name = str_dup(val(f, CF_NAME));
  char *pw = str_dup(val(f, CF_PASS)), *spw = str_dup(val(f, CF_SSHPASS));
  load(f, m, true);
  text_input_set(&f->in[CF_NAME], name);
  if (!strcmp(val(f, CF_PASS), CF_PASSWORD_MASK)) text_input_set(&f->in[CF_PASS], pw);
  if (!strcmp(val(f, CF_SSHPASS), CF_PASSWORD_MASK)) text_input_set(&f->in[CF_SSHPASS], spw);
  free(name);
  str_secure_free(pw);
  str_secure_free(spw);
  lace_saved_conn_free(m);
  free(f->message);
  f->message = NULL;
  return true;
}
