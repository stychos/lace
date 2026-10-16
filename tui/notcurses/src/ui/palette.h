/* The command palette: one filterable list of every action, table, view,
 * workspace and connection. (The help overlay is dialogs/help.c.) */
#ifndef NC_PALETTE_H
#define NC_PALETTE_H
#include "app.h"

/* -1 no match; higher is better: prefix, word start, substring, subsequence
 * per space-separated word, less a little for a long text. Case-insensitive. */
int palette_score(const char *needle, const char *hay);
/* Whether the palette lists an action now: it applies (app_action_applies)
 * and runs from the main screen, not only inside an editor or a dialog */
bool palette_lists_action(const NcApp *app, HotkeyAction a);
void palette_open(NcApp *app);
#endif
