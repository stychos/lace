/*
 * liblace - Lace Client Library
 * SQL Query History - core types and operations
 *
 * This module provides the core history management functionality.
 * File persistence is handled by the frontend (TUI/GUI) which has
 * access to platform-specific code.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_HISTORY_H
#define LIBLACE_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ==========================================================================
 * History Configuration Constants
 * ========================================================================== */

/* History mode values */
#define LACE_HISTORY_MODE_OFF 0
#define LACE_HISTORY_MODE_SESSION 1
#define LACE_HISTORY_MODE_PERSISTENT 2

/* History size limits */
#define LACE_HISTORY_SIZE_MIN 10
#define LACE_HISTORY_SIZE_MAX 10000
#define LACE_HISTORY_SIZE_DEFAULT 500

/* Initial capacity for history array */
#define LACE_HISTORY_INITIAL_CAPACITY 64

/* ==========================================================================
 * History Entry Types
 * ========================================================================== */

typedef enum {
  LACE_HISTORY_QUERY,  /* Manual query from editor */
  LACE_HISTORY_SELECT, /* Table open/refresh */
  LACE_HISTORY_UPDATE, /* Cell edit */
  LACE_HISTORY_DELETE, /* Row delete */
  LACE_HISTORY_INSERT, /* Row insert */
  LACE_HISTORY_DDL     /* CREATE/ALTER/DROP */
} LaceHistoryType;

/* ==========================================================================
 * History Entry
 * ========================================================================== */

typedef struct {
  char *sql;              /* SQL statement (heap allocated) */
  time_t timestamp;       /* When the query was executed */
  LaceHistoryType type;   /* Type of query */
} LaceHistoryEntry;

/* ==========================================================================
 * History Collection
 * ========================================================================== */

typedef struct {
  char *connection_id;        /* UUID of connection (heap allocated) */
  LaceHistoryEntry *entries;  /* Array of entries */
  size_t num_entries;         /* Number of entries */
  size_t capacity;            /* Allocated capacity */
} LaceHistory;

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

/* Create a new history for a connection
 * connection_id: UUID of the connection (can be NULL)
 * Returns: allocated history, or NULL on allocation failure */
LaceHistory *lace_history_create(const char *connection_id);

/* Free history and all entries */
void lace_history_free(LaceHistory *history);

/* ==========================================================================
 * Operations
 * ========================================================================== */

/* Add an entry to history
 * history: history collection
 * sql: SQL statement to add
 * type: type of query
 * max_size: maximum entries (oldest trimmed if exceeded), 0 = unlimited */
void lace_history_add(LaceHistory *history, const char *sql,
                      LaceHistoryType type, int max_size);

/* Remove entry at index */
void lace_history_remove(LaceHistory *history, size_t index);

/* Clear all entries (keeps allocated memory) */
void lace_history_clear(LaceHistory *history);

/* Get entry at index (returns NULL if out of bounds) */
const LaceHistoryEntry *lace_history_get(const LaceHistory *history,
                                         size_t index);

/* Get number of entries */
size_t lace_history_count(const LaceHistory *history);

/* ==========================================================================
 * Utilities
 * ========================================================================== */

/* Get human-readable name for entry type */
const char *lace_history_type_name(LaceHistoryType type);

/* Get short tag for entry type (for display) */
const char *lace_history_type_tag(LaceHistoryType type);

/* Detect entry type from SQL string */
LaceHistoryType lace_history_detect_type(const char *sql);

/* Convert type to string for serialization */
const char *lace_history_type_to_string(LaceHistoryType type);

/* Parse type from string */
LaceHistoryType lace_history_type_from_string(const char *str);

#endif /* LIBLACE_HISTORY_H */
