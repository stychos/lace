/*
 * Lace
 * Recent connections (recent.json)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "recent.h"
#include "../platform/platform.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include "connections.h"
#include "util/json_helpers.h"
#include <cjson/cJSON.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static char *recent_path(void) {
  const char *dir = platform_get_config_dir();
  return dir ? str_printf("%s/%s", dir, RECENT_FILE) : NULL;
}

char *recent_strip_passwords(const char *url) {
  SavedConnection *c = connmgr_parse_connstr(url, NULL);
  if (!c)
    return NULL;
  c->save_password = false;
  c->save_ssh_password = false;
  char *out = lace_saved_conn_build_connstr(c, NULL, NULL);
  connmgr_free_connection(c);
  free(c);
  return out;
}

void recent_load(RecentList *list) {
  memset(list, 0, sizeof(*list));
  char *path = recent_path();
  if (!path || !platform_file_exists(path)) {
    free(path);
    return;
  }
  char *content = platform_read_file(path, 1024 * 1024);
  free(path);
  cJSON *json = content ? cJSON_Parse(content) : NULL;
  str_secure_free(content);
  cJSON *arr = json ? json_get_array(json, "recent") : NULL;
  if (arr) {
    list->items = safe_calloc(RECENT_MAX, sizeof(RecentEntry));
    cJSON *e;
    cJSON_ArrayForEach(e, arr) {
      if (list->count >= RECENT_MAX)
        break;
      const char *url = json_get_string(e, "url", NULL);
      /* Passwords are never kept, also if someone wrote one in the file */
      char *clean = url && url[0] ? recent_strip_passwords(url) : NULL;
      if (!clean)
        continue;
      list->items[list->count].url = clean;
      list->items[list->count].used = (time_t)json_get_int64(e, "used", 0);
      list->count++;
    }
  }
  cJSON_Delete(json);
}

bool recent_save(const RecentList *list, char **error) {
  char *path = recent_path();
  const char *dir = platform_get_config_dir();
  if (!path || !dir) {
    free(path);
    if (error)
      *error = str_dup("No configuration directory");
    return false;
  }
  if (!platform_dir_exists(dir))
    platform_mkdir(dir);
  cJSON *json = cJSON_CreateObject();
  cJSON *arr = cJSON_CreateArray();
  for (size_t i = 0; i < list->count; i++) {
    cJSON *e = cJSON_CreateObject();
    JSON_ADD_STR(e, "url", list->items[i].url);
    cJSON_AddNumberToObject(e, "used", (double)list->items[i].used);
    cJSON_AddItemToArray(arr, e);
  }
  cJSON_AddItemToObject(json, "recent", arr);
  char *content = cJSON_Print(json);
  cJSON_Delete(json);
  bool ok = content && platform_write_file_private(path, content, strlen(content));
  int e = errno;
  free(content);
  if (!ok && error)
    *error = str_printf("Could not write %s: %s", path, strerror(e));
  free(path);
  return ok;
}

bool recent_add(RecentList *list, const char *url) {
  char *clean = recent_strip_passwords(url);
  if (!clean)
    return false;
  if (!list->items)
    list->items = safe_calloc(RECENT_MAX, sizeof(RecentEntry));
  for (size_t i = 0; i < list->count; i++) {
    if (str_eq(list->items[i].url, clean)) {
      recent_remove(list, i);
      break;
    }
  }
  if (list->count == RECENT_MAX)
    recent_remove(list, RECENT_MAX - 1);
  memmove(&list->items[1], &list->items[0], list->count * sizeof(RecentEntry));
  list->items[0].url = clean;
  list->items[0].used = time(NULL);
  list->count++;
  return true;
}

void recent_remove(RecentList *list, size_t index) {
  if (index >= list->count)
    return;
  free(list->items[index].url);
  memmove(&list->items[index], &list->items[index + 1],
          (list->count - index - 1) * sizeof(RecentEntry));
  list->count--;
}

void recent_free(RecentList *list) {
  for (size_t i = 0; i < list->count; i++)
    free(list->items[i].url);
  free(list->items);
  memset(list, 0, sizeof(*list));
}
