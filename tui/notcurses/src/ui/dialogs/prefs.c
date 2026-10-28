/* Preferences: the settings that matter, changed in place and saved at once */
#include "config/config.h"
#include "core/prefs_model.h"
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

/* The rows: the shared table's (core/prefs_model.c) that this frontend shows */
#define SECTION "notcurses"
#define NAME_COLS 25

static int rows_of(int *idx) {
  int n = 0;
  for (int i = 0; i < prefs_model_count(); i++)
    if (prefs_model_for(i, CONFIG_FE_NOTCURSES)) idx[n++] = i;
  return n;
}

bool prefs_section_bool(const Config *c, const char *key, bool dflt) {
  return prefs_model_section_bool(c, SECTION, key, dflt);
}

static void fill(ListWidget *l, Config *c) {
  int idx[64];
  int n = rows_of(idx);
  ListRow *rows = safe_calloc((size_t)n, sizeof *rows);
  for (int r = 0; r < n; r++) {
    char *v = prefs_model_value(c, idx[r], SECTION);
    rows[r].text = str_printf("%-*s%s", NAME_COLS, prefs_model_name(idx[r]), v);
    free(v);
    rows[r].kind = idx[r];
  }
  list_set_rows(l, rows, (size_t)n);
}

/* Change one setting and save the config: the one path for the dialog and the
 * palette */
static bool apply(NcApp *app, int i, int dir) {
  Config *c = app->config;
  if (prefs_model_kind(i) == PREF_OPEN) { /* the hotkeys editor, which saves its own changes */
    help_open(app);
    return true;
  }
  prefs_model_change(c, i, dir, SECTION);
  if (prefs_model_is_scheme(i)) screen_apply_theme(app);
  char *err = NULL;
  if (!config_save(c, &err)) {
    app_set_status(app, true, "config: %s", err ? err : "cannot save");
    free(err);
    return false;
  }
  return true;
}

static void change(NcApp *app, Overlay *o, int i, int dir) {
  Config *c = app->config;
  (void)apply(app, i, dir);
  if (prefs_model_is_scheme(i) && o->plane) { /* the frame's blanks follow the new colors */
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
      change(app, o, r->kind, left ? -1 : 1);
      return true;
    }
  }
  ListAction a;
  if (ev->type == UI_EVENT_MOUSE) {
    a = list_mouse(l, ev, 0, o->rows);
    if (a == LIST_ACTIVATE) {
      ListRow *r = list_selected(l);
      if (r) change(app, o, r->kind, 1);
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
  int idx[64];
  int n = rows_of(idx);
  o->rows = n < app->rows - 6 ? n : app->rows - 6;
  if (o->rows < 3) o->rows = 3;
}

static void prefs_free(void *data) {
  list_free(data);
  free(data);
}

void prefs_open(NcApp *app) {
  config_section(app->config, SECTION); /* the frontend's own section exists from now on */
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

int prefs_count(void) { return prefs_model_count(); }

bool prefs_is_toggle(int i) { return prefs_model_for(i, CONFIG_FE_NOTCURSES) && prefs_model_is_toggle(i); }

char *prefs_toggle_text(NcApp *app, int i) {
  if (!prefs_is_toggle(i)) return NULL;
  return prefs_model_toggle_text(app->config, i, SECTION);
}

void prefs_toggle(NcApp *app, int i) {
  if (!prefs_is_toggle(i)) return;
  config_section(app->config, SECTION); /* as prefs_open: the section's settings live there */
  if (!apply(app, i, 1)) return; /* the status says why */
  char *text = prefs_toggle_text(app, i);
  app_set_status(app, false, "%s", text ? text : "");
  free(text);
}
