/* Preferences: the settings that matter, changed in place and saved at once */
#include "config/config.h"
#include "ui/dialogs/help.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "ui/panels.h"
#include "ui/screen.h"
#include "util/mem.h"
#include "util/str.h"
#include <cjson/cJSON.h>
#include <notcurses/notcurses.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum { P_BOOL, P_ENUM, P_NUM, P_SECT, P_OPEN } Kind; /* P_SECT: a bool in the notcurses section;
                                                        * P_OPEN: a row that opens another dialog */

typedef struct {
  const char *name;
  Kind kind;
  void *(*field)(Config *c); /* the setting inside general (not for P_OSC52) */
  const char *const *names;  /* P_ENUM */
  int count;
  const int *values;         /* P_NUM: the steps to cycle through */
  int num_values;
  const char *unit;
  const char *key;           /* P_SECT: the key in the notcurses section */
  bool dflt;
} Pref;

static void *f_delete(Config *c) { return &c->general.delete_confirmation; }
static void *f_quit(Config *c) { return &c->general.quit_confirmation; }
static void *f_session(Config *c) { return &c->general.restore_session; }
static void *f_cursor(Config *c) { return &c->general.restore_cursor_position; }
static void *f_approx(Config *c) { return &c->general.approximate_counts; }
static void *f_rownum(Config *c) { return &c->general.row_numbers; }
static void *f_scheme(Config *c) { return &c->general.color_scheme; }
static void *f_hist(Config *c) { return &c->general.history_mode; }
static void *f_histsize(Config *c) { return &c->general.history_max_size; }
static void *f_daemon(Config *c) { return &c->general.daemon_spawn_mode; }
static void *f_idle(Config *c) { return &c->general.daemon_idle_timeout; }

static const char *const ROWNUM[] = {"never", "auto", "always"};
static const char *const SCHEME[] = {"system", "light", "dark"};
static const char *const HIST[] = {"off", "session", "persistent"};
static const char *const DAEMON[] = {"unix", "stdio", "tcp", "detect"};
static const int HIST_SIZES[] = {50, 100, 500, 1000, 5000, 10000};
static const int IDLE[] = {0, 60, 300, 900, 3600};

static const Pref PREFS[] = {
    {.name = "Confirm deletes", .kind = P_BOOL, .field = f_delete},
    {.name = "Confirm quit", .kind = P_BOOL, .field = f_quit},
    {.name = "Restore session", .kind = P_BOOL, .field = f_session},
    {.name = "Restore cursor position", .kind = P_BOOL, .field = f_cursor},
    {.name = "Approximate counts", .kind = P_BOOL, .field = f_approx},
    {.name = "Row numbers", .kind = P_ENUM, .field = f_rownum, .names = ROWNUM, .count = 3},
    {.name = "Color scheme", .kind = P_ENUM, .field = f_scheme, .names = SCHEME, .count = 3},
    {.name = "History", .kind = P_ENUM, .field = f_hist, .names = HIST, .count = 3},
    {.name = "History size", .kind = P_NUM, .field = f_histsize, .values = HIST_SIZES, .num_values = 6, .unit = ""},
    {.name = "Daemon", .kind = P_ENUM, .field = f_daemon, .names = DAEMON, .count = 4},
    {.name = "Daemon idle timeout", .kind = P_NUM, .field = f_idle, .values = IDLE, .num_values = 5, .unit = "s"},
    {.name = "Tabs strip", .kind = P_SECT, .key = "tabs_strip", .dflt = true},
    {.name = "Filters strip", .kind = P_SECT, .key = "filters_strip", .dflt = true},
    {.name = "Clipboard OSC 52", .kind = P_SECT, .key = "clipboard_osc52", .dflt = true},
    {.name = "Hotkeys\xe2\x80\xa6", .kind = P_OPEN},
};
#define NPREFS ((int)(sizeof PREFS / sizeof *PREFS))
#define NAME_COLS 25

bool prefs_section_bool(const Config *c, const char *key, bool dflt) {
  const cJSON *sec = c && c->extra ? cJSON_GetObjectItem(c->extra, "notcurses") : NULL;
  const cJSON *it = sec ? cJSON_GetObjectItem(sec, key) : NULL;
  return cJSON_IsBool(it) ? cJSON_IsTrue(it) : dflt;
}

static char *value_text(Config *c, const Pref *p) {
  switch (p->kind) {
  case P_BOOL: return str_dup(*(bool *)p->field(c) ? "on" : "off");
  case P_ENUM: {
    int v = *(int *)p->field(c);
    return str_dup(v >= 0 && v < p->count ? p->names[v] : "?");
  }
  case P_NUM: {
    int v = *(int *)p->field(c);
    return str_printf("%d%s", v, p->unit);
  }
  case P_SECT: return str_dup(prefs_section_bool(c, p->key, p->dflt) ? "on" : "off");
  case P_OPEN: break;
  }
  return str_dup("");
}

static void fill(ListWidget *l, Config *c) {
  ListRow *rows = safe_calloc(NPREFS, sizeof *rows);
  for (int i = 0; i < NPREFS; i++) {
    char *v = value_text(c, &PREFS[i]);
    rows[i].text = str_printf("%-*s%s", NAME_COLS, PREFS[i].name, v);
    free(v);
    rows[i].kind = i;
  }
  list_set_rows(l, rows, (size_t)NPREFS);
}

