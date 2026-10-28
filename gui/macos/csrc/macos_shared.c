/*
 * Lace macOS frontend
 * See macos_shared.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "macos_shared.h"
#include "config/session_json.h"
#include <util/str.h>
#include <cjson/cJSON.h>
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
