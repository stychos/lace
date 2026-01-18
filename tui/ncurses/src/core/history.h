/*
 * Lace TUI
 * SQL Query History - per-connection history of executed queries
 *
 * Uses liblace history types with TUI-specific persistence.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_HISTORY_H
#define LACE_TUI_HISTORY_H

#include "../../../liblace/include/config/history.h"

/* ==========================================================================
 * Type Aliases - Map TUI names to liblace types
 * ========================================================================== */

typedef LaceHistoryType HistoryEntryType;
typedef LaceHistoryEntry HistoryEntry;
typedef LaceHistory QueryHistory;

/* Entry type enum aliases */
#define HISTORY_TYPE_QUERY  LACE_HISTORY_QUERY
#define HISTORY_TYPE_SELECT LACE_HISTORY_SELECT
#define HISTORY_TYPE_UPDATE LACE_HISTORY_UPDATE
#define HISTORY_TYPE_DELETE LACE_HISTORY_DELETE
#define HISTORY_TYPE_INSERT LACE_HISTORY_INSERT
#define HISTORY_TYPE_DDL    LACE_HISTORY_DDL

/* History mode aliases */
#define HISTORY_MODE_OFF        LACE_HISTORY_MODE_OFF
#define HISTORY_MODE_SESSION    LACE_HISTORY_MODE_SESSION
#define HISTORY_MODE_PERSISTENT LACE_HISTORY_MODE_PERSISTENT

/* ==========================================================================
 * Function Aliases - Map TUI names to liblace functions
 * ========================================================================== */

#define history_create(conn_id)         lace_history_create(conn_id)
#define history_free(h)                 lace_history_free(h)
#define history_add(h, sql, type, max)  lace_history_add(h, sql, type, max)
#define history_remove(h, idx)          lace_history_remove(h, idx)
#define history_clear(h)                lace_history_clear(h)
#define history_type_name(t)            lace_history_type_name(t)
#define history_type_tag(t)             lace_history_type_tag(t)
#define history_detect_type(sql)        lace_history_detect_type(sql)

/* ==========================================================================
 * TUI-Specific Persistence Functions
 * These are implemented in history.c and provide file persistence
 * ========================================================================== */

/* Load history from file (returns false on error, history is cleared) */
bool history_load(QueryHistory *history, char **error);

/* Save history to file */
bool history_save(const QueryHistory *history, char **error);

/* Get file path for history storage */
char *history_get_file_path(const char *connection_id);

/* Ensure history directory exists */
bool history_ensure_dir(char **error);

#endif /* LACE_TUI_HISTORY_H */
