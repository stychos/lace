/*
 * Lace TUI
 * SQL Query History - TUI-specific persistence functions
 *
 * Core history operations are provided by liblace.
 * This file implements file persistence for the TUI.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "history.h"
#include "../platform/platform.h"
#include "util/json_helpers.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include <cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define HISTORY_DIR "history"
#define HISTORY_VERSION 1

/* ============================================================================
 * Persistence
 * ============================================================================
 */

char *history_get_file_path(const char *connection_id) {
  if (!connection_id || !connection_id[0])
    return NULL;

  const char *data_dir = platform_get_data_dir();
  if (!data_dir)
    return NULL;

  return str_printf("%s%s%s%s%s.json", data_dir, LACE_PATH_SEP_STR, HISTORY_DIR,
                    LACE_PATH_SEP_STR, connection_id);
}

bool history_ensure_dir(char **error) {
  const char *data_dir = platform_get_data_dir();
  if (!data_dir) {
    err_setf(error, "Failed to get data directory");
    return false;
  }

  char *history_dir =
      str_printf("%s%s%s", data_dir, LACE_PATH_SEP_STR, HISTORY_DIR);
  if (!history_dir) {
    err_setf(error, "Out of memory");
    return false;
  }

  if (!platform_dir_exists(history_dir)) {
    if (!platform_mkdir(history_dir)) {
      err_setf(error, "Failed to create history directory: %s", history_dir);
      free(history_dir);
      return false;
    }
  }

  free(history_dir);
  return true;
}

bool history_load(QueryHistory *history, char **error) {
  if (!history || !history->connection_id) {
    err_setf(error, "Invalid history object");
    return false;
  }

  /* Clear existing entries */
  lace_history_clear(history);

  char *path = history_get_file_path(history->connection_id);
  if (!path) {
    err_setf(error, "Failed to get history file path");
    return false;
  }

  /* Check if file exists */
  if (!platform_file_exists(path)) {
    /* No history file yet - that's fine */
    free(path);
    return true;
  }

  /* Read file */
  FILE *f = fopen(path, "r");
  if (!f) {
    err_setf(error, "Failed to open %s: %s", path, strerror(errno));
    free(path);
    return false;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0) {
    fclose(f);
    free(path);
    return true; /* Empty file is valid */
  }

  char *content = safe_malloc((size_t)size + 1);

  size_t read_size = fread(content, 1, (size_t)size, f);
  fclose(f);
  content[read_size] = '\0';

  /* Parse JSON */
  cJSON *json = cJSON_Parse(content);
  free(content);

  if (!json) {
    err_setf(error, "Failed to parse history JSON: %s", path);
    free(path);
    return false;
  }

  free(path);

  /* Parse entries array */
  cJSON *entries = json_get_array(json, "entries");
  if (!entries) {
    cJSON_Delete(json);
    return true; /* No entries is valid */
  }

  cJSON *entry_json;
  cJSON_ArrayForEach(entry_json, entries) {
    const char *sql = json_get_string(entry_json, "sql", NULL);
    if (!sql)
      continue;

    const char *type_str = json_get_string(entry_json, "type", NULL);
    LaceHistoryType type = lace_history_type_from_string(type_str);

    /* Add entry using liblace function (0 = no max limit during load) */
    lace_history_add(history, sql, type, 0);

    /* Update timestamp on the just-added entry */
    if (history->num_entries > 0) {
      history->entries[history->num_entries - 1].timestamp =
          (time_t)json_get_int64(entry_json, "timestamp", (int64_t)time(NULL));
    }
  }

  cJSON_Delete(json);
  return true;
}

bool history_save(const QueryHistory *history, char **error) {
  if (!history || !history->connection_id) {
    err_setf(error, "Invalid history object");
    return false;
  }

  /* Ensure directory exists */
  if (!history_ensure_dir(error))
    return false;

  char *path = history_get_file_path(history->connection_id);
  if (!path) {
    err_setf(error, "Failed to get history file path");
    return false;
  }

  /* Build JSON */
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    free(path);
    err_setf(error, "Failed to create JSON object");
    return false;
  }

  cJSON_AddNumberToObject(json, "version", HISTORY_VERSION);
  cJSON_AddStringToObject(json, "connection_id", history->connection_id);

  cJSON *entries = cJSON_CreateArray();
  if (!entries) {
    cJSON_Delete(json);
    free(path);
    err_setf(error, "Failed to create entries array");
    return false;
  }

  for (size_t i = 0; i < history->num_entries; i++) {
    const LaceHistoryEntry *entry = &history->entries[i];
    if (!entry->sql)
      continue;

    cJSON *entry_json = cJSON_CreateObject();
    if (!entry_json)
      continue;

    cJSON_AddStringToObject(entry_json, "sql", entry->sql);
    cJSON_AddNumberToObject(entry_json, "timestamp", (double)entry->timestamp);
    cJSON_AddStringToObject(entry_json, "type",
                            lace_history_type_to_string(entry->type));

    cJSON_AddItemToArray(entries, entry_json);
  }

  cJSON_AddItemToObject(json, "entries", entries);

  /* Write to file */
  char *content = cJSON_Print(json);
  cJSON_Delete(json);

  if (!content) {
    free(path);
    err_setf(error, "Failed to serialize JSON");
    return false;
  }

  FILE *f = fopen(path, "w");
  if (!f) {
    free(content);
    err_setf(error, "Failed to open %s for writing: %s", path,
              strerror(errno));
    free(path);
    return false;
  }

  /* Set file permissions to 0600 (owner read/write only) */
#ifndef LACE_OS_WINDOWS
  chmod(path, 0600);
#endif

  size_t len = strlen(content);
  size_t written = fwrite(content, 1, len, f);
  fclose(f);
  free(content);
  free(path);

  if (written != len) {
    err_setf(error, "Failed to write all data");
    return false;
  }

  return true;
}
