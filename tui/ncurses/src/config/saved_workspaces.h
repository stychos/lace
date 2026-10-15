/*
 * Lace
 * Saved workspaces: named workspace layouts (tabs, tables, sorts, filters,
 * query text, by saved connection id) kept in workspaces.json, to open again
 * later
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONFIG_SAVED_WORKSPACES_H
#define LACE_CONFIG_SAVED_WORKSPACES_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define SAVED_WORKSPACES_FILE "workspaces.json"

typedef struct {
  char *name;
  time_t saved;
  cJSON *workspace; /* As session.json holds a workspace */
} SavedWorkspace;

typedef struct {
  SavedWorkspace *items;
  size_t count;
  char *load_error; /* The file exists but could not be read: never written */
} SavedWorkspaces;

/* Load workspaces.json (missing: an empty list) */
void saved_workspaces_load(SavedWorkspaces *list);

/* Write the list (refused after a failed load) */
bool saved_workspaces_save(const SavedWorkspaces *list, char **error);

/* Index of the layout named name, or SIZE_MAX */
size_t saved_workspaces_find(const SavedWorkspaces *list, const char *name);

/* Store a layout under name, replacing one of that name; takes workspace */
void saved_workspaces_put(SavedWorkspaces *list, const char *name, cJSON *workspace);

void saved_workspaces_remove(SavedWorkspaces *list, size_t index);
void saved_workspaces_free(SavedWorkspaces *list);

#endif /* LACE_CONFIG_SAVED_WORKSPACES_H */
