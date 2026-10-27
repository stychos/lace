/*
 * Lace
 * session.json's JSON layer for the GUI frontends (GTK, macOS): reading the workspaces out of a
 * session file of any shape, and writing them into the shared version-2
 * root without touching what other frontends keep there
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_SESSION_JSON_H
#define LACE_SESSION_JSON_H

#include <cjson/cJSON.h>
#include <stdbool.h>

/* The workspace objects in a session file's text: version 2's "workspaces";
 * a version-1 (ncurses) file converted with session_json_to_v2(); legacy
 * (gtk-session.json, macos-session.json) its "windows". Window geometry
 * found at a workspace's top level (width, height, frame, maximized) moves
 * under the frontend's own member ("gtk", "macos"). NULL (and *error) for
 * text that does not parse. Caller deletes. */
cJSON *lace_session_json_workspaces(const char *text, bool legacy, const char *frontend, char **error);

/* The text to write: existing_text's other top-level members kept, version
 * 2, "workspaces" (taken over) and "current". NULL (and *error) when
 * existing_text is not empty and does not parse: it is never overwritten.
 * Caller frees. */
char *lace_session_json_merge(const char *existing_text, cJSON *workspaces, int current, char **error);

#endif /* LACE_SESSION_JSON_H */
