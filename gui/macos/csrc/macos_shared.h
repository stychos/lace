/*
 * Lace macOS frontend
 * The shared C code's calls Swift cannot make as they are: session.json's
 * JSON layer (config/session_json.c) and workspaces.json
 * (config/saved_workspaces.c) as text in and out, so cJSON stays on the C
 * side, and the connection form's checkboxes (an array in ConnFields).
 * Compiled into build/shared/liblaceshared.a by the Makefile.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_MACOS_SHARED_H
#define LACE_MACOS_SHARED_H

#include "../../../tui/ncurses/src/core/conn_fields.h"
#include <stdbool.h>

/* The workspaces of a session file's text (version 2, a version-1 file
 * converted, or legacy macos-session.json's "windows" with their geometry
 * moved under "macos") as a JSON array; NULL and *error when the text does
 * not parse. Caller frees. */
char *lace_macos_session_workspaces(const char *text, bool legacy, char **error);

/* The text to write: existing's other members kept, version 2, the
 * workspaces (a JSON array's text) and current. NULL and *error when
 * existing is not empty and does not parse, or workspaces is not an array.
 * Caller frees. */
char *lace_macos_session_merge(const char *existing, const char *workspaces_json, int current, char **error);

/* workspaces.json's saved layouts as a JSON array of {"name", "saved" (unix
 * time), "workspace" (nested, as session.json holds one: a flat ncurses
 * layout converted; one that cannot be read is left out)}; "[]" when there
 * is no file. NULL and *error when the file exists but does not parse.
 * Every call reads the file again. Caller frees. */
char *lace_macos_saved_workspaces_list(char **error);

/* Store workspace_json (one workspace object's text) under name, replacing
 * one of that name, and write the file; false and *error when it cannot be
 * read or written (a file that does not parse is never written) */
bool lace_macos_saved_workspaces_put(const char *name, const char *workspace_json, char **error);

/* Remove the layout named name (rename: new_name non-NULL, refused when a
 * layout of that name exists) and write the file; false and *error when
 * there is none of that name or the file cannot be read or written */
bool lace_macos_saved_workspaces_remove(const char *name, char **error);
bool lace_macos_saved_workspaces_rename(const char *name, const char *new_name, char **error);

/* A checkbox of the form (CF_SAVEPW, CF_SSH, CF_SAVESSHPW) */
bool lace_macos_fields_check(const ConnFields *f, int field);
void lace_macos_fields_set_check(ConnFields *f, int field, bool on);
/* The message line (conn_fields_set_message() takes a format); NULL clears it */
void lace_macos_fields_set_message(ConnFields *f, bool error, const char *text);

#endif /* LACE_MACOS_SHARED_H */
