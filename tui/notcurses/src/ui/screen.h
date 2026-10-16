#ifndef LACE_NC_SCREEN_H
#define LACE_NC_SCREEN_H
#include "app.h"

bool screen_init(NcApp *app, char **error);
/* Re-run theme_init from the config's color scheme (and the terminal's background) */
void screen_apply_theme(NcApp *app);
void screen_stop(NcApp *app);
void screen_render(NcApp *app);   /* full redraw + notcurses_render */
void screen_resized(NcApp *app);  /* after NCKEY_RESIZE: refresh dims, relayout overlays */
#define MIN_ROWS 10
#define MIN_COLS 40
bool screen_too_small(const NcApp *app); /* below 40x10: only "Terminal too small" is drawn */
int screen_rows(const NcApp *app);
int screen_cols(const NcApp *app);
#endif
