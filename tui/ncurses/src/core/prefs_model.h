/*
 * Lace
 * The TUIs' preferences: one table of the settings both preferences
 * dialogs and command palettes show, changed in place (the caller saves
 * the config). A row may belong to one frontend only (CONFIG_FE_*): the
 * notcurses strips and OSC 52, the ncurses header and status bars and
 * pagination. A frontend's own on/off settings live in its config.json
 * section ("notcurses", "ncurses").
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_PREFS_MODEL_H
#define LACE_PREFS_MODEL_H

#include "../config/config.h"
#include <stdbool.h>

typedef enum {
  PREF_BOOL, /* a bool in general */
  PREF_ENUM, /* an int in general with a name per value */
  PREF_NUM,  /* an int in general cycled through steps */
  PREF_SECT, /* a bool in the frontend's own section */
  PREF_OPEN, /* a row that opens another dialog (the hotkeys editor) */
} PrefKind;

int prefs_model_count(void);
const char *prefs_model_name(int i);
PrefKind prefs_model_kind(int i);
/* The row is shown by the frontend (CONFIG_FE_*) */
bool prefs_model_for(int i, unsigned frontend);
/* An on/off row (the palettes list them) */
bool prefs_model_is_toggle(int i);
/* The colour scheme row: the theme follows a change */
bool prefs_model_is_scheme(int i);
/* The value as shown: "on", "quit", "1000", "0s"; caller frees */
char *prefs_model_value(const Config *c, int i, const char *section);
/* "Name: value" (a palette row); caller frees */
char *prefs_model_toggle_text(const Config *c, int i, const char *section);
/* Flip a bool, or step an enum or a number by dir (+1 / -1); nothing for
 * PREF_OPEN. section: the frontend's own ("notcurses", "ncurses"). */
void prefs_model_change(Config *c, int i, int dir, const char *section);
/* A bool of the frontend's section, dflt when absent */
bool prefs_model_section_bool(const Config *c, const char *section, const char *key, bool dflt);

#endif /* LACE_PREFS_MODEL_H */
