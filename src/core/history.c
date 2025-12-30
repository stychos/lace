/*
 * Lace
 * SQL Query History - per-connection history of executed queries
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "history.h"
#include "../platform/platform.h"
#include "../util/str.h"
#include <cJSON.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define HISTORY_DIR "history"
#define HISTORY_VERSION 1

/* ============================================================================
 * Helper Functions
 * ============================================================================
 */

static void set_error(char **error, const char *fmt, ...) {
  if (!error)
    return;
  va_list args;
  va_start(args, fmt);
  *error = str_vprintf(fmt, args);
  va_end(args);
}

/* Ensure capacity for entries array */
static bool history_ensure_capacity(QueryHistory *history, size_t needed) {
  if (!history)
    return false;

  if (history->capacity >= needed)
    return true;

  size_t new_capacity =
      history->capacity == 0 ? HISTORY_INITIAL_CAPACITY : history->capacity * 2;
  while (new_capacity < needed)
    new_capacity *= 2;

  HistoryEntry *new_entries =
      realloc(history->entries, new_capacity * sizeof(HistoryEntry));
  if (!new_entries)
    return false;

  /* Zero new entries */
  memset(&new_entries[history->capacity], 0,
         (new_capacity - history->capacity) * sizeof(HistoryEntry));

  history->entries = new_entries;
  history->capacity = new_capacity;
  return true;
}

/* Free a single entry's contents */
static void entry_free(HistoryEntry *entry) {
  if (!entry)
    return;
  free(entry->sql);
  entry->sql = NULL;
  entry->timestamp = 0;
  entry->type = HISTORY_TYPE_QUERY;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

QueryHistory *history_create(const char *connection_id) {
  QueryHistory *history = calloc(1, sizeof(QueryHistory));
  if (!history)
    return NULL;

  if (connection_id) {
    history->connection_id = str_dup(connection_id);
    if (!history->connection_id) {
      free(history);
      return NULL;
    }
  }

  return history;
}

void history_free(QueryHistory *history) {
  if (!history)
    return;

  /* Free all entries */
  for (size_t i = 0; i < history->num_entries; i++) {
    entry_free(&history->entries[i]);
  }
  free(history->entries);
  free(history->connection_id);
  free(history);
}

/* ============================================================================
 * Operations
 * ============================================================================
 */

void history_add(QueryHistory *history, const char *sql, HistoryEntryType type,
                 int max_size) {
  if (!history || !sql || !sql[0])
    return;

  /* Ensure we have capacity */
  if (!history_ensure_capacity(history, history->num_entries + 1))
    return;

  /* Trim oldest entries if we're at max */
  if (max_size > 0 && history->num_entries >= (size_t)max_size) {
    /* Free oldest entry */
    entry_free(&history->entries[0]);

    /* Shift all entries down */
    memmove(&history->entries[0], &history->entries[1],
            (history->num_entries - 1) * sizeof(HistoryEntry));
    history->num_entries--;
  }

  /* Add new entry at end (newest) */
  HistoryEntry *entry = &history->entries[history->num_entries];
  entry->sql = str_dup(sql);
  if (!entry->sql)
    return;

  entry->timestamp = time(NULL);
  entry->type = type;
  history->num_entries++;
}

void history_remove(QueryHistory *history, size_t index) {
  if (!history || index >= history->num_entries)
    return;

  /* Free the entry */
  entry_free(&history->entries[index]);

  /* Shift remaining entries */
  if (index < history->num_entries - 1) {
    memmove(&history->entries[index], &history->entries[index + 1],
            (history->num_entries - index - 1) * sizeof(HistoryEntry));
  }

  history->num_entries--;
}

void history_clear(QueryHistory *history) {
  if (!history)
    return;

  for (size_t i = 0; i < history->num_entries; i++) {
    entry_free(&history->entries[i]);
  }
  history->num_entries = 0;
}

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
    set_error(error, "Failed to get data directory");
    return false;
  }

  char *history_dir =
      str_printf("%s%s%s", data_dir, LACE_PATH_SEP_STR, HISTORY_DIR);
  if (!history_dir) {
    set_error(error, "Out of memory");
    return false;
  }

  if (!platform_dir_exists(history_dir)) {
    if (!platform_mkdir(history_dir)) {
      set_error(error, "Failed to create history directory: %s", history_dir);
      free(history_dir);
      return false;
    }
  }

  free(history_dir);
  return true;
}