/* Change one setting and save the config: the one path for the dialog and the
 * palette */
static bool apply(NcApp *app, const Pref *p, int dir) {
  Config *c = app->config;
  if (p->kind == P_OPEN) { /* the hotkeys editor, which saves its own changes */
    help_open(app);
    return true;
  }
  switch (p->kind) {
  case P_BOOL: {
    bool *b = p->field(c);
    *b = !*b;
    break;
  }
  case P_ENUM: {
    int *v = p->field(c);
    *v = (*v + dir + p->count) % p->count;
    break;
  }
  case P_NUM: {
    int *v = p->field(c);
    int at = 0;
    for (int i = 0; i < p->num_values; i++)
      if (p->values[i] <= *v) at = i;
    *v = p->values[(at + dir + p->num_values) % p->num_values];
    break;
  }
  case P_SECT: {
    cJSON *sec = config_section(c, "notcurses");
    bool now = !prefs_section_bool(c, p->key, p->dflt);
    cJSON_DeleteItemFromObject(sec, p->key);
    cJSON_AddBoolToObject(sec, p->key, now);
    break;
  }
  case P_OPEN: break;
  }
  if (p->field == f_scheme) screen_apply_theme(app);
  char *err = NULL;
  if (!config_save(c, &err)) {
    app_set_status(app, true, "config: %s", err ? err : "cannot save");
    free(err);
    return false;
  }
  return true;
}

static void change(NcApp *app, Overlay *o, const Pref *p, int dir) {
  Config *c = app->config;
  (void)apply(app, p, dir);
  if (p->field == f_scheme && o->plane) { /* the frame's blanks follow the new colors */
    uint64_t ch = 0;
    ncchannels_set_bg_rgb(&ch, app->theme.rgb[TC_OVERLAY_BG]);
    ncchannels_set_fg_rgb(&ch, app->theme.rgb[TC_OVERLAY_FG]);
    ncplane_set_base(o->plane, " ", 0, ch);
  }
  ListWidget *l = o->data;
  size_t cur = l->cursor;
  fill(l, c);
  l->cursor = cur;
}

static bool prefs_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  ListWidget *l = o->data;
  if (ev->type == UI_EVENT_KEY) {
    ListRow *r = list_selected(l);
    bool space = !ev->key.is_special && ev->key.key == ' ' && !ev->key.mods;
    bool left = ev->key.is_special && ev->key.key == UI_KEY_LEFT;
    bool right = ev->key.is_special && ev->key.key == UI_KEY_RIGHT;
    if (r && (space || right || left || (ev->key.is_special && ev->key.key == UI_KEY_ENTER))) {
      change(app, o, &PREFS[r->kind], left ? -1 : 1);
      return true;
    }
  }
  ListAction a;
  if (ev->type == UI_EVENT_MOUSE) {
    a = list_mouse(l, ev, 0, o->rows);
    if (a == LIST_ACTIVATE) {
      ListRow *r = list_selected(l);
      if (r) change(app, o, &PREFS[r->kind], 1);
      return true;
    }
  } else {
    a = list_key(l, ev);
  }
  if (a == LIST_CANCEL) overlay_pop_this(app, o);
  return true;
}

static void prefs_draw(Overlay *o, NcApp *app) {
  list_draw(o->data, o->plane, &app->theme, 1, 1, o->rows, o->cols, NULL, NULL);
}

static void prefs_layout(Overlay *o, NcApp *app) {
  o->cols = app->cols - 8 < 56 ? app->cols - 8 : 56;
  o->rows = NPREFS < app->rows - 6 ? NPREFS : app->rows - 6;
  if (o->rows < 3) o->rows = 3;
}

static void prefs_free(void *data) {
  list_free(data);
  free(data);
}

void prefs_open(NcApp *app) {
  config_section(app->config, "notcurses"); /* the frontend's own section exists from now on */
  ListWidget *l = safe_calloc(1, sizeof *l);
  list_init(l, false);
  fill(l, app->config);
  Overlay *o = overlay_new("Preferences");
  o->footer = "Space change  \xe2\x86\x90\xe2\x86\x92 cycle  Esc close";
  o->data = l;
  o->free_data = prefs_free;
  o->draw = prefs_draw;
  o->key = prefs_key;
  o->layout = prefs_layout;
  overlay_push(app, o);
}

int prefs_count(void) { return NPREFS; }

bool prefs_is_toggle(int i) {
  return i >= 0 && i < NPREFS && (PREFS[i].kind == P_BOOL || PREFS[i].kind == P_SECT);
}

char *prefs_toggle_text(NcApp *app, int i) {
  if (!prefs_is_toggle(i)) return NULL;
  char *v = value_text(app->config, &PREFS[i]);
  char *out = str_printf("%s: %s", PREFS[i].name, v);
  free(v);
  return out;
}

void prefs_toggle(NcApp *app, int i) {
  if (!prefs_is_toggle(i)) return;
  config_section(app->config, "notcurses"); /* as prefs_open: the section's settings live there */
  if (!apply(app, &PREFS[i], 1)) return; /* the status says why */
  char *text = prefs_toggle_text(app, i);
  app_set_status(app, false, "%s", text ? text : "");
  free(text);
}
