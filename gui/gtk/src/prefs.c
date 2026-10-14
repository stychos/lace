/*
 * Lace GTK frontend
 * Preferences
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
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PREFS_FILE "gtk.json"

char *lace_prefs_path(void) {
  const char *dir = platform_get_config_dir();
  if (!dir)
    return NULL;
  return str_printf("%s%s%s", dir, LACE_PATH_SEP_STR, PREFS_FILE);
}

static void defaults(LacePrefs *p) {
  memset(p, 0, sizeof(*p));
  p->daemon_spawn_mode = 0;
  p->daemon_idle_timeout = 0;
  p->confirm_delete = true;
  p->restore_session = true;
  p->approximate_counts = true;
  p->history_mode = LACE_HISTORY_MODE_PERSISTENT;
  p->history_max_size = LACE_HISTORY_SIZE_DEFAULT;
  p->color_scheme = LACE_COLOR_SYSTEM;
  p->page_size = 1000;
  p->row_numbers = LACE_ROW_NUMBERS_AUTO;
}

LacePrefs *lace_prefs_load(void) {
  LacePrefs *p = safe_calloc(1, sizeof(*p));
  defaults(p);
  char *path = lace_prefs_path();
  if (!path || !platform_file_exists(path)) {
    free(path);
    return p;
  }
  char *content = platform_read_file(path, 1024 * 1024);
  free(path);
  if (!content)
    return p;
  cJSON *json = cJSON_Parse(content);
  str_secure_free(content);
  if (!json)
    return p;
  JSON_GET_INT_RANGE(json, "daemon_spawn_mode", p->daemon_spawn_mode, 0, 3);
  JSON_GET_INT_RANGE(json, "daemon_idle_timeout", p->daemon_idle_timeout, 0, 86400);
  p->confirm_delete = json_get_bool(json, "confirm_delete", p->confirm_delete);
  p->restore_session = json_get_bool(json, "restore_session", p->restore_session);
  p->approximate_counts = json_get_bool(json, "approximate_counts", p->approximate_counts);
  JSON_GET_INT_RANGE(json, "history_mode", p->history_mode, 0, 2);
  JSON_GET_INT_RANGE(json, "history_max_size", p->history_max_size,
                     LACE_HISTORY_SIZE_MIN, LACE_HISTORY_SIZE_MAX);
  JSON_GET_INT_RANGE(json, "color_scheme", p->color_scheme, 0, 2);
  JSON_GET_INT_RANGE(json, "page_size", p->page_size, 100, 100000);
  JSON_GET_INT_RANGE(json, "row_numbers", p->row_numbers, 0, 2);
  /* The earlier boolean */
  if (!cJSON_GetObjectItem(json, "row_numbers") && cJSON_GetObjectItem(json, "show_row_numbers"))
    p->row_numbers = json_get_bool(json, "show_row_numbers", true) ? LACE_ROW_NUMBERS_AUTO
                                                                   : LACE_ROW_NUMBERS_NEVER;
  cJSON_Delete(json);
  return p;
}

bool lace_prefs_save(const LacePrefs *p, char **error) {
  char *path = lace_prefs_path();
  if (!path) {
    err_set(error, "No configuration directory");
    return false;
  }
  cJSON *json = cJSON_CreateObject();
  JSON_ADD_INT(json, "daemon_spawn_mode", p->daemon_spawn_mode);
  JSON_ADD_INT(json, "daemon_idle_timeout", p->daemon_idle_timeout);
  JSON_ADD_BOOL(json, "confirm_delete", p->confirm_delete);
  JSON_ADD_BOOL(json, "restore_session", p->restore_session);
  JSON_ADD_BOOL(json, "approximate_counts", p->approximate_counts);
  JSON_ADD_INT(json, "history_mode", p->history_mode);
  JSON_ADD_INT(json, "history_max_size", p->history_max_size);
  JSON_ADD_INT(json, "color_scheme", p->color_scheme);
  JSON_ADD_INT(json, "page_size", p->page_size);
  JSON_ADD_INT(json, "row_numbers", p->row_numbers);
  char *text = cJSON_Print(json);
  cJSON_Delete(json);
  bool ok = text && platform_write_file_private(path, text, strlen(text));
  if (!ok)
    err_setf(error, "Could not write %s: %s", path, strerror(errno));
  free(text);
  free(path);
  return ok;
}

void lace_prefs_free(LacePrefs *p) { free(p); }

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
