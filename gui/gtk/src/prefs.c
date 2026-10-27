/*
 * Lace GTK frontend
 * Preferences: a view over the shared config.json's "general"
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "prefs.h"
#include "platform/platform.h"
#include <config/history.h>
#include <util/json_helpers.h>
#include <util/mem.h>
#include <util/str.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEGACY_FILE "gtk.json"

static void from_config(LacePrefs *p) {
  const GeneralConfig *g = &p->config->general;
  p->daemon_spawn_mode = g->daemon_spawn_mode;
  p->daemon_idle_timeout = g->daemon_idle_timeout;
  p->confirm_delete = g->delete_confirmation;
  p->restore_session = g->restore_session;
  p->approximate_counts = g->approximate_counts;
  p->history_mode = g->history_mode;
  p->history_max_size = g->history_max_size;
  p->color_scheme = g->color_scheme;
  p->page_size = g->page_size;
  p->row_numbers = g->row_numbers;
  p->keep_connection_tab = g->keep_connection_tab;
  p->last_workspace_close = g->last_workspace_close;
  p->auto_open_first_table = g->auto_open_first_table;
}

static void to_config(LacePrefs *p) {
  GeneralConfig *g = &p->config->general;
  g->daemon_spawn_mode = p->daemon_spawn_mode;
  g->daemon_idle_timeout = p->daemon_idle_timeout;
  g->delete_confirmation = p->confirm_delete;
  g->restore_session = p->restore_session;
  g->approximate_counts = p->approximate_counts;
  g->history_mode = p->history_mode;
  g->history_max_size = p->history_max_size;
  g->color_scheme = p->color_scheme;
  g->page_size = p->page_size;
  g->row_numbers = p->row_numbers;
  g->keep_connection_tab = p->keep_connection_tab;
  g->last_workspace_close = p->last_workspace_close;
  g->auto_open_first_table = p->auto_open_first_table;
}

/* The "general" object config.json holds now (NULL: no file, or it does
 * not parse); caller deletes */
static cJSON *file_general(const char *config_path, bool *parses) {
  *parses = true;
  if (!platform_file_exists(config_path))
    return NULL;
  char *text = platform_read_file(config_path, 1024 * 1024);
  cJSON *root = text && *text ? cJSON_Parse(text) : NULL;
  if (text && *text && !root)
    *parses = false;
  str_secure_free(text);
  cJSON *g = root ? cJSON_DetachItemFromObject(root, "general") : NULL;
  cJSON_Delete(root);
  return g;
}

/* gtk.json's values for the keys "general" lacks, once */
static void migrate(LacePrefs *p, const char *config_path, const char *legacy_path) {
  if (!legacy_path || !platform_file_exists(legacy_path))
    return;
  bool parses = true;
  cJSON *general = file_general(config_path, &parses);
  if (!parses) { /* never overwritten: migrate another time */
    cJSON_Delete(general);
    return;
  }
  char *text = platform_read_file(legacy_path, 1024 * 1024);
  cJSON *old = text ? cJSON_Parse(text) : NULL;
  str_secure_free(text);
  GeneralConfig *g = &p->config->general;
#define HAS(key) (general && cJSON_GetObjectItem(general, key))
  if (old) {
    if (!HAS("daemon_spawn_mode"))
      JSON_GET_INT_RANGE(old, "daemon_spawn_mode", g->daemon_spawn_mode, 0, 3);
    if (!HAS("daemon_idle_timeout"))
      JSON_GET_INT_RANGE(old, "daemon_idle_timeout", g->daemon_idle_timeout, 0, 86400);
    if (!HAS("delete_confirmation"))
      g->delete_confirmation = json_get_bool(old, "confirm_delete", g->delete_confirmation);
    if (!HAS("restore_session"))
      g->restore_session = json_get_bool(old, "restore_session", g->restore_session);
    if (!HAS("approximate_counts"))
      g->approximate_counts = json_get_bool(old, "approximate_counts", g->approximate_counts);
    if (!HAS("history_mode"))
      JSON_GET_INT_RANGE(old, "history_mode", g->history_mode, 0, 2);
    if (!HAS("history_max_size"))
      JSON_GET_INT_RANGE(old, "history_max_size", g->history_max_size, LACE_HISTORY_SIZE_MIN,
                         LACE_HISTORY_SIZE_MAX);
    if (!HAS("color_scheme"))
      JSON_GET_INT_RANGE(old, "color_scheme", g->color_scheme, 0, 2);
    if (!HAS("page_size"))
      JSON_GET_INT_RANGE(old, "page_size", g->page_size, CONFIG_PAGE_SIZE_MIN, CONFIG_PAGE_SIZE_MAX);
    if (!HAS("row_numbers")) {
      JSON_GET_INT_RANGE(old, "row_numbers", g->row_numbers, 0, 2);
      /* The earlier boolean */
      if (!cJSON_GetObjectItem(old, "row_numbers") && cJSON_GetObjectItem(old, "show_row_numbers"))
        g->row_numbers = json_get_bool(old, "show_row_numbers", true) ? LACE_ROW_NUMBERS_AUTO
                                                                       : LACE_ROW_NUMBERS_NEVER;
    }
  }
#undef HAS
  cJSON_Delete(old);
  cJSON_Delete(general);
  char *err = NULL;
  if (config_save_file(p->config, config_path, &err)) {
    char *to = str_printf("%s.migrated", legacy_path);
    if (rename(legacy_path, to) != 0)
      fprintf(stderr, "lace-gtk: could not rename %s\n", legacy_path);
    free(to);
  }
  free(err);
}

LacePrefs *lace_prefs_load_from(const char *config_path, const char *legacy_path) {
  LacePrefs *p = safe_calloc(1, sizeof(*p));
  char *err = NULL;
  p->config = config_load_file(config_path, &err);
  free(err);
  if (!p->config)
    p->config = config_get_defaults();
  p->config->hotkeys_foreign = true; /* the TUIs' table: kept as the file has it */
  migrate(p, config_path, legacy_path);
  from_config(p);
  return p;
}

bool lace_prefs_save_to(LacePrefs *p, const char *config_path, char **error) {
  to_config(p);
  return config_save_file(p->config, config_path, error);
}

LacePrefs *lace_prefs_load(void) {
  char *cfg = config_get_path();
  const char *dir = platform_get_config_dir();
  char *legacy = dir ? str_printf("%s%s%s", dir, LACE_PATH_SEP_STR, LEGACY_FILE) : NULL;
  LacePrefs *p = lace_prefs_load_from(cfg, legacy);
  free(cfg);
  free(legacy);
  return p;
}

bool lace_prefs_save(LacePrefs *p, char **error) { return lace_prefs_save_to(p, NULL, error); }

void lace_prefs_free(LacePrefs *p) {
  if (!p)
    return;
  config_free(p->config);
  free(p);
}

bool lace_prefs_row_numbers_for(const LacePrefs *p, const LaceSchema *schema) {
  if (p->row_numbers == LACE_ROW_NUMBERS_NEVER)
    return false;
  if (p->row_numbers == LACE_ROW_NUMBERS_ALWAYS || !schema)
    return true;
  /* One auto-increment integer key already numbers the rows */
  size_t keys = 0;
  bool auto_int = false;
  for (size_t i = 0; i < schema->num_columns; i++) {
    const LaceColumn *c = &schema->columns[i];
    if (!c->primary_key)
      continue;
    keys++;
    auto_int = c->auto_increment && c->type == LACE_TYPE_INT;
  }
  return !(keys == 1 && auto_int);
}
