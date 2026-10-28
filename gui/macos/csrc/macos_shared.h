/*
 * Lace macOS frontend
 * The shared C code's calls Swift cannot make as they are: session.json's
 * JSON layer (config/session_json.c) as text in and out, so cJSON stays on
 * the C side, and the connection form's checkboxes (an array in ConnFields).
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

/* A checkbox of the form (CF_SAVEPW, CF_SSH, CF_SAVESSHPW) */
bool lace_macos_fields_check(const ConnFields *f, int field);
void lace_macos_fields_set_check(ConnFields *f, int field, bool on);
/* The message line (conn_fields_set_message() takes a format); NULL clears it */
void lace_macos_fields_set_message(ConnFields *f, bool error, const char *text);

#endif /* LACE_MACOS_SHARED_H */
