/* The connection form tab: what a ConnTab without a connection shows. The
 * saved connections and Recent on the left, the URI line and the fields on
 * the right, the open and manage buttons under them. */
#ifndef LACE_NC_FORM_TAB_H
#define LACE_NC_FORM_TAB_H

#include "app.h"

typedef struct FormTab FormTab;

FormTab *form_tab_new(void);
void form_tab_free(FormTab *f);
/* The tab's label: the highlighted saved connection's name, else "New connection" */
const char *form_tab_label(const FormTab *f);
/* The form shows connstr in its fields and URI line (not dirty, no row highlighted) */
void form_tab_set_connstr(FormTab *f, const char *connstr);
/* The content area rows y0 .. y0+rows-1; true when a text field wants the
 * cursor (at *cursor_y, *cursor_x) */
bool form_tab_draw(NcApp *app, ConnTab *t, int y0, int rows, int *cursor_y, int *cursor_x);
/* false: not the form's key, the global hotkeys get it */
bool form_tab_handle_key(NcApp *app, ConnTab *t, const UiEvent *ev);
bool form_tab_handle_mouse(NcApp *app, ConnTab *t, const UiEvent *ev);
/* The form's actions (HOTKEY_CONN_*), from keys, buttons and status hints. An
 * open here frees the form: nothing of it may be used afterwards. */
bool form_tab_do_action(NcApp *app, ConnTab *t, HotkeyAction a);
bool form_tab_list_focused(const ConnTab *t);
#endif
