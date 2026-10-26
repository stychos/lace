/* The hotkeys overlay (`?`): every action of this frontend by category with
 * its keys, and their editor: Enter (or +) captures the next key and adds it
 * to the selected action, - (x, Delete) removes its last key, the
 * config_reset keys (r, R) restore the defaults of one or every action; a
 * key already bound in the action's category is refused; every change is
 * saved to config.json at once and applies from then on. */
#include "ui/dialogs/help.h"
#include "config/config.h"
#include "ui/dialogs/confirm.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

#define FOOTER_EDIT "Enter add key  - remove last  r reset  R reset all  Esc close"

typedef struct {
  ListWidget list;
  bool capture; /* the next key goes to the selected action */
  char *prompt; /* the capture footer */
} HelpState;

static void fill(HelpState *s, NcApp *app) {
  ListRow *rows = NULL;
  size_t n = 0, cap = 0;
  for (int c = 0; c < HOTKEY_CAT_COUNT; c++) {
    size_t first = n;
    for (int a = 0; a < HOTKEY_COUNT; a++) {
      if ((int)hotkey_get_category((HotkeyAction)a) != c) continue;
      if (!(hotkey_action_frontends((HotkeyAction)a) & CONFIG_FE_NOTCURSES)) continue;
      if (n + 2 > cap) { cap = cap ? cap * 2 : 64; rows = safe_realloc(rows, cap * sizeof *rows); }
      if (n == first)
        rows[n++] = (ListRow){str_dup(hotkey_category_name((HotkeyCategory)c)), NULL, NULL, -1, NULL, true};
      /* a key another action of the same place has is marked */
      const HotkeyBinding *b = &app->config->hotkeys[a];
      HotkeyAction other = HOTKEY_COUNT;
      for (size_t k = 0; k < b->num_keys && other == HOTKEY_COUNT; k++)
        other = hotkey_find_conflict(app->config, (HotkeyAction)a, b->keys[k]);
      char *detail = other == HOTKEY_COUNT ? NULL
                     : str_printf("\xe2\x9a\xa0 also %s", hotkey_action_name(other)); /* ⚠ */
      rows[n++] = (ListRow){str_dup(hotkey_action_name((HotkeyAction)a)), detail,
                            hotkey_get_display(app->config, (HotkeyAction)a), a, NULL, false};
    }
  }
  bool refill = s->list.num_rows > 0; /* keep the place on a refill; the first fill starts at the top */
  size_t cur = s->list.cursor, scroll = s->list.scroll;
  list_set_rows(&s->list, rows, n);
  if (refill && cur < s->list.num_shown) { s->list.cursor = cur; s->list.scroll = scroll; }
}

static HotkeyAction selected(HelpState *s) {
  ListRow *r = list_selected(&s->list);
  return r && r->kind >= 0 ? (HotkeyAction)r->kind : HOTKEY_COUNT;
}

static bool save(NcApp *app) {
  char *err = NULL;
  if (config_save(app->config, &err)) return true;
  app_set_status(app, true, "config: %s", err ? err : "cannot save");
  free(err);
  return false;
}

static void start_capture(HelpState *s, Overlay *o) {
  HotkeyAction a = selected(s);
  if (a == HOTKEY_COUNT) return;
  s->capture = true;
  free(s->prompt);
  s->prompt = str_printf("Press the key to add to %s  (Esc cancels)", hotkey_action_name(a));
  o->footer = s->prompt;
}

static void capture(HelpState *s, Overlay *o, NcApp *app, const UiEvent *ev) {
  HotkeyAction a = selected(s);
  s->capture = false;
  o->footer = FOOTER_EDIT;
  if (a == HOTKEY_COUNT || (ev->key.is_special && ev->key.key == UI_KEY_ESCAPE)) return;
  char *key = hotkey_key_string(ev);
  if (!key) {
    app_set_status(app, false, "That key cannot be bound");
    return;
  }
  HotkeyAction other = hotkey_find_conflict(app->config, a, key);
  if (other != HOTKEY_COUNT && other != a) {
    app_set_status(app, false, "%s is already bound to %s", key, hotkey_action_name(other));
  } else if (!hotkey_add_key(app->config, a, key)) {
    app_set_status(app, false, "Cannot bind %s", key);
  } else if (save(app)) {
    app_set_status(app, false, "%s: added %s", hotkey_action_name(a), key);
  }
  fill(s, app);
  free(key);
}

