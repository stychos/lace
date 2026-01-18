/*
 * liblace - Lace Client Library
 * SQL Query History - core implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../include/config/history.h"
#include "../../include/util/mem.h"
#include "../../include/util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/* Ensure capacity for entries array */
static void history_ensure_capacity(LaceHistory *history, size_t needed) {
  if (!history || history->capacity >= needed)
    return;

  size_t new_cap =
      history->capacity == 0 ? LACE_HISTORY_INITIAL_CAPACITY : history->capacity;
  while (new_cap < needed) {
    new_cap *= 2;
  }

  size_t old_cap = history->capacity;
  history->entries =
      safe_reallocarray(history->entries, new_cap, sizeof(LaceHistoryEntry));
  memset(&history->entries[old_cap], 0,
         (new_cap - old_cap) * sizeof(LaceHistoryEntry));
  history->capacity = new_cap;
}

/* Free a single entry's contents */
static void entry_free(LaceHistoryEntry *entry) {
  if (!entry)
    return;
  FREE_NULL(entry->sql);
  entry->timestamp = 0;
  entry->type = LACE_HISTORY_QUERY;
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

LaceHistory *lace_history_create(const char *connection_id) {
  LaceHistory *history = safe_calloc(1, sizeof(LaceHistory));
  if (!history)
    return NULL;

  if (connection_id) {
    history->connection_id = str_dup(connection_id);
  }

  return history;
}

void lace_history_free(LaceHistory *history) {
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

/* ==========================================================================
 * Operations
 * ========================================================================== */

void lace_history_add(LaceHistory *history, const char *sql,
                      LaceHistoryType type, int max_size) {
  if (!history || !sql || !sql[0])
    return;

  /* Ensure we have capacity */
  history_ensure_capacity(history, history->num_entries + 1);

  char *new_sql = str_dup(sql);
  if (!new_sql)
    return;

  /* Trim oldest entries if we're at max */
  if (max_size > 0 && history->num_entries >= (size_t)max_size) {
    /* Free oldest entry */
    entry_free(&history->entries[0]);

    /* Shift all entries down */
    memmove(&history->entries[0], &history->entries[1],
            (history->num_entries - 1) * sizeof(LaceHistoryEntry));
    history->num_entries--;
  }

  /* Add new entry at end (newest) */
  LaceHistoryEntry *entry = &history->entries[history->num_entries];
  entry->sql = new_sql;
  entry->timestamp = time(NULL);
  entry->type = type;
  history->num_entries++;
}

void lace_history_remove(LaceHistory *history, size_t index) {
  if (!history || index >= history->num_entries)
    return;

  /* Free the entry */
  entry_free(&history->entries[index]);

  /* Shift remaining entries */
  if (index < history->num_entries - 1) {
    memmove(&history->entries[index], &history->entries[index + 1],
            (history->num_entries - index - 1) * sizeof(LaceHistoryEntry));
  }

  history->num_entries--;
}

void lace_history_clear(LaceHistory *history) {
  if (!history)
    return;

  for (size_t i = 0; i < history->num_entries; i++) {
    entry_free(&history->entries[i]);
  }
  history->num_entries = 0;
}

const LaceHistoryEntry *lace_history_get(const LaceHistory *history,
                                         size_t index) {
  if (!history || index >= history->num_entries)
    return NULL;
  return &history->entries[index];
}

size_t lace_history_count(const LaceHistory *history) {
  return history ? history->num_entries : 0;
}

/* ==========================================================================
 * Utilities
 * ========================================================================== */

const char *lace_history_type_name(LaceHistoryType type) {
  switch (type) {
  case LACE_HISTORY_QUERY:
    return "Query";
  case LACE_HISTORY_SELECT:
    return "Select";
  case LACE_HISTORY_UPDATE:
    return "Update";
  case LACE_HISTORY_DELETE:
    return "Delete";
  case LACE_HISTORY_INSERT:
    return "Insert";
  case LACE_HISTORY_DDL:
    return "DDL";
  default:
    return "Query";
  }
}

const char *lace_history_type_tag(LaceHistoryType type) {
  switch (type) {
  case LACE_HISTORY_QUERY:
    return "QRY";
  case LACE_HISTORY_SELECT:
    return "SEL";
  case LACE_HISTORY_UPDATE:
    return "UPD";
  case LACE_HISTORY_DELETE:
    return "DEL";
  case LACE_HISTORY_INSERT:
    return "INS";
  case LACE_HISTORY_DDL:
    return "DDL";
  default:
    return "QRY";
  }
}

const char *lace_history_type_to_string(LaceHistoryType type) {
  switch (type) {
  case LACE_HISTORY_QUERY:
    return "query";
  case LACE_HISTORY_SELECT:
    return "select";
  case LACE_HISTORY_UPDATE:
    return "update";
  case LACE_HISTORY_DELETE:
    return "delete";
  case LACE_HISTORY_INSERT:
    return "insert";
  case LACE_HISTORY_DDL:
    return "ddl";
  default:
    return "query";
  }
}

LaceHistoryType lace_history_type_from_string(const char *str) {
  if (!str)
    return LACE_HISTORY_QUERY;
  if (strcmp(str, "select") == 0)
    return LACE_HISTORY_SELECT;
  if (strcmp(str, "update") == 0)
    return LACE_HISTORY_UPDATE;
  if (strcmp(str, "delete") == 0)
    return LACE_HISTORY_DELETE;
  if (strcmp(str, "insert") == 0)
    return LACE_HISTORY_INSERT;
  if (strcmp(str, "ddl") == 0)
    return LACE_HISTORY_DDL;
  return LACE_HISTORY_QUERY;
}

LaceHistoryType lace_history_detect_type(const char *sql) {
  if (!sql)
    return LACE_HISTORY_QUERY;

  /* Skip whitespace */
  while (*sql && isspace((unsigned char)*sql))
    sql++;

  if (!*sql)
    return LACE_HISTORY_QUERY;

  /* Check first keyword (case insensitive) */
  if (strncasecmp(sql, "SELECT", 6) == 0 &&
      (sql[6] == '\0' || isspace((unsigned char)sql[6])))
    return LACE_HISTORY_SELECT;

  if (strncasecmp(sql, "UPDATE", 6) == 0 &&
      (sql[6] == '\0' || isspace((unsigned char)sql[6])))
    return LACE_HISTORY_UPDATE;

  if (strncasecmp(sql, "DELETE", 6) == 0 &&
      (sql[6] == '\0' || isspace((unsigned char)sql[6])))
    return LACE_HISTORY_DELETE;

  if (strncasecmp(sql, "INSERT", 6) == 0 &&
      (sql[6] == '\0' || isspace((unsigned char)sql[6])))
    return LACE_HISTORY_INSERT;

  if (strncasecmp(sql, "CREATE", 6) == 0 || strncasecmp(sql, "ALTER", 5) == 0 ||
      strncasecmp(sql, "DROP", 4) == 0 || strncasecmp(sql, "TRUNCATE", 8) == 0)
    return LACE_HISTORY_DDL;

  return LACE_HISTORY_QUERY;
}
