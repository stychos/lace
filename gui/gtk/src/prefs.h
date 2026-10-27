/*
 * Lace GTK frontend
 * Preferences: the shared config.json's "general" (config/config.c), with
 * gtk.json migrated into it once
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_PREFS_H
#define LACE_GTK_PREFS_H

#include "config/config.h"
#include <lace.h>
#include <stdbool.h>

typedef enum {
  LACE_COLOR_SYSTEM = 0,
  LACE_COLOR_LIGHT = 1,
  LACE_COLOR_DARK = 2,
} LaceColorScheme;

typedef enum {
  LACE_ROW_NUMBERS_NEVER = 0,
  LACE_ROW_NUMBERS_AUTO = 1,  /* Hidden when the key is one auto-increment integer */
  LACE_ROW_NUMBERS_ALWAYS = 2,
} LaceRowNumbers;

typedef struct {
  int daemon_spawn_mode;  /* LaceSpawnMode value (0 unix, 1 stdio, 2 tcp, 3 detect) */
  int daemon_idle_timeout; /* seconds, 0 = never */
  bool confirm_delete;
  bool restore_session;
  bool approximate_counts; /* catalog estimates for big server tables */
  int history_mode;        /* LACE_HISTORY_MODE_* */
  int history_max_size;
  int color_scheme;        /* LaceColorScheme */
  int page_size;           /* rows per page */
  int row_numbers;         /* LaceRowNumbers */
  bool keep_connection_tab;   /* the last inner tab's close leaves a form */
  int last_workspace_close;   /* CONFIG_LAST_WS_*: the last window closing */
  bool auto_open_first_table; /* a new connection opens its first table */
  Config *config;             /* the shared file's contents (owned) */
} LacePrefs;

/* Whether a grid of this table shows row numbers under the preference */
bool lace_prefs_row_numbers_for(const LacePrefs *p, const LaceSchema *schema);

/* config.json's "general" (defaults for what it lacks; a file that does not
 * parse gives defaults and is never overwritten); gtk.json's values are
 * taken once for the keys "general" lacks, then it is renamed
 * gtk.json.migrated */
LacePrefs *lace_prefs_load(void);
/* The fields into "general", then config_save(): the file's other members
 * (other frontends' sections, the TUIs' hotkeys) are kept */
bool lace_prefs_save(LacePrefs *p, char **error);
void lace_prefs_free(LacePrefs *p);

/* The same with explicit paths (tests) */
LacePrefs *lace_prefs_load_from(const char *config_path, const char *legacy_path);
bool lace_prefs_save_to(LacePrefs *p, const char *config_path, char **error);

#endif /* LACE_GTK_PREFS_H */