static const char *type_to_string(HistoryEntryType type) {
  switch (type) {
  case HISTORY_TYPE_QUERY:
    return "query";
  case HISTORY_TYPE_SELECT:
    return "select";
  case HISTORY_TYPE_UPDATE:
    return "update";
  case HISTORY_TYPE_DELETE:
    return "delete";
  case HISTORY_TYPE_INSERT:
    return "insert";
  case HISTORY_TYPE_DDL:
    return "ddl";
  default:
    return "query";
  }
}

static HistoryEntryType string_to_type(const char *str) {
  if (!str)
    return HISTORY_TYPE_QUERY;
  if (strcmp(str, "select") == 0)
    return HISTORY_TYPE_SELECT;
  if (strcmp(str, "update") == 0)
    return HISTORY_TYPE_UPDATE;
  if (strcmp(str, "delete") == 0)
    return HISTORY_TYPE_DELETE;
  if (strcmp(str, "insert") == 0)
    return HISTORY_TYPE_INSERT;
  if (strcmp(str, "ddl") == 0)
    return HISTORY_TYPE_DDL;
  return HISTORY_TYPE_QUERY;
}

bool history_load(QueryHistory *history, char **error) {
  if (!history || !history->connection_id) {
    set_error(error, "Invalid history object");
    return false;
  }

  /* Clear existing entries */
  history_clear(history);

  char *path = history_get_file_path(history->connection_id);
  if (!path) {
    set_error(error, "Failed to get history file path");
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
    set_error(error, "Failed to open %s: %s", path, strerror(errno));
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

  char *content = malloc((size_t)size + 1);
  if (!content) {
    fclose(f);
    free(path);
    set_error(error, "Out of memory");
    return false;
  }

  size_t read_size = fread(content, 1, (size_t)size, f);
  fclose(f);
  content[read_size] = '\0';

  /* Parse JSON */
  cJSON *json = cJSON_Parse(content);
  free(content);

  if (!json) {
    set_error(error, "Failed to parse history JSON: %s", path);
    free(path);
    return false;
  }

  free(path);

  /* Parse entries array */
  cJSON *entries = cJSON_GetObjectItem(json, "entries");
  if (!cJSON_IsArray(entries)) {
    cJSON_Delete(json);
    return true; /* No entries is valid */
  }

  size_t count = (size_t)cJSON_GetArraySize(entries);
  if (count > 0) {
    if (!history_ensure_capacity(history, count)) {
      cJSON_Delete(json);
      set_error(error, "Out of memory");
      return false;
    }

    cJSON *entry_json;
    cJSON_ArrayForEach(entry_json, entries) {
      cJSON *sql_item = cJSON_GetObjectItem(entry_json, "sql");
      cJSON *ts_item = cJSON_GetObjectItem(entry_json, "timestamp");
      cJSON *type_item = cJSON_GetObjectItem(entry_json, "type");

      if (!cJSON_IsString(sql_item))
        continue;

      HistoryEntry *entry = &history->entries[history->num_entries];
      entry->sql = str_dup(sql_item->valuestring);
      if (!entry->sql)
        continue;

      entry->timestamp =
          cJSON_IsNumber(ts_item) ? (time_t)ts_item->valuedouble : time(NULL);
      entry->type = string_to_type(
          cJSON_IsString(type_item) ? type_item->valuestring : NULL);

      history->num_entries++;
    }
  }

  cJSON_Delete(json);
  return true;
}

bool history_save(const QueryHistory *history, char **error) {
  if (!history || !history->connection_id) {
    set_error(error, "Invalid history object");
    return false;
  }

  /* Ensure directory exists */
  if (!history_ensure_dir(error))
    return false;

  char *path = history_get_file_path(history->connection_id);
  if (!path) {
    set_error(error, "Failed to get history file path");
    return false;
  }

  /* Build JSON */
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    free(path);
    set_error(error, "Failed to create JSON object");
    return false;
  }

  cJSON_AddNumberToObject(json, "version", HISTORY_VERSION);
  cJSON_AddStringToObject(json, "connection_id", history->connection_id);

  cJSON *entries = cJSON_CreateArray();
  if (!entries) {
    cJSON_Delete(json);
    free(path);
    set_error(error, "Failed to create entries array");
    return false;
  }

  for (size_t i = 0; i < history->num_entries; i++) {
    const HistoryEntry *entry = &history->entries[i];
    if (!entry->sql)
      continue;

    cJSON *entry_json = cJSON_CreateObject();
    if (!entry_json)
      continue;

    cJSON_AddStringToObject(entry_json, "sql", entry->sql);
    cJSON_AddNumberToObject(entry_json, "timestamp", (double)entry->timestamp);
    cJSON_AddStringToObject(entry_json, "type", type_to_string(entry->type));

    cJSON_AddItemToArray(entries, entry_json);
  }

  cJSON_AddItemToObject(json, "entries", entries);

  /* Write to file */
  char *content = cJSON_Print(json);
  cJSON_Delete(json);

  if (!content) {
    free(path);
    set_error(error, "Failed to serialize JSON");
    return false;
  }

  FILE *f = fopen(path, "w");
  if (!f) {
    free(content);
    set_error(error, "Failed to open %s for writing: %s", path,
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
    set_error(error, "Failed to write all data");
    return false;
  }

  return true;
}

/* ============================================================================
 * Utilities
 * ============================================================================
 */

const char *history_type_name(HistoryEntryType type) {
  switch (type) {
  case HISTORY_TYPE_QUERY:
    return "Query";
  case HISTORY_TYPE_SELECT:
    return "Select";
  case HISTORY_TYPE_UPDATE:
    return "Update";
  case HISTORY_TYPE_DELETE:
    return "Delete";
  case HISTORY_TYPE_INSERT:
    return "Insert";
  case HISTORY_TYPE_DDL:
    return "DDL";
  default:
    return "Query";
  }
}

const char *history_type_tag(HistoryEntryType type) {
  switch (type) {
  case HISTORY_TYPE_QUERY:
    return "QRY";
  case HISTORY_TYPE_SELECT:
    return "SEL";
  case HISTORY_TYPE_UPDATE:
    return "UPD";
  case HISTORY_TYPE_DELETE:
    return "DEL";
  case HISTORY_TYPE_INSERT:
    return "INS";
  case HISTORY_TYPE_DDL:
    return "DDL";
  default:
    return "QRY";
  }
}

HistoryEntryType history_detect_type(const char *sql) {
  if (!sql)
    return HISTORY_TYPE_QUERY;

  /* Skip whitespace */
  while (*sql && isspace((unsigned char)*sql))
    sql++;

  if (!*sql)
    return HISTORY_TYPE_QUERY;

  /* Check first keyword (case insensitive) */
  if (strncasecmp(sql, "SELECT", 6) == 0 &&
      (sql[6] == '\0' || isspace((unsigned char)sql[6])))
    return HISTORY_TYPE_SELECT;

  if (strncasecmp(sql, "UPDATE", 6) == 0 &&
      (sql[6] == '\0' || isspace((unsigned char)sql[6])))
    return HISTORY_TYPE_UPDATE;

  if (strncasecmp(sql, "DELETE", 6) == 0 &&
      (sql[6] == '\0' || isspace((unsigned char)sql[6])))
    return HISTORY_TYPE_DELETE;

  if (strncasecmp(sql, "INSERT", 6) == 0 &&
      (sql[6] == '\0' || isspace((unsigned char)sql[6])))
    return HISTORY_TYPE_INSERT;

  if (strncasecmp(sql, "CREATE", 6) == 0 || strncasecmp(sql, "ALTER", 5) == 0 ||
      strncasecmp(sql, "DROP", 4) == 0 || strncasecmp(sql, "TRUNCATE", 8) == 0)
    return HISTORY_TYPE_DDL;

  return HISTORY_TYPE_QUERY;
}
