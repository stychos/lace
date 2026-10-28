/*
 * Lace
 * The TUIs' preferences: see prefs_model.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "prefs_model.h"
#include "../../../liblace/include/util/str.h"
#include <cjson/cJSON.h>
#include <stddef.h>
#include <stdlib.h>

typedef struct {
  const char *name;
  PrefKind kind;
  size_t offset;            /* the field inside GeneralConfig */
  const char *const *names; /* PREF_ENUM */
  int count;
  const int *values; /* PREF_NUM: the steps */
  int num_values;
  const char *unit;
  const char *key; /* PREF_SECT */
  bool dflt;
  unsigned frontends;
} Pref;

#define G(f) offsetof(GeneralConfig, f)

static const char *const ROWNUM[] = {"never", "auto", "always"};
static const char *const SCHEME[] = {"system", "light", "dark"};
static const char *const HIST[] = {"off", "session", "persistent"};
static const char *const LASTWS[] = {"quit", "new workspace"}; /* CONFIG_LAST_WS_* */
static const char *const DAEMON[] = {"unix", "stdio", "tcp", "detect"};
static const char *const PAGING[] = {"offset", "keyset"}; /* LacePaginationStrategy */
static const int HIST_SIZES[] = {50, 100, 500, 1000, 5000, 10000};
static const int IDLE[] = {0, 60, 300, 900, 3600};

static const Pref PREFS[] = {
    {.name = "Confirm deletes", .kind = PREF_BOOL, .offset = G(delete_confirmation), .frontends = CONFIG_FE_ALL},
    {.name = "Confirm quit", .kind = PREF_BOOL, .offset = G(quit_confirmation), .frontends = CONFIG_FE_ALL},
    {.name = "Restore session", .kind = PREF_BOOL, .offset = G(restore_session), .frontends = CONFIG_FE_ALL},
    {.name = "Restore cursor position", .kind = PREF_BOOL, .offset = G(restore_cursor_position),
     .frontends = CONFIG_FE_ALL},
    {.name = "Keep connection tab", .kind = PREF_BOOL, .offset = G(keep_connection_tab), .frontends = CONFIG_FE_ALL},
    {.name = "Open the first table", .kind = PREF_BOOL, .offset = G(auto_open_first_table),
     .frontends = CONFIG_FE_ALL},
    {.name = "Last workspace closes", .kind = PREF_ENUM, .offset = G(last_workspace_close), .names = LASTWS,
     .count = 2, .frontends = CONFIG_FE_ALL},
    {.name = "Approximate counts", .kind = PREF_BOOL, .offset = G(approximate_counts), .frontends = CONFIG_FE_ALL},
    {.name = "Row numbers", .kind = PREF_ENUM, .offset = G(row_numbers), .names = ROWNUM, .count = 3,
     .frontends = CONFIG_FE_NOTCURSES},
    {.name = "Pagination", .kind = PREF_ENUM, .offset = G(pagination_strategy), .names = PAGING, .count = 2,
     .frontends = CONFIG_FE_NCURSES},
    {.name = "Color scheme", .kind = PREF_ENUM, .offset = G(color_scheme), .names = SCHEME, .count = 3,
     .frontends = CONFIG_FE_ALL},
    {.name = "Show header", .kind = PREF_BOOL, .offset = G(show_header), .frontends = CONFIG_FE_NCURSES},
    {.name = "Show status bar", .kind = PREF_BOOL, .offset = G(show_status_bar), .frontends = CONFIG_FE_NCURSES},
    {.name = "History", .kind = PREF_ENUM, .offset = G(history_mode), .names = HIST, .count = 3,
     .frontends = CONFIG_FE_ALL},
    {.name = "History size", .kind = PREF_NUM, .offset = G(history_max_size), .values = HIST_SIZES, .num_values = 6,
     .unit = "", .frontends = CONFIG_FE_ALL},
    {.name = "Daemon", .kind = PREF_ENUM, .offset = G(daemon_spawn_mode), .names = DAEMON, .count = 4,
     .frontends = CONFIG_FE_ALL},
    {.name = "Daemon idle timeout", .kind = PREF_NUM, .offset = G(daemon_idle_timeout), .values = IDLE,
     .num_values = 5, .unit = "s", .frontends = CONFIG_FE_ALL},
    {.name = "Tabs strip", .kind = PREF_SECT, .key = "tabs_strip", .dflt = true, .frontends = CONFIG_FE_NOTCURSES},
    {.name = "Filters strip", .kind = PREF_SECT, .key = "filters_strip", .dflt = true,
     .frontends = CONFIG_FE_NOTCURSES},
    {.name = "Clipboard OSC 52", .kind = PREF_SECT, .key = "clipboard_osc52", .dflt = true,
     .frontends = CONFIG_FE_NOTCURSES},
    {.name = "Hotkeys\xe2\x80\xa6", .kind = PREF_OPEN, .frontends = CONFIG_FE_ALL},
};
#define NPREFS ((int)(sizeof PREFS / sizeof *PREFS))

