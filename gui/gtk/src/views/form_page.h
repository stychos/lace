/*
 * Lace GTK frontend
 * LaceFormPage: a connection form as a window's tab (the connection tab
 * design): the saved connections and Recent on the left, the URI row over
 * the fields on the right, the open and manage buttons. Its field model is
 * the TUIs' shared ConnFields (core/conn_fields.c).
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_FORM_PAGE_H
#define LACE_GTK_FORM_PAGE_H

#include "app.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define LACE_TYPE_FORM_PAGE (lace_form_page_get_type())
G_DECLARE_FINAL_TYPE(LaceFormPage, lace_form_page, LACE, FORM_PAGE, GtkWidget)

LaceFormPage *lace_form_page_new(LaceWindow *w);
/* "New connection", or the highlighted saved connection's name; the
 * "label-changed" signal says when it changes */
const char *lace_form_page_get_label(LaceFormPage *p);
/* Typed into the URI row, then applied to the fields (as leaving the row does) */
void lace_form_page_set_uri(LaceFormPage *p, const char *text);
/* Highlight the saved connection or Recent entry with that name (through the
 * "drop the changes" question when the form holds edits); FALSE if none */
gboolean lace_form_page_pick(LaceFormPage *p, const char *name);
/* open, background, new-window, to-window:N, save, save-as-new, test, clear */
void lace_form_page_action(LaceFormPage *p, const char *action);
/* Set a field by its script key (name, driver, file, host, port, database,
 * user, password, save-password, ssh, ssh-host, ssh-port, ssh-user, ssh-key,
 * ssh-password, save-ssh-password); FALSE for an unknown key */
gboolean lace_form_page_set_field(LaceFormPage *p, const char *key, const char *value);
/* A field's text (a switch: "true"/"false"; the driver: its name); NULL for an unknown key */
char *lace_form_page_get_field(LaceFormPage *p, const char *key);
const char *lace_form_page_get_uri(LaceFormPage *p);
/* The script driver: aim the list's context menu at the row called name (as
 * a right-click does), type into the list's search, run a "tree.*" action */
gboolean lace_form_page_menu_at(LaceFormPage *p, const char *name);
void lace_form_page_search(LaceFormPage *p, const char *text);
void lace_form_page_tree_action(LaceFormPage *p, const char *action);
const char *lace_form_page_get_message(LaceFormPage *p);

G_END_DECLS

#endif /* LACE_GTK_FORM_PAGE_H */
