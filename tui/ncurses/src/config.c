/*
 * Lace ncurses frontend
 * Configuration and hotkeys
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config.h"
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <cjson/cJSON.h>

/* ==========================================================================
 * Default Key Bindings
 * ========================================================================== */

typedef struct {
  HotkeyAction action;
  const char *name;
  int default_keys[MAX_KEYS_PER_ACTION];
  size_t num_default;
} DefaultBinding;

/* Note: key values use ncurses KEY_* constants and ASCII values */
static const DefaultBinding defaults[] = {
  /* Navigation */
  {HOTKEY_MOVE_UP, "move_up", {'k', KEY_UP}, 2},
  {HOTKEY_MOVE_DOWN, "move_down", {'j', KEY_DOWN}, 2},
  {HOTKEY_MOVE_LEFT, "move_left", {'h', KEY_LEFT}, 2},
  {HOTKEY_MOVE_RIGHT, "move_right", {'l', KEY_RIGHT}, 2},
  {HOTKEY_PAGE_UP, "page_up", {KEY_PPAGE}, 1},
  {HOTKEY_PAGE_DOWN, "page_down", {KEY_NPAGE}, 1},
  {HOTKEY_FIRST_ROW, "first_row", {'g', 'a'}, 2},
  {HOTKEY_LAST_ROW, "last_row", {'G', 'z'}, 2},
  {HOTKEY_GOTO_ROW, "goto_row", {'/', KEY_F(5)}, 2},
  {HOTKEY_FIRST_COL, "first_col", {KEY_HOME}, 1},
  {HOTKEY_LAST_COL, "last_col", {KEY_END}, 1},

  /* Table Viewer */
  {HOTKEY_EDIT_INLINE, "edit_inline", {'\n', KEY_ENTER}, 2},
  {HOTKEY_EDIT_MODAL, "edit_modal", {'e', KEY_F(4)}, 2},
  {HOTKEY_SET_NULL, "set_null", {'n', 14}, 2},  /* 14 = Ctrl+N */
  {HOTKEY_SET_EMPTY, "set_empty", {'d', 4}, 2}, /* 4 = Ctrl+D */
  {HOTKEY_DELETE_ROW, "delete_row", {'x', KEY_DC}, 2},
  {HOTKEY_TOGGLE_FILTERS, "toggle_filters", {'f', '/'}, 2},
  {HOTKEY_TOGGLE_SIDEBAR, "toggle_sidebar", {'t', KEY_F(9)}, 2},
  {HOTKEY_SHOW_SCHEMA, "show_schema", {'s', KEY_F(3)}, 2},
  {HOTKEY_REFRESH, "refresh", {'r', KEY_F(5)}, 2},

  /* General */
  {HOTKEY_PREV_TAB, "prev_tab", {'[', KEY_F(7)}, 2},
  {HOTKEY_NEXT_TAB, "next_tab", {']', KEY_F(6)}, 2},
  {HOTKEY_CLOSE_TAB, "close_tab", {'-'}, 1},
  {HOTKEY_NEW_TAB, "new_tab", {'+', '='}, 2},
  {HOTKEY_HELP, "help", {'?', KEY_F(1)}, 2},
  {HOTKEY_QUIT, "quit", {'q', 24, KEY_F(10)}, 3},  /* 24 = Ctrl+X */

  /* Query */
  {HOTKEY_OPEN_QUERY, "open_query", {'p'}, 1},
  {HOTKEY_EXECUTE_QUERY, "execute_query", {18}, 1},  /* 18 = Ctrl+R */
  {HOTKEY_EXECUTE_ALL, "execute_all", {1}, 1},       /* 1 = Ctrl+A */
  {HOTKEY_QUERY_SWITCH_FOCUS, "query_switch_focus", {23, 27}, 2},  /* 23=Ctrl+W, 27=Esc */

  /* Filters */
  {HOTKEY_ADD_FILTER, "add_filter", {'+', '='}, 2},
  {HOTKEY_REMOVE_FILTER, "remove_filter", {'-', 'x', KEY_DC}, 3},
  {HOTKEY_CLEAR_FILTERS, "clear_filters", {'c'}, 1},

  /* Connection */
  {HOTKEY_CONNECT, "connect", {'w', KEY_F(2)}, 2},
};

