/*
 * Lace
 * session.json's JSON layer for the GUI frontends
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "session_json.h"
#include "workspace_json.h"
#include <util/json_helpers.h>
#include <util/str.h>
#include <stdlib.h>

static cJSON *parse(const char *text, char **error) {
  cJSON *root = text ? cJSON_Parse(text) : NULL;
  if (!root || !cJSON_IsObject(root)) {
    const char *at = cJSON_GetErrorPtr();
    err_setf(error, "session.json: JSON parse error near byte %ld", at && text ? (long)(at - text) : 0L);
    cJSON_Delete(root);
    return NULL;
  }
  return root;
}

/* Geometry at a workspace's top level goes under the frontend's member */
static void geometry_under(cJSON *ws, const char *frontend) {
  static const char *const keys[] = {"width", "height", "frame", "maximized"};
  for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
    cJSON *v = cJSON_DetachItemFromObject(ws, keys[i]);
    if (!v)
      continue;
    cJSON *own = cJSON_GetObjectItem(ws, frontend);
    if (!own)
      own = cJSON_AddObjectToObject(ws, frontend);
    cJSON_DeleteItemFromObject(own, keys[i]);
    cJSON_AddItemToObject(own, keys[i], v);
  }
}

cJSON *lace_session_json_workspaces(const char *text, bool legacy, const char *frontend, char **error) {
  cJSON *root = parse(text, error);
  if (!root)
    return NULL;
  cJSON *ws = NULL;
  if (legacy) {
    ws = cJSON_DetachItemFromObject(root, "windows");
  } else if (json_get_int(root, "version", 1) >= 2) {
    ws = cJSON_DetachItemFromObject(root, "workspaces");
  } else {
    cJSON *v2 = session_json_to_v2(root, error);
    ws = v2 ? cJSON_DetachItemFromObject(v2, "workspaces") : NULL;
    cJSON_Delete(v2);
    if (!ws) {
      cJSON_Delete(root);
      return NULL;
    }
  }
  cJSON_Delete(root);
  if (!ws || !cJSON_IsArray(ws)) {
    cJSON_Delete(ws);
    ws = cJSON_CreateArray();
  }
  cJSON *w;
  cJSON_ArrayForEach(w, ws) geometry_under(w, frontend && *frontend ? frontend : "gtk");
  return ws;
}

char *lace_session_json_merge(const char *existing_text, cJSON *workspaces, int current, char **error) {
  cJSON *root = NULL;
  if (existing_text && *existing_text) {
    root = parse(existing_text, error);
    if (!root) {
      cJSON_Delete(workspaces);
      return NULL;
    }
  } else {
    root = cJSON_CreateObject();
  }
  cJSON_DeleteItemFromObject(root, "version");
  cJSON_DeleteItemFromObject(root, "workspaces");
  cJSON_DeleteItemFromObject(root, "current");
  cJSON_DeleteItemFromObject(root, "windows"); /* gtk-session.json's name for them */
  cJSON_AddNumberToObject(root, "version", 2);
  cJSON_AddItemToObject(root, "workspaces", workspaces ? workspaces : cJSON_CreateArray());
  cJSON_AddNumberToObject(root, "current", current);
  char *text = cJSON_Print(root);
  cJSON_Delete(root);
  return text;
}
