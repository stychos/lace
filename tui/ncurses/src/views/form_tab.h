/*
 * Lace
 * The connection form tab: the saved connections tree and Recent on the
 * left, the URI line, the fields and the buttons on the right
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FORM_TAB_H
#define LACE_FORM_TAB_H

#include "../tui.h"

/* Draw a form tab into main_win (its state is made here the first time) */
void form_tab_draw(TuiState *state, Tab *tab);
/* A key for the form; false for a key left to the global hotkeys */
bool form_tab_handle_key(TuiState *state, Tab *tab, const UiEvent *ev);
/* A mouse event at (y, x) inside main_win; wheel: -1 up, 1 down, 0 none */
bool form_tab_handle_mouse(TuiState *state, Tab *tab, int y, int x, bool double_click, int wheel);
/* The buttons' and the panel's actions (conn_open*, conn_save*, conn_test,
 * conn_clear, conn_new_folder, conn_rename, ...); false if not one */
bool form_tab_do_action(TuiState *state, Tab *tab, HotkeyAction a);
/* The tab's label: the highlighted saved connection's name, else "New
 * connection"; caller frees */
char *form_tab_label(const Tab *tab);
/* The keys to show on the status line */
void form_tab_status(TuiState *state, Tab *tab, char *buf, size_t n);
/* The saved connections panel has the keys (not its filter): the status
 * line shows the panel's hints */
bool form_tab_list_focused(TuiState *state, Tab *tab);
/* Fill the form from a connection string (NULL or "": empty), nothing
 * highlighted */
void form_tab_set_connstr(TuiState *state, Tab *tab, const char *connstr);
/* Frees a form's state (tab_form_free_hook) */
void form_tab_free(struct FormTab *f);

/* The saved connections and Recent of this process, loaded on first use */
ConnectionManager *tui_connmgr(TuiState *state);
RecentList *tui_recent(TuiState *state);
/* Free both (at cleanup) */
void tui_connmgr_free(TuiState *state);

#endif /* LACE_FORM_TAB_H */
