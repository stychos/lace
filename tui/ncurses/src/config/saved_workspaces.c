/*
 * Lace
 * Saved workspaces (workspaces.json)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "saved_workspaces.h"
#include "../platform/platform.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include "util/json_helpers.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *file_path(void) {
  const char *dir = platform_get_config_dir();
  return dir ? str_printf("%s/%s", dir, SAVED_WORKSPACES_FILE) : NULL;
}

void saved_workspaces_load(SavedWorkspaces *list) {
  memset(list, 0, sizeof(*list));
  char *path = file_path();
  if (!path || !platform_file_exists(path)) {
    free(path);
    return;
  }
  char *content = platform_read_file(path, 10 * 1024 * 1024);
  cJSON *json = content ? cJSON_Parse(content) : NULL;
  str_secure_free(content); /* Query text may hold anything */
  cJSON *arr = json ? json_get_array(json, "workspaces") : NULL;
  if (!arr) {
    list->load_error = str_printf("%s could not be read", SAVED_WORKSPACES_FILE);
    cJSON_Delete(json);
    free(path);
    return;
  }
  free(path);
  size_t n = (size_t)json_array_size(arr);
  list->items = n ? safe_calloc(n, sizeof(SavedWorkspace)) : NULL;
  cJSON *e;
  cJSON_ArrayForEach(e, arr) {
    const char *name = json_get_string(e, "name", NULL);
    cJSON *ws = json_get_object(e, "workspace");
    if (!name || !name[0] || !ws || list->count >= n)
      continue;
    SavedWorkspace *s = &list->items[list->count++];
    s->name = str_dup(name);
    s->saved = (time_t)json_get_int64(e, "saved", 0);
    s->workspace = cJSON_Duplicate(ws, true);
  }
  cJSON_Delete(json);
}

bool saved_workspaces_save(const SavedWorkspaces *list, char **error) {
  if (list->load_error) {
    if (error)
      *error = str_printf("Not saved: %s", list->load_error);
    return false;
  }
  const char *dir = platform_get_config_dir();
  char *path = file_path();
  if (!dir || !path) {
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
    JSON_ADD_STR(e, "name", list->items[i].name);
    cJSON_AddNumberToObject(e, "saved", (double)list->items[i].saved);
    cJSON_AddItemToObject(e, "workspace", cJSON_Duplicate(list->items[i].workspace, true));
    cJSON_AddItemToArray(arr, e);
  }
  cJSON_AddItemToObject(json, "workspaces", arr);
  char *content = cJSON_Print(json);
  cJSON_Delete(json);
  bool ok = content && platform_write_file_private(path, content, strlen(content));
  int e = errno;
  str_secure_free(content);
  if (!ok && error)
    *error = str_printf("Could not write %s: %s", path, strerror(e));
  free(path);
  return ok;
}

size_t saved_workspaces_find(const SavedWorkspaces *list, const char *name) {
  for (size_t i = 0; i < list->count; i++)
    if (str_eq(list->items[i].name, name))
      return i;
  return SIZE_MAX;
}

void saved_workspaces_put(SavedWorkspaces *list, const char *name, cJSON *workspace) {
  size_t i = saved_workspaces_find(list, name);
  if (i == SIZE_MAX) {
    list->items = safe_reallocarray(list->items, list->count + 1, sizeof(SavedWorkspace));
    i = list->count++;
    list->items[i].name = str_dup(name);
  } else {
    cJSON_Delete(list->items[i].workspace);
  }
  list->items[i].workspace = workspace;
  list->items[i].saved = time(NULL);
}

void saved_workspaces_remove(SavedWorkspaces *list, size_t index) {
  if (index >= list->count)
    return;
  free(list->items[index].name);
  cJSON_Delete(list->items[index].workspace);
  memmove(&list->items[index], &list->items[index + 1],
          (list->count - index - 1) * sizeof(SavedWorkspace));
  list->count--;
}

void saved_workspaces_free(SavedWorkspaces *list) {
  for (size_t i = 0; i < list->count; i++) {
    free(list->items[i].name);
    cJSON_Delete(list->items[i].workspace);
  }
  free(list->items);
  free(list->load_error);
  memset(list, 0, sizeof(*list));
}