static const size_t num_defaults = sizeof(defaults) / sizeof(defaults[0]);

/* ==========================================================================
 * Helper Functions
 * ========================================================================== */

char *config_get_dir(void) {
  const char *home = getenv("HOME");
  if (!home) return NULL;

  char *path = malloc(strlen(home) + 32);
  if (!path) return NULL;

  snprintf(path, strlen(home) + 32, "%s/.config/lace", home);
  return path;
}

static void init_default_bindings(Config *cfg) {
  /* Clear all bindings */
  memset(cfg->hotkeys, 0, sizeof(cfg->hotkeys));

  /* Set defaults */
  for (size_t i = 0; i < num_defaults; i++) {
    HotkeyBinding *b = &cfg->hotkeys[defaults[i].action];
    b->num_keys = defaults[i].num_default;
    for (size_t j = 0; j < b->num_keys && j < MAX_KEYS_PER_ACTION; j++) {
      b->keys[j] = defaults[i].default_keys[j];
    }
  }
}

/* ==========================================================================
 * Configuration Functions
 * ========================================================================== */

Config *config_create(void) {
  Config *cfg = calloc(1, sizeof(Config));
  if (!cfg) return NULL;

  /* Set defaults */
  init_default_bindings(cfg);
  cfg->show_header = true;
  cfg->show_status = true;
  cfg->sidebar_width = 24;
  cfg->confirm_quit = true;
  cfg->confirm_delete = true;
  cfg->page_size = 500;

  /* Set config path */
  char *dir = config_get_dir();
  if (dir) {
    cfg->config_path = malloc(strlen(dir) + 32);
    if (cfg->config_path) {
      snprintf(cfg->config_path, strlen(dir) + 32, "%s/ncurses/config.json", dir);
    }
    free(dir);
  }

  return cfg;
}

void config_free(Config *cfg) {
  if (!cfg) return;
  free(cfg->config_path);
  free(cfg);
}

Config *config_load(const char *path) {
  Config *cfg = config_create();
  if (!cfg) return NULL;

  if (!path) path = cfg->config_path;
  if (!path) return cfg;

  /* Try to read file */
  FILE *f = fopen(path, "r");
  if (!f) return cfg;  /* Use defaults */

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0 || size > 1024 * 1024) {
    fclose(f);
    return cfg;
  }

  char *data = malloc((size_t)size + 1);
  if (!data) {
    fclose(f);
    return cfg;
  }

  size_t read = fread(data, 1, (size_t)size, f);
  fclose(f);
  data[read] = '\0';

  /* Parse JSON */
  cJSON *json = cJSON_Parse(data);
  free(data);
  if (!json) return cfg;

  /* Load appearance settings */
  cJSON *appearance = cJSON_GetObjectItem(json, "appearance");
  if (appearance) {
    cJSON *item;
    if ((item = cJSON_GetObjectItem(appearance, "show_header"))) {
      cfg->show_header = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(appearance, "show_status"))) {
      cfg->show_status = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(appearance, "sidebar_width"))) {
      cfg->sidebar_width = item->valueint;
    }
  }

  /* Load behavior settings */
  cJSON *behavior = cJSON_GetObjectItem(json, "behavior");
  if (behavior) {
    cJSON *item;
    if ((item = cJSON_GetObjectItem(behavior, "confirm_quit"))) {
      cfg->confirm_quit = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(behavior, "confirm_delete"))) {
      cfg->confirm_delete = cJSON_IsTrue(item);
    }
  }

  /* Load pagination settings */
  cJSON *pagination = cJSON_GetObjectItem(json, "pagination");
  if (pagination) {
    cJSON *item;
    if ((item = cJSON_GetObjectItem(pagination, "page_size"))) {
      cfg->page_size = item->valueint;
      if (cfg->page_size < 50) cfg->page_size = 50;
      if (cfg->page_size > 10000) cfg->page_size = 10000;
    }
  }

  cJSON_Delete(json);
  return cfg;
}

