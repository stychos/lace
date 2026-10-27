/*
 * Lace GTK frontend
 * session.json version 2: the root's shape, other frontends' members kept,
 * a version-1 (ncurses) file and gtk-session.json converted, an unparsable
 * file never overwritten
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/session_json.h"
#include "test.h"
#include <cjson/cJSON.h>
#include <glib.h>
#include <stdlib.h>

static cJSON *item(cJSON *o, const char *k) { return o ? cJSON_GetObjectItem(o, k) : NULL; }

static void test_session_round_trip(void) {
  const char *ws_text = "[{\"name\": \"W\", \"gtk\": {\"width\": 900, \"height\": 600, \"maximized\": false},"
                        " \"connections\": [{\"saved_id\": \"id-1\", \"name\": \"db\", \"sidebar\": true,"
                        " \"tabs\": [{\"type\": \"table\", \"table\": \"big\"}], \"current\": 0}]}]";
  cJSON *ws = cJSON_Parse(ws_text);
  char *err = NULL;
  char *text = lace_session_json_merge(NULL, cJSON_Duplicate(ws, 1), 0, &err);
  CHECK(text != NULL);
  cJSON *root = text ? cJSON_Parse(text) : NULL;
  CHECK(cJSON_IsNumber(item(root, "version")) && item(root, "version")->valueint == 2);
  CHECK(item(root, "windows") == NULL);
  cJSON *w0 = cJSON_GetArrayItem(item(root, "workspaces"), 0);
  CHECK(item(item(w0, "gtk"), "width") && item(item(w0, "gtk"), "width")->valueint == 900);
  cJSON *back = text ? lace_session_json_workspaces(text, false, "gtk", &err) : NULL;
  CHECK(back && cJSON_Compare(back, ws, 1));
  cJSON_Delete(back);
  cJSON_Delete(root);
  cJSON_Delete(ws);
  free(text);
  free(err);
}

static void test_session_keeps_foreign(void) {
  const char *existing = "{\"version\": 2, \"workspaces\": [], \"ncurses\": {\"a\": 1}, \"notcurses\": {\"b\": 2}}";
  char *err = NULL;
  char *text = lace_session_json_merge(existing, cJSON_CreateArray(), 0, &err);
  cJSON *root = text ? cJSON_Parse(text) : NULL, *orig = cJSON_Parse(existing);
  CHECK(root && cJSON_Compare(item(root, "ncurses"), item(orig, "ncurses"), 1));
  CHECK(root && cJSON_Compare(item(root, "notcurses"), item(orig, "notcurses"), 1));
  cJSON_Delete(root);
  cJSON_Delete(orig);
  free(text);
  free(err);
}

static void test_session_v1_converts(void) {
  const char *v1 = "{\"version\": 1, \"workspaces\": [{\"name\": \"T\", \"current_tab\": 0, \"tabs\":"
                   " [{\"type\": \"TABLE\", \"connection_id\": \"id-1\", \"table_name\": \"big\"}]}]}";
  char *err = NULL;
  cJSON *ws = lace_session_json_workspaces(v1, false, "gtk", &err);
  CHECK(ws && cJSON_GetArraySize(ws) == 1);
  cJSON *c0 = cJSON_GetArrayItem(item(cJSON_GetArrayItem(ws, 0), "connections"), 0);
  CHECK(cJSON_IsString(item(c0, "saved_id")) && g_str_equal(item(c0, "saved_id")->valuestring, "id-1"));
  cJSON *t0 = cJSON_GetArrayItem(item(c0, "tabs"), 0);
  CHECK(cJSON_IsString(item(t0, "table")) && g_str_equal(item(t0, "table")->valuestring, "big"));
  cJSON_Delete(ws);
  free(err);
}

static void test_session_legacy_geometry(void) {
  const char *legacy = "{\"version\": 1, \"windows\": [{\"name\": \"G\", \"width\": 800, \"height\": 500,"
                       " \"maximized\": true, \"connections\": []}]}";
  char *err = NULL;
  cJSON *ws = lace_session_json_workspaces(legacy, true, "gtk", &err);
  cJSON *w0 = ws ? cJSON_GetArrayItem(ws, 0) : NULL;
  CHECK(cJSON_IsTrue(item(item(w0, "gtk"), "maximized")));
  CHECK(item(item(w0, "gtk"), "width") && item(item(w0, "gtk"), "width")->valueint == 800);
  CHECK(item(w0, "width") == NULL);
  cJSON_Delete(ws);
  free(err);
}

static void test_session_unparsable_refused(void) {
  char *err = NULL;
  char *text = lace_session_json_merge("{\"version\": 2, \"work", cJSON_CreateArray(), 0, &err);
  CHECK(text == NULL);
  CHECK(err != NULL);
  free(err);
  err = NULL;
  CHECK(lace_session_json_workspaces("{\"version\": 2, \"work", false, "gtk", &err) == NULL && err);
  free(err);
}

int main(void) {
  test_session_round_trip();
  test_session_keeps_foreign();
  test_session_v1_converts();
  test_session_legacy_geometry();
  test_session_unparsable_refused();
  TEST_MAIN_END();
}
