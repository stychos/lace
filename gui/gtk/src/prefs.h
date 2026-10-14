/*
 * Lace GTK frontend
 * Preferences (gtk.json in the shared config directory)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_PREFS_H
#define LACE_GTK_PREFS_H

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
} LacePrefs;

/* Whether a grid of this table shows row numbers under the preference */
bool lace_prefs_row_numbers_for(const LacePrefs *p, const LaceSchema *schema);

/* Defaults, then whatever gtk.json holds */
LacePrefs *lace_prefs_load(void);
bool lace_prefs_save(const LacePrefs *p, char **error);
void lace_prefs_free(LacePrefs *p);

/* Path of gtk.json (allocated) */
char *lace_prefs_path(void);

#endif /* LACE_GTK_PREFS_H */