static bool still_open(const NcApp *app, const Overlay *o) {
  for (size_t i = 0; i < app->num_overlays; i++)
    if (app->overlays[i] == o) return true;
  return false;
}

static void reset_all(NcApp *app, bool yes, void *data) {
  Overlay *o = data;
  if (!yes) return;
  for (int i = 0; i < HOTKEY_COUNT; i++) config_reset_hotkey(app->config, (HotkeyAction)i);
  if (save(app)) app_set_status(app, false, "Every action has its default keys again");
  if (still_open(app, o)) fill(o->data, app);
}

static bool help_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  HelpState *s = o->data;
  if (s->capture) {
    if (ev->type == UI_EVENT_KEY) capture(s, o, app, ev);
    return true;
  }
  if (ev->type == UI_EVENT_KEY) {
    HotkeyAction a = selected(s);
    bool plain = !ev->key.is_special && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT));
    bool special = ev->key.is_special && !ev->key.mods;
    bool add = (plain && (ev->key.key == '+' || ev->key.key == '=')) || (special && ev->key.key == UI_KEY_INSERT);
    bool remove = (plain && (ev->key.key == '-' || ev->key.key == 'x')) || (special && ev->key.key == UI_KEY_DELETE);
    if (add) {
      start_capture(s, o);
      return true;
    }
    if (remove && a != HOTKEY_COUNT) {
      const HotkeyBinding *b = &app->config->hotkeys[a];
      if (!b->num_keys) {
        app_set_status(app, false, "%s has no key", hotkey_action_name(a));
        return true;
      }
      char *gone = str_dup(b->keys[b->num_keys - 1]);
      hotkey_remove_key(app->config, a, b->num_keys - 1);
      if (save(app)) app_set_status(app, false, "%s: removed %s", hotkey_action_name(a), gone);
      free(gone);
      fill(s, app);
      return true;
    }
    if (a != HOTKEY_COUNT && hotkey_matches(app->config, ev, HOTKEY_CONFIG_RESET)) {
      config_reset_hotkey(app->config, a);
      if (save(app)) app_set_status(app, false, "%s: default keys", hotkey_action_name(a));
      fill(s, app);
      return true;
    }
    if (hotkey_matches(app->config, ev, HOTKEY_CONFIG_RESET_ALL)) {
      confirm_open(app, "Reset hotkeys", "Give every action its default keys?", "Reset", true, reset_all, o);
      return true;
    }
  }
  ListAction act = ev->type == UI_EVENT_MOUSE ? list_mouse(&s->list, ev, 0, o->rows) : list_key(&s->list, ev);
  if (act == LIST_CANCEL) overlay_pop_this(app, o);
  else if (act == LIST_ACTIVATE) start_capture(s, o);
  return true;
}

static void help_draw(Overlay *o, NcApp *app) {
  HelpState *s = o->data;
  list_draw(&s->list, o->plane, &app->theme, 1, 1, o->rows, o->cols, NULL, NULL);
}

static void help_layout(Overlay *o, NcApp *app) {
  o->cols = app->cols - 8 < 76 ? app->cols - 8 : 76;
  o->rows = app->rows - 5; /* as tall as the layout allows */
}

static void help_free(void *data) {
  HelpState *s = data;
  list_free(&s->list);
  free(s->prompt);
  free(s);
}

void help_open(NcApp *app) {
  HelpState *s = safe_calloc(1, sizeof *s);
  list_init(&s->list, false);
  fill(s, app);
  Overlay *o = overlay_new("Hotkeys");
  o->footer = FOOTER_EDIT;
  o->data = s;
  o->free_data = help_free;
  o->draw = help_draw;
  o->key = help_key;
  o->layout = help_layout;
  overlay_push(app, o);
}