static const Pref *at(int i) { return i >= 0 && i < NPREFS ? &PREFS[i] : NULL; }

int prefs_model_count(void) { return NPREFS; }
const char *prefs_model_name(int i) { return at(i) ? at(i)->name : ""; }
PrefKind prefs_model_kind(int i) { return at(i) ? at(i)->kind : PREF_OPEN; }
bool prefs_model_for(int i, unsigned frontend) { return at(i) && (at(i)->frontends & frontend); }
bool prefs_model_is_toggle(int i) { return at(i) && (at(i)->kind == PREF_BOOL || at(i)->kind == PREF_SECT); }
bool prefs_model_is_scheme(int i) { return at(i) && at(i)->kind == PREF_ENUM && at(i)->names == SCHEME; }

static bool *bool_field(const Config *c, const Pref *p) { return (bool *)((char *)&c->general + p->offset); }
static int *int_field(const Config *c, const Pref *p) { return (int *)((char *)&c->general + p->offset); }

bool prefs_model_section_bool(const Config *c, const char *section, const char *key, bool dflt) {
  const cJSON *sec = c && c->extra && section ? cJSON_GetObjectItem(c->extra, section) : NULL;
  const cJSON *it = sec ? cJSON_GetObjectItem(sec, key) : NULL;
  return cJSON_IsBool(it) ? cJSON_IsTrue(it) : dflt;
}

char *prefs_model_value(const Config *c, int i, const char *section) {
  const Pref *p = at(i);
  if (!p || !c)
    return str_dup("");
  switch (p->kind) {
  case PREF_BOOL: return str_dup(*bool_field(c, p) ? "on" : "off");
  case PREF_ENUM: {
    int v = *int_field(c, p);
    return str_dup(v >= 0 && v < p->count ? p->names[v] : "?");
  }
  case PREF_NUM: return str_printf("%d%s", *int_field(c, p), p->unit);
  case PREF_SECT: return str_dup(prefs_model_section_bool(c, section, p->key, p->dflt) ? "on" : "off");
  case PREF_OPEN: break;
  }
  return str_dup("");
}

char *prefs_model_toggle_text(const Config *c, int i, const char *section) {
  char *v = prefs_model_value(c, i, section);
  char *out = str_printf("%s: %s", prefs_model_name(i), v);
  free(v);
  return out;
}

void prefs_model_change(Config *c, int i, int dir, const char *section) {
  const Pref *p = at(i);
  if (!p || !c)
    return;
  switch (p->kind) {
  case PREF_BOOL: {
    bool *b = bool_field(c, p);
    *b = !*b;
    break;
  }
  case PREF_ENUM: {
    int *v = int_field(c, p);
    *v = ((*v + dir) % p->count + p->count) % p->count;
    break;
  }
  case PREF_NUM: {
    int *v = int_field(c, p);
    int pos = 0;
    for (int k = 0; k < p->num_values; k++)
      if (p->values[k] <= *v)
        pos = k;
    *v = p->values[((pos + dir) % p->num_values + p->num_values) % p->num_values];
    break;
  }
  case PREF_SECT: {
    cJSON *sec = config_section(c, section);
    bool now = !prefs_model_section_bool(c, section, p->key, p->dflt);
    cJSON_DeleteItemFromObject(sec, p->key);
    cJSON_AddBoolToObject(sec, p->key, now);
    break;
  }
  case PREF_OPEN: break;
  }
}
