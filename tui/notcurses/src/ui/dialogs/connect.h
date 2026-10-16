#ifndef NC_CONNECT_H
#define NC_CONNECT_H
#include "app.h"

/* The picker overlay: input, saved connections, recent URLs. Enter connects
 * as a new tab, t as a tab, w in a new workspace, e edits, n adds, x deletes,
 * s saves a recent URL. (Declared in app.h too.) */
void connect_open(NcApp *app);
/* The new/edit connection form. edit is a heap copy the form takes over
 * (an existing saved id edits that entry, a new id adds one); NULL: blank. */
void connect_form_open(NcApp *app, LaceSavedConnection *edit_or_null);
/* Validation shared with the tests: an error message or NULL.
 * port: empty or 1..65535; host: no "/ ? # @" or whitespace */
char *connect_check_port(const char *text, const char *what);
char *connect_check_host(const char *text, const char *what);
#endif
