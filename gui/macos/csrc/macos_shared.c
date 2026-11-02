/*
 * Lace macOS frontend
 * See macos_shared.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "macos_shared.h"
#include "config/saved_workspaces.h"
#include "config/session_json.h"
#include "config/workspace_json.h"
#include <util/str.h>
#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdlib.h>

char *lace_macos_session_workspaces(const char *text, bool legacy, char **error) {
  cJSON *ws = lace_session_json_workspaces(text, legacy, "macos", error);
  if (!ws)
    return NULL;
  char *out = cJSON_PrintUnformatted(ws);
  cJSON_Delete(ws);
  return out;
}

char *lace_macos_session_merge(const char *existing, const char *workspaces_json, int current, char **error) {
  cJSON *ws = workspaces_json ? cJSON_Parse(workspaces_json) : NULL;
  if (!ws || !cJSON_IsArray(ws)) {
    cJSON_Delete(ws);
    if (error)
      *error = str_dup("session.json: the workspaces are not a JSON array");
    return NULL;
  }
  return lace_session_json_merge(existing, ws, current, error); /* takes ws */
}

/* workspaces.json, read afresh; false and *error when it does not parse */
static bool saved_load(SavedWorkspaces *list, char **error) {
  saved_workspaces_load(list);
  if (!list->load_error)
    return true;
  if (error)
    *error = str_dup(list->load_error);
  saved_workspaces_free(list);
  return false;
}

static bool saved_write(SavedWorkspaces *list, char **error) {
  bool ok = saved_workspaces_save(list, error);
  saved_workspaces_free(list);
  return ok;
}

char *lace_macos_saved_workspaces_list(char **error) {
  SavedWorkspaces list;
  if (!saved_load(&list, error))
    return NULL;
  cJSON *arr = cJSON_CreateArray();
  for (size_t i = 0; i < list.count; i++) {
    const SavedWorkspace *s = &list.items[i];
    cJSON *ws = NULL;
    switch (workspace_json_shape(s->workspace)) {
    case WORKSPACE_JSON_NESTED:
      ws = cJSON_Duplicate(s->workspace, true);
      break;
    case WORKSPACE_JSON_FLAT:
      ws = workspace_json_to_nested(s->workspace, NULL);
      break;
    default:
      break;
    }
    if (!ws)
      continue;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", s->name);
    cJSON_AddNumberToObject(e, "saved", (double)s->saved);
    cJSON_AddItemToObject(e, "workspace", ws);
    cJSON_AddItemToArray(arr, e);
  }
  saved_workspaces_free(&list);
  char *out = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);
  return out;
}

bool lace_macos_saved_workspaces_put(const char *name, const char *workspace_json, char **error) {
  cJSON *ws = workspace_json ? cJSON_Parse(workspace_json) : NULL;
  if (!name || !name[0] || !cJSON_IsObject(ws)) {
    cJSON_Delete(ws);
    if (error)
      *error = str_dup("workspaces.json: a layout needs a name and a workspace object");
    return false;
  }
  SavedWorkspaces list;
  if (!saved_load(&list, error)) {
    cJSON_Delete(ws);
    return false;
  }
  saved_workspaces_put(&list, name, ws); /* takes ws */
  return saved_write(&list, error);
}

bool lace_macos_saved_workspaces_remove(const char *name, char **error) {
  SavedWorkspaces list;
  if (!saved_load(&list, error))
    return false;
  size_t i = name ? saved_workspaces_find(&list, name) : SIZE_MAX;
  if (i == SIZE_MAX) {
    if (error)
      *error = str_printf("No saved workspace named %s", name ? name : "");
    saved_workspaces_free(&list);
    return false;
  }
  saved_workspaces_remove(&list, i);
  return saved_write(&list, error);
}

bool lace_macos_saved_workspaces_rename(const char *name, const char *new_name, char **error) {
  SavedWorkspaces list;
  if (!new_name || !new_name[0]) {
    if (error)
      *error = str_dup("A saved workspace needs a name");
    return false;
  }
  if (!saved_load(&list, error))
    return false;
  size_t i = name ? saved_workspaces_find(&list, name) : SIZE_MAX;
  const char *err = NULL;
  if (i == SIZE_MAX)
    err = "No saved workspace of that name";
  else if (saved_workspaces_find(&list, new_name) != SIZE_MAX)
    err = "A saved workspace of that name exists already";
  if (err) {
    if (error)
      *error = str_dup(err);
    saved_workspaces_free(&list);
    return false;
  }
  /* As the TUIs rename: stored again under the new name, the old one removed */
  saved_workspaces_put(&list, new_name, cJSON_Duplicate(list.items[i].workspace, true));
  size_t old = saved_workspaces_find(&list, name);
  if (old != SIZE_MAX)
    saved_workspaces_remove(&list, old);
  return saved_write(&list, error);
}

bool lace_macos_fields_check(const ConnFields *f, int field) {
  return f && field >= 0 && field < CF_COUNT && f->chk[field];
}

void lace_macos_fields_set_check(ConnFields *f, int field, bool on) {
  if (f && field >= 0 && field < CF_COUNT)
    f->chk[field] = on;
}

void lace_macos_fields_set_message(ConnFields *f, bool error, const char *text) {
  if (!f)
    return;
  free(f->message);
  f->message = text ? str_dup(text) : NULL;
  f->message_error = error;
}
