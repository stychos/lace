/*
 * Lace
 * SQL Query History - per-connection history of executed queries
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_HISTORY_H
#define LACE_HISTORY_H

#include "constants.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* History mode configuration values */
#define HISTORY_MODE_OFF 0
#define HISTORY_MODE_SESSION 1
#define HISTORY_MODE_PERSISTENT 2

/* History limits and initial capacity are in constants.h:
 * HISTORY_SIZE_MIN, HISTORY_SIZE_MAX, HISTORY_SIZE_DEFAULT,
 * HISTORY_INITIAL_CAPACITY */

/* History entry types */
typedef enum {
  HISTORY_TYPE_QUERY,  /* Manual query from editor */
  HISTORY_TYPE_SELECT, /* Table open/refresh */
  HISTORY_TYPE_UPDATE, /* Cell edit */
  HISTORY_TYPE_DELETE, /* Row delete */
  HISTORY_TYPE_INSERT, /* Row insert */
  HISTORY_TYPE_DDL     /* CREATE/ALTER/DROP */
} HistoryEntryType;

/* Single history entry */
typedef struct {
  char *sql;
  time_t timestamp;
  HistoryEntryType type;
} HistoryEntry;

/* History for a connection */
typedef struct QueryHistory {
  char *connection_id; /* UUID of connection */
  HistoryEntry *entries;
  size_t num_entries;
  size_t capacity;
} QueryHistory;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create a new history for a connection */
QueryHistory *history_create(const char *connection_id);

/* Free history and all entries */
void history_free(QueryHistory *history);

/* ============================================================================
 * Operations
 * ============================================================================
 */

/* Add an entry to history (trims oldest if over max_size) */
void history_add(QueryHistory *history, const char *sql, HistoryEntryType type,
                 int max_size);

/* Remove entry at index */
void history_remove(QueryHistory *history, size_t index);

/* Clear all entries */
void history_clear(QueryHistory *history);

/* ============================================================================
 * Persistence
 * ============================================================================
 */

/* Load history from file (returns false on error, history is cleared) */
bool history_load(QueryHistory *history, char **error);

/* Save history to file */
bool history_save(const QueryHistory *history, char **error);

/* Get file path for history storage */
char *history_get_file_path(const char *connection_id);

/* Ensure history directory exists */
bool history_ensure_dir(char **error);

/* ============================================================================
 * Utilities
 * ============================================================================
 */

/* Get human-readable name for entry type */
const char *history_type_name(HistoryEntryType type);

/* Get short tag for entry type (for display) */
const char *history_type_tag(HistoryEntryType type);

/* Detect entry type from SQL string */
HistoryEntryType history_detect_type(const char *sql);

#endif /* LACE_HISTORY_H */