bool config_save(Config *cfg) {
  if (!cfg || !cfg->config_path) return false;

  /* Create directories if needed */
  char *dir = config_get_dir();
  if (dir) {
    mkdir(dir, 0755);
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/ncurses", dir);
    mkdir(subdir, 0755);
    free(dir);
  }

  /* Build JSON */
  cJSON *json = cJSON_CreateObject();
  if (!json) return false;

  cJSON *appearance = cJSON_CreateObject();
  cJSON_AddBoolToObject(appearance, "show_header", cfg->show_header);
  cJSON_AddBoolToObject(appearance, "show_status", cfg->show_status);
  cJSON_AddNumberToObject(appearance, "sidebar_width", cfg->sidebar_width);
  cJSON_AddItemToObject(json, "appearance", appearance);

  cJSON *behavior = cJSON_CreateObject();
  cJSON_AddBoolToObject(behavior, "confirm_quit", cfg->confirm_quit);
  cJSON_AddBoolToObject(behavior, "confirm_delete", cfg->confirm_delete);
  cJSON_AddItemToObject(json, "behavior", behavior);

  cJSON *pagination = cJSON_CreateObject();
  cJSON_AddNumberToObject(pagination, "page_size", cfg->page_size);
  cJSON_AddItemToObject(json, "pagination", pagination);

  char *str = cJSON_Print(json);
  cJSON_Delete(json);
  if (!str) return false;

  FILE *f = fopen(cfg->config_path, "w");
  if (!f) {
    free(str);
    return false;
  }

  fputs(str, f);
  fclose(f);
  free(str);
  return true;
}

bool hotkey_matches(Config *cfg, int key, HotkeyAction action) {
  if (!cfg || action >= HOTKEY_COUNT) return false;

  HotkeyBinding *b = &cfg->hotkeys[action];
  for (size_t i = 0; i < b->num_keys; i++) {
    if (b->keys[i] == key) return true;
  }
  return false;
}

char *hotkey_get_display(Config *cfg, HotkeyAction action) {
  if (!cfg || action >= HOTKEY_COUNT) return NULL;

  HotkeyBinding *b = &cfg->hotkeys[action];
  if (b->num_keys == 0) return strdup("(none)");

  int key = b->keys[0];

  /* Special keys */
  if (key >= KEY_F(1) && key <= KEY_F(12)) {
    char buf[16];
    snprintf(buf, sizeof(buf), "F%d", key - KEY_F(0));
    return strdup(buf);
  }

  switch (key) {
  case KEY_UP: return strdup("Up");
  case KEY_DOWN: return strdup("Down");
  case KEY_LEFT: return strdup("Left");
  case KEY_RIGHT: return strdup("Right");
  case KEY_PPAGE: return strdup("PgUp");
  case KEY_NPAGE: return strdup("PgDn");
  case KEY_HOME: return strdup("Home");
  case KEY_END: return strdup("End");
  case KEY_DC: return strdup("Delete");
  case KEY_ENTER:
  case '\n': return strdup("Enter");
  case 27: return strdup("Escape");
  case '\t': return strdup("Tab");
  case ' ': return strdup("Space");
  }

  /* Control keys */
  if (key >= 1 && key <= 26) {
    char buf[16];
    snprintf(buf, sizeof(buf), "Ctrl+%c", 'A' + key - 1);
    return strdup(buf);
  }

  /* Printable */
  if (key >= 32 && key < 127) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%c", key);
    return strdup(buf);
  }

  return strdup("?");
}
