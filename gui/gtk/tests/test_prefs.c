/*
 * Lace GTK frontend
 * Preferences over the shared config.json: migration from gtk.json, other
 * frontends' sections kept, an unparsable file never overwritten
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "prefs.h"
#include "test.h"
#include <cjson/cJSON.h>
#include <glib.h>
#include <glib/gstdio.h>

static char *g_dir;

static char *path_in(const char *name) { return g_build_filename(g_dir, name, NULL); }

static void put(const char *name, const char *text) {
  char *p = path_in(name);
  g_file_set_contents(p, text, -1, NULL);
  g_free(p);
}

static cJSON *read_json(const char *name) {
  char *p = path_in(name), *text = NULL;
  g_file_get_contents(p, &text, NULL, NULL);
  g_free(p);
  cJSON *j = text ? cJSON_Parse(text) : NULL;
  g_free(text);
  return j;
}

static void reset(void) {
  const char *names[] = {"config.json", "gtk.json", "gtk.json.migrated"};
  for (gsize i = 0; i < G_N_ELEMENTS(names); i++) {
    char *p = path_in(names[i]);
    g_remove(p);
    g_free(p);
  }
}

static void test_prefs_migrates_gtk_json(void) {
  reset();
  put("gtk.json", "{\"confirm_delete\": false, \"page_size\": 500, \"color_scheme\": 2}");
  char *cfg = path_in("config.json"), *legacy = path_in("gtk.json");
  LacePrefs *p = lace_prefs_load_from(cfg, legacy);
  CHECK(p && !p->confirm_delete);
  CHECK(p && p->page_size == 500);
  CHECK(p && p->color_scheme == 2);
  CHECK(!g_file_test(legacy, G_FILE_TEST_EXISTS));
  char *migrated = path_in("gtk.json.migrated");
  CHECK(g_file_test(migrated, G_FILE_TEST_EXISTS));
  g_free(migrated);
  cJSON *j = read_json("config.json");
  cJSON *g = j ? cJSON_GetObjectItem(j, "general") : NULL;
  CHECK(g && cJSON_IsFalse(cJSON_GetObjectItem(g, "delete_confirmation")));
  CHECK(g && cJSON_GetObjectItem(g, "page_size") && cJSON_GetObjectItem(g, "page_size")->valueint == 500);
  cJSON_Delete(j);
  lace_prefs_free(p);
  g_free(cfg);
  g_free(legacy);
}

static void test_prefs_keep_others(void) {
  reset();
  const char *orig = "{\"general\": {\"show_header\": false, \"quit_confirmation\": true, \"page_size\": 1000},"
                     " \"hotkeys\": {\"palette\": [\"CTRL+P\"]}, \"notcurses\": {\"clipboard_osc52\": false},"
                     " \"macos\": {\"x\": 1}}";
  put("config.json", orig);
  char *cfg = path_in("config.json"), *legacy = path_in("gtk.json");
  LacePrefs *p = lace_prefs_load_from(cfg, legacy);
  p->page_size = 2000;
  char *err = NULL;
  CHECK(lace_prefs_save_to(p, cfg, &err));
  g_free(err);
  cJSON *before = cJSON_Parse(orig), *after = read_json("config.json");
  const char *kept[] = {"hotkeys", "notcurses", "macos"};
  for (gsize i = 0; i < G_N_ELEMENTS(kept); i++)
    CHECK(after && cJSON_Compare(cJSON_GetObjectItem(before, kept[i]), cJSON_GetObjectItem(after, kept[i]), 1));
  cJSON *g = after ? cJSON_GetObjectItem(after, "general") : NULL;
  CHECK(g && cJSON_IsFalse(cJSON_GetObjectItem(g, "show_header")));
  CHECK(g && cJSON_IsTrue(cJSON_GetObjectItem(g, "quit_confirmation")));
  CHECK(g && cJSON_GetObjectItem(g, "page_size") && cJSON_GetObjectItem(g, "page_size")->valueint == 2000);
  cJSON_Delete(before);
  cJSON_Delete(after);
  lace_prefs_free(p);
  g_free(cfg);
  g_free(legacy);
}

static void test_prefs_unparsable_kept(void) {
  reset();
  put("config.json", "{\"general\": ");
  char *cfg = path_in("config.json"), *legacy = path_in("gtk.json");
  LacePrefs *p = lace_prefs_load_from(cfg, legacy);
  CHECK(p && p->page_size == 1000 && p->confirm_delete);
  char *err = NULL;
  CHECK(!lace_prefs_save_to(p, cfg, &err));
  CHECK(err != NULL);
  g_free(err);
  char *text = NULL;
  g_file_get_contents(cfg, &text, NULL, NULL);
  CHECK_STR(text, "{\"general\": ");
  g_free(text);
  lace_prefs_free(p);
  g_free(cfg);
  g_free(legacy);
}

static void test_prefs_connection_tab_keys(void) {
  reset();
  put("config.json", "{\"general\": {\"keep_connection_tab\": false, \"on_last_workspace_close\": \"new\","
                     " \"auto_open_first_table\": true}}");
  char *cfg = path_in("config.json"), *legacy = path_in("gtk.json");
  LacePrefs *p = lace_prefs_load_from(cfg, legacy);
  CHECK(p && !p->keep_connection_tab);
  CHECK(p && p->last_workspace_close == CONFIG_LAST_WS_NEW);
  CHECK(p && p->auto_open_first_table);
  char *err = NULL;
  CHECK(lace_prefs_save_to(p, cfg, &err));
  g_free(err);
  cJSON *j = read_json("config.json");
  cJSON *g = j ? cJSON_GetObjectItem(j, "general") : NULL;
  CHECK(g && cJSON_IsFalse(cJSON_GetObjectItem(g, "keep_connection_tab")));
  CHECK(g && cJSON_IsString(cJSON_GetObjectItem(g, "on_last_workspace_close")) &&
        g_str_equal(cJSON_GetObjectItem(g, "on_last_workspace_close")->valuestring, "new"));
  CHECK(g && cJSON_IsTrue(cJSON_GetObjectItem(g, "auto_open_first_table")));
  cJSON_Delete(j);
  lace_prefs_free(p);
  g_free(cfg);
  g_free(legacy);
}

int main(void) {
  g_dir = g_dir_make_tmp("lace-gtk-prefs-XXXXXX", NULL);
  test_prefs_migrates_gtk_json();
  test_prefs_keep_others();
  test_prefs_unparsable_kept();
  test_prefs_connection_tab_keys();
  reset();
  g_rmdir(g_dir);
  g_free(g_dir);
  TEST_MAIN_END();
}
