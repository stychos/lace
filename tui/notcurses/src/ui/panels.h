/* The list overlays: log, query history, preferences, view switcher */
#ifndef NC_PANELS_H
#define NC_PANELS_H
#include "app.h"

void log_open(NcApp *app);           /* ui/log.c: app->log, newest at the bottom */
void history_open(NcApp *app);       /* ui/dialogs/history.c: the current connection's history */
void prefs_open(NcApp *app);         /* ui/dialogs/prefs.c */
/* The preferences dialog's settings, for the palette: the on/off ones by
 * index, their "Name: on" text (allocated), and a toggle that saves the
 * config the way the dialog does */
int prefs_count(void);
bool prefs_is_toggle(int i);
char *prefs_toggle_text(NcApp *app, int i);
void prefs_toggle(NcApp *app, int i);
/* A boolean of the frontend's own config section (notcurses.<key>), dflt when unset */
bool prefs_section_bool(const Config *config, const char *key, bool dflt);
void view_switcher_open(NcApp *app); /* ui/dialogs/view_switcher.c */
#endif
