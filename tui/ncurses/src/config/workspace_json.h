/*
 * Lace
 * Workspace JSON shapes: the ncurses TUI's flat tab list (session.json
 * version 1) and the nested window object every frontend shares (version
 * 2). Pure cJSON; see docs/superpowers/specs/2026-10-15-shared-config-session-design.md
 */
#ifndef LACE_CONFIG_WORKSPACE_JSON_H
#define LACE_CONFIG_WORKSPACE_JSON_H

#include <cjson/cJSON.h>

typedef enum {
  WORKSPACE_JSON_FLAT,    /* {"tabs": [...]} */
  WORKSPACE_JSON_NESTED,  /* {"connections": [...]} */
  WORKSPACE_JSON_INVALID
} WorkspaceJsonShape;

WorkspaceJsonShape workspace_json_shape(const cJSON *workspace);

/* New objects; NULL with *error set when the input is not a workspace or a
 * filter uses an operator key this build does not know */
/* A connection form is a flat tab {"type": "FORM", "connection_id": ""} and
 * a nested connection {"form": true, "order": N}, one object per form */
cJSON *workspace_json_to_nested(const cJSON *flat, char **error);
cJSON *workspace_json_to_flat(const cJSON *nested, char **error);

/* Whole session.json roots: v1 (no "version", flat workspaces, "settings",
 * "current_workspace") <-> v2 ("version": 2, nested workspaces, "ncurses",
 * "current"). Other top-level members are copied as they are. */
cJSON *session_json_to_v2(const cJSON *root, char **error);
cJSON *session_json_to_v1(const cJSON *root, char **error);

/* FilterOperator integer <-> lace_filter_op_key() string; -1 / NULL unknown */
int workspace_json_op_from_key(const char *key);
const char *workspace_json_op_key(int op);

#endif
